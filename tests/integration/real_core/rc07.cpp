/// @file
/// @brief Checks all supported native root modules against the fixed Xray
/// executable.

#include <catch2/catch_test_macros.hpp>

#include "infra/store.hpp"
#include "model/module_catalog.hpp"
#include "profile/profile_service.hpp"
#include "xray/runtime_assembler.hpp"
#include "xray/runtime_writer.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

extern char **environ;

namespace {
using namespace std::chrono_literals;
using namespace xrm;

std::filesystem::path test_xray() {
  if (const char *value = std::getenv("XRM_TEST_XRAY"); value && *value)
    return std::filesystem::path(value);
#ifdef XRM_TEST_XRAY_CONFIGURED
  return XRM_TEST_XRAY_CONFIGURED;
#else
  return {};
#endif
}

class TempDirectory {
public:
  TempDirectory() {
    path_ = std::filesystem::temp_directory_path() /
            ("xrm-rc07-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path_);
  }
  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

std::string read_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::uint16_t free_loopback_port() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return 0;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) !=
      0) {
    ::close(fd);
    return 0;
  }
  socklen_t size = sizeof(address);
  if (::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &size) != 0) {
    ::close(fd);
    return 0;
  }
  const auto port = ntohs(address.sin_port);
  ::close(fd);
  return port;
}

struct CommandResult {
  int code = -1;
  std::string out;
  std::string err;
};

CommandResult run_command(const std::filesystem::path &executable,
                          const std::vector<std::string> &arguments,
                          const std::filesystem::path &root) {
  static std::atomic<unsigned> sequence{0};
  const auto number = sequence.fetch_add(1);
  const auto out_path = root / ("command-" + std::to_string(number) + ".out");
  const auto err_path = root / ("command-" + std::to_string(number) + ".err");
  std::vector<std::string> values{executable.string()};
  values.insert(values.end(), arguments.begin(), arguments.end());
  std::vector<char *> argv;
  for (auto &value : values)
    argv.push_back(value.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, out_path.c_str(),
                                   O_CREAT | O_TRUNC | O_WRONLY, 0600);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, err_path.c_str(),
                                   O_CREAT | O_TRUNC | O_WRONLY, 0600);
  pid_t pid = -1;
  const int spawned = posix_spawn(&pid, executable.c_str(), &actions, nullptr,
                                  argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (spawned != 0)
    return {};
  const auto deadline = std::chrono::steady_clock::now() + 20s;
  int status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto waited = ::waitpid(pid, &status, WNOHANG);
    if (waited == pid) {
      return {WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status),
              read_file(out_path), read_file(err_path)};
    }
    if (waited < 0)
      break;
    ::poll(nullptr, 0, 10);
  }
  ::kill(pid, SIGKILL);
  ::waitpid(pid, &status, 0);
  return {-1, read_file(out_path), read_file(err_path)};
}

bool save_module(ProfileService &profiles, const Id &profile, std::string key,
                 const Json &document) {
  const auto previous = profiles.module(profile, key);
  if (!previous)
    return false;
  const auto base =
      previous->document
          ? std::optional<std::string>(previous->document->source_text)
          : std::nullopt;
  if (!profiles.save_module(profile, {key, {base}, document.dump(2) + "\n"}))
    return false;
  return static_cast<bool>(profiles.include_module(profile, key, true));
}

struct ModuleCase {
  std::string key;
  Json value;
  std::vector<std::pair<std::string, Json>> dependencies;
};

std::vector<ModuleCase> cases() {
  const auto inbound_port = free_loopback_port();
  const auto api_port = free_loopback_port();
  const auto metrics_port = free_loopback_port();
  const Json direct{
      {"protocol", "freedom"}, {"tag", "direct"}, {"settings", Json::object()}};
  const Json outbounds{{"outbounds", Json::array({direct})}};
  const Json api_inbounds{
      {"inbounds",
       Json::array({Json{{"listen", "127.0.0.1"},
                         {"port", api_port},
                         {"protocol", "dokodemo-door"},
                         {"tag", "rc07-api"},
                         {"settings", Json{{"address", "127.0.0.1"}}}}})}};
  const Json api_routing{
      {"routing",
       Json{{"rules",
             Json::array({Json{{"type", "field"},
                               {"inboundTag", Json::array({"rc07-api"})},
                               {"outboundTag", "rc07-api"}}})}}}};
  std::vector<ModuleCase> result;
  result.push_back({"log", Json{{"log", Json{{"loglevel", "warning"}}}}, {}});
  result.push_back(
      {"dns",
       Json{{"dns", Json{{"servers", Json::array({"127.0.0.1"})}}}},
       {}});
  result.push_back(
      {"inbounds",
       Json{{"inbounds",
             Json::array({Json{
                 {"listen", "127.0.0.1"},
                 {"port", inbound_port},
                 {"protocol", "dokodemo-door"},
                 {"tag", "rc07-in"},
                 {"settings", Json{{"address", "127.0.0.1"}, {"port", 9}}}}})}},
       {}});
  result.push_back({"outbounds", outbounds, {}});
  result.push_back(
      {"routing",
       Json{{"routing",
             Json{{"domainStrategy", "AsIs"},
                  {"rules", Json::array({Json{{"type", "field"},
                                              {"network", "tcp,udp"},
                                              {"outboundTag", "direct"}}})}}}},
       {}});
  result.push_back(
      {"observatory",
       Json{{"observatory", Json{{"subjectSelector", Json::array({"direct"})},
                                 {"probeUrl", "http://127.0.0.1:9/rc07"},
                                 {"probeInterval", "1m"}}}},
       {}});
  result.push_back(
      {"api",
       Json{{"api",
             Json{{"tag", "rc07-api"},
                  {"services", Json::array({"HandlerService", "LoggerService",
                                            "StatsService"})}}}},
       {{"inbounds", api_inbounds}, {"routing", api_routing}}});
  result.push_back(
      {"metrics",
       Json{{"metrics",
             Json{{"listen", "127.0.0.1:" + std::to_string(metrics_port)}}}},
       {}});
  result.push_back({"stats", Json{{"stats", Json::object()}}, {}});
  Json policy = Json::object();
  policy["policy"]["levels"]["0"] =
      Json{{"handshake", 4}, {"connIdle", 300}, {"rc07UserLevel", "preserved"}};
  policy["policy"]["system"] = Json{{"statsInboundUplink", true},
                                    {"statsInboundDownlink", true},
                                    {"statsOutboundUplink", true},
                                    {"statsOutboundDownlink", true},
                                    {"rc07SystemField", "preserved"}};
  result.push_back({"policy", std::move(policy), {}});
  result.push_back({"fakedns",
                    Json{{"fakedns", Json{{"ipPool", "198.18.0.0/15"},
                                          {"poolSize", 65535}}}},
                    {}});
  result.push_back(
      {"fakedns",
       Json{{"fakedns",
             Json::array(
                 {Json{{"ipPool", "198.18.0.0/15"}, {"poolSize", 65535}},
                  Json{{"ipPool", "198.19.0.0/16"}, {"poolSize", 8192}}})}},
       {}});
  result.push_back({"geodata", Json{{"geodata", Json::object()}}, {}});
  result.push_back(
      {"env", Json{{"env", Json{{"XRM_RC07_FIXTURE", "isolated"}}}}, {}});
  result.push_back(
      {"version",
       Json{{"version", Json{{"min", "26.9.9"}, {"max", "26.9.9"}}}},
       {}});
  Json burst = Json::object();
  burst["burstObservatory"]["subjectSelector"] = Json::array({"direct"});
  burst["burstObservatory"]["pingConfig"] =
      Json{{"destination", "http://127.0.0.1:9/rc07"},
           {"connectivity", "http://127.0.0.1:9/rc07"},
           {"interval", "1m"},
           {"sampling", 1},
           {"timeout", "1s"}};
  result.push_back({"burstObservatory", std::move(burst), {}});
  return result;
}

} // namespace

TEST_CASE("RC-07 fixed Xray recognizes each native root module in isolated "
          "legal contexts",
          "[real_core][RC-07]") {
  const auto xray = test_xray();
  if (xray.empty() || !std::filesystem::exists(xray))
    SKIP("set XRM_TEST_XRAY to the fixed real Xray v26.9.9 binary");

  TempDirectory temp;
  const auto version = run_command(xray, {"version"}, temp.path());
  REQUIRE(version.code == 0);
  INFO("Xray version stdout: " << version.out);
  REQUIRE(version.out.find("26.9.9") != std::string::npos);
  REQUIRE(version.out.find("52a412d") != std::string::npos);

  auto opened = Store::open(temp.path() / "state.sqlite");
  REQUIRE(opened);
  ProfileService profiles(**opened, temp.path() / "profiles");
  RuntimeWriter writer(temp.path() / "runtime");
  std::size_t index = 0;
  for (const auto &item : cases()) {
    SECTION(("native root " + item.key + " fixture " + std::to_string(index))
                .c_str()) {
      const auto profile =
          profiles.create({"RC-07 " + item.key + " " + std::to_string(index),
                           "test-core",
                           "local",
                           {}});
      REQUIRE(profile);
      auto profile_settings = profiles.get(profile->id);
      REQUIRE(profile_settings);
      profile_settings->append_selected_default = false;
      const auto profile_file =
          temp.path() / "profiles" / profile->id / "profile.json";
      REQUIRE(profiles.save(profile->id,
                            {{read_file(profile_file)}, *profile_settings}));
      if (item.key != "outbounds") {
        REQUIRE(save_module(
            profiles, profile->id, "outbounds",
            Json{{"outbounds",
                  Json::array({Json{{"protocol", "freedom"},
                                    {"tag", "direct"},
                                    {"settings", Json::object()}}})}}));
      }
      for (const auto &[key, value] : item.dependencies)
        REQUIRE(save_module(profiles, profile->id, key, value));
      REQUIRE(save_module(profiles, profile->id, item.key, item.value));

      const auto descriptor = find_module(item.key);
      REQUIRE(descriptor != nullptr);
      const auto source = profiles.module(profile->id, item.key);
      REQUIRE(source);
      REQUIRE(source->setting.included);
      REQUIRE(source->document);
      CHECK(source->document->filename == descriptor->default_filename);
      CHECK(std::filesystem::exists(temp.path() / "profiles" / profile->id /
                                    "modules" / descriptor->default_filename));

      auto snapshot = profiles.snapshot(profile->id);
      REQUIRE(snapshot);
      auto prepared = assemble(BuildInput{std::move(*snapshot), {}});
      const std::string assembly_error =
          prepared ? std::string{} : prepared.error().message;
      INFO("RuntimeAssembler failed: " << assembly_error);
      REQUIRE(prepared);
      const auto expected_file = descriptor->default_filename;
      const auto output =
          std::find_if(prepared->files.begin(), prepared->files.end(),
                       [&](const OutputFile &file) {
                         return file.relative_name == expected_file;
                       });
      REQUIRE(output != prepared->files.end());
      const auto emitted = Json::parse(output->bytes);
      REQUIRE(emitted.is_object());
      REQUIRE(emitted.contains(item.key));
      CHECK(emitted.at(item.key) == item.value.at(item.key));

      const auto directory =
          writer.materialize(*prepared, DirectoryPurpose::candidate,
                             "rc07-" + std::to_string(index));
      REQUIRE(directory);
      const auto disk_module = directory->confdir / expected_file;
      REQUIRE(std::filesystem::exists(disk_module));
      CHECK(Json::parse(read_file(disk_module)).at(item.key) ==
            item.value.at(item.key));

      const auto checked = run_command(
          xray, {"run", "-test", "-confdir", directory->confdir.string()},
          temp.path());
      INFO("root key: " << item.key << "; confdir: " << directory->confdir
                        << "; xray stdout: " << checked.out
                        << "; xray stderr: " << checked.err);
      CHECK(checked.code == 0);
      ++index;
    }
  }
}
