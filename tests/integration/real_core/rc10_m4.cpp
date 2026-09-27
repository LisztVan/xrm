/// @file
/// @brief Checks advanced native modules and exported confdir input with real
/// Xray.

#include <catch2/catch_test_macros.hpp>

#include "infra/store.hpp"
#include "model/module_catalog.hpp"
#include "xray/codecs/codec.hpp"
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
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

extern char **environ;

namespace {
using namespace std::chrono_literals;
using namespace xrm;

std::filesystem::path real_xray() {
  if (const char *value = std::getenv("XRM_TEST_XRAY"); value && *value)
    return std::filesystem::path(value);
#ifdef XRM_TEST_XRAY_CONFIGURED
  return XRM_TEST_XRAY_CONFIGURED;
#else
  return {};
#endif
}

struct TempDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("xrm-real-core-rc10-m4-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  TempDirectory() { std::filesystem::create_directories(path); }
  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

std::string read_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

struct CommandResult {
  int code = -1;
  std::string out;
  std::string err;
};

CommandResult
run_command(const std::filesystem::path &executable,
            const std::vector<std::string> &arguments,
            const std::filesystem::path &cwd,
            const std::map<std::string, std::string> &environment) {
  static std::atomic<unsigned> sequence{0};
  const auto number = sequence.fetch_add(1);
  const auto out_path = cwd / ("rc10-m4-" + std::to_string(number) + ".out");
  const auto err_path = cwd / ("rc10-m4-" + std::to_string(number) + ".err");
  std::vector<std::string> args{executable.string()};
  args.insert(args.end(), arguments.begin(), arguments.end());
  std::vector<char *> argv;
  for (auto &arg : args)
    argv.push_back(arg.data());
  argv.push_back(nullptr);
  std::vector<std::string> env_values;
  for (const auto &[key, value] : environment)
    env_values.push_back(key + "=" + value);
  std::vector<char *> envp;
  for (auto &value : env_values)
    envp.push_back(value.data());
  envp.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0)
    return {};
  const int chdir = posix_spawn_file_actions_addchdir_np(&actions, cwd.c_str());
  const int out = posix_spawn_file_actions_addopen(
      &actions, STDOUT_FILENO, out_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY,
      0600);
  const int err = posix_spawn_file_actions_addopen(
      &actions, STDERR_FILENO, err_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY,
      0600);
  if (chdir != 0 || out != 0 || err != 0) {
    posix_spawn_file_actions_destroy(&actions);
    return {};
  }
  pid_t pid = -1;
  const int spawned = posix_spawn(&pid, executable.c_str(), &actions, nullptr,
                                  argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&actions);
  if (spawned != 0)
    return {};
  int status = 0;
  const auto deadline = std::chrono::steady_clock::now() + 20s;
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

class RunningXray {
public:
  bool start(const std::filesystem::path &binary,
             const std::filesystem::path &confdir,
             const std::filesystem::path &root,
             const std::map<std::string, std::string> &environment) {
    const auto output = root / "running.stdout";
    const auto errors = root / "running.stderr";
    std::vector<std::string> args{binary.string(), "run", "-confdir",
                                  confdir.string()};
    std::vector<char *> argv;
    for (auto &arg : args)
      argv.push_back(arg.data());
    argv.push_back(nullptr);
    std::vector<std::string> env_values;
    for (const auto &[key, value] : environment)
      env_values.push_back(key + "=" + value);
    std::vector<char *> envp;
    for (auto &value : env_values)
      envp.push_back(value.data());
    envp.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0)
      return false;
    const int chdir =
        posix_spawn_file_actions_addchdir_np(&actions, root.c_str());
    const int out = posix_spawn_file_actions_addopen(
        &actions, STDOUT_FILENO, output.c_str(), O_CREAT | O_TRUNC | O_WRONLY,
        0600);
    const int err = posix_spawn_file_actions_addopen(
        &actions, STDERR_FILENO, errors.c_str(), O_CREAT | O_TRUNC | O_WRONLY,
        0600);
    if (chdir != 0 || out != 0 || err != 0) {
      posix_spawn_file_actions_destroy(&actions);
      return false;
    }
    const int spawned = posix_spawn(&pid_, binary.c_str(), &actions, nullptr,
                                    argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    return spawned == 0;
  }
  bool stop() {
    if (pid_ <= 0)
      return true;
    ::kill(pid_, SIGTERM);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
      if (::waitpid(pid_, &status, WNOHANG) == pid_) {
        pid_ = -1;
        return WIFEXITED(status) || WIFSIGNALED(status);
      }
      ::poll(nullptr, 0, 10);
    }
    ::kill(pid_, SIGKILL);
    ::waitpid(pid_, &status, 0);
    pid_ = -1;
    return false;
  }
  ~RunningXray() { (void)stop(); }

private:
  pid_t pid_ = -1;
};

std::optional<std::uint16_t> free_loopback_port() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return std::nullopt;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) !=
      0) {
    ::close(fd);
    return std::nullopt;
  }
  socklen_t size = sizeof(address);
  if (::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &size) != 0) {
    ::close(fd);
    return std::nullopt;
  }
  const auto port = ntohs(address.sin_port);
  ::close(fd);
  return port;
}

bool loopback_accepts(std::uint16_t port) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      address.sin_port = htons(port);
      if (::connect(fd, reinterpret_cast<sockaddr *>(&address),
                    sizeof(address)) == 0) {
        ::close(fd);
        return true;
      }
      ::close(fd);
    }
    ::poll(nullptr, 0, 20);
  }
  return false;
}

void enable(ProfileSnapshot &profile, std::string key, std::string filename,
            Json body) {
  profile.profile.modules.push_back(ModuleSetting{key, filename, true});
  profile.modules.push_back(
      ModuleDocument{std::move(key), std::move(filename), body.dump(2) + "\n"});
}

} // namespace

TEST_CASE("RC-10 M4 VMess and advanced native modules run from exported files "
          "without Server",
          "[real_core][RC-10]") {
  const auto binary = real_xray();
  REQUIRE_FALSE(binary.empty());
  REQUIRE(binary.is_absolute());
  REQUIRE(std::filesystem::is_regular_file(binary));

  TempDirectory temporary;
  const auto assets = temporary.path / "assets";
  std::filesystem::create_directories(assets);
  std::ofstream(assets / "rc10-fixture.dat", std::ios::binary)
      << "loopback resource\n";
  const auto port = free_loopback_port();
  REQUIRE(port.has_value());

  const std::string uri =
      "vmess://"
      "eyJ2IjoiMiIsInBzIjoiUkMxMCIsImFkZCI6IjEyNy4wLjAuMSIsInBvcnQiOiI0NDMiLCJp"
      "ZCI6IjAwMDAwMDAwLTAwMDAtNDAwMC04MDAwLTAwMDAwMDAwMDAxMCIsImFpZCI6IjAiLCJz"
      "Y3kiOiJhdXRvIiwibmV0IjoidGNwIiwidHlwZSI6Im5vbmUiLCJ0bHMiOiIifQ==";
  auto decoded = parse_vmess_uri(uri);
  REQUIRE(decoded);
  decoded->node.id = "rc10-m4-vmess";
  auto store = Store::open(temporary.path / "nodes.sqlite");
  REQUIRE(store);
  REQUIRE((*store)->put_node(decoded->node));
  auto node = (*store)->get_node(decoded->node.id);
  REQUIRE(node);

  ProfileSnapshot profile;
  profile.profile.id = "rc10-m4-profile";
  profile.profile.name = "RC-10 M4 independent export";
  profile.profile.core_id = "xray-v26.9.9";
  profile.profile.selected = Target{TargetKind::node, node->id};
  profile.profile.append_selected_default = true;
  profile.profile.environment.cwd = temporary.path;
  profile.profile.environment.process_environment = {{"PATH", "/usr/bin:/bin"}};
  enable(profile, "log", "00_log.json",
         Json{{"log", Json{{"loglevel", "warning"}}}});
  enable(profile, "inbounds", "02_inbounds.json",
         Json{{"inbounds",
               Json::array({Json{
                   {"listen", "127.0.0.1"},
                   {"port", *port},
                   {"protocol", "socks"},
                   {"tag", "rc10-socks"},
                   {"settings", Json{{"auth", "noauth"}, {"udp", false}}}}})}});
  enable(
      profile, "outbounds", "03_outbounds.json",
      Json{{"outbounds", Json::array({Json{{"protocol", "freedom"},
                                           {"tag", "direct"},
                                           {"settings", Json::object()}}})}});
  enable(profile, "routing", "04_routing.json",
         Json{{"routing", Json{{"rules", Json::array()}}}});
  enable(
      profile, "geodata", "11_geodata.json",
      Json{{"geodata",
            Json{{"assets",
                  Json::array({Json{{"url", "https://fixture.invalid/rc10.dat"},
                                    {"file", "rc10-fixture.dat"}}})}}}});
  enable(profile, "env", "12_env.json",
         Json{{"env", Json{{"xray.location.asset", assets.string()}}}});
  enable(profile, "version", "13_version.json",
         Json{{"version", Json{{"min", "26.9.9"}, {"max", "26.9.9"}}}});

  auto prepared = assemble(BuildInput{ProfileSnapshot{profile}, {*node}});
  REQUIRE(prepared);
  REQUIRE(std::none_of(prepared->files.begin(), prepared->files.end(),
                       [](const OutputFile &file) {
                         return file.relative_name == "config.json";
                       }));
  const auto tail =
      std::find_if(prepared->files.begin(), prepared->files.end(),
                   [](const OutputFile &file) {
                     return file.relative_name == "90_nodes_tail.json";
                   });
  REQUIRE(tail != prepared->files.end());
  CHECK(Json::parse(tail->bytes).at("outbounds").front().at("protocol") ==
        "vmess");

  RuntimeWriter writer(temporary.path / "runtime-store");
  auto managed =
      writer.materialize(*prepared, DirectoryPurpose::candidate, "rc10-m4-run");
  REQUIRE(managed);
  const auto export_dir = temporary.path / "exported-m4-confdir";
  REQUIRE(writer.export_files(managed->confdir, export_dir));
  for (const auto *filename :
       {"00_log.json", "02_inbounds.json", "03_outbounds.json",
        "04_routing.json", "11_geodata.json", "12_env.json", "13_version.json",
        "90_nodes_tail.json"})
    REQUIRE(std::filesystem::is_regular_file(export_dir / filename));
  CHECK_FALSE(std::filesystem::exists(export_dir / "config.json"));
  CHECK(read_file(export_dir / "90_nodes_tail.json") ==
        read_file(managed->confdir / "90_nodes_tail.json"));

  const std::map<std::string, std::string> process_environment{
      {"PATH", "/usr/bin:/bin"}};
  const auto check =
      run_command(binary, {"run", "-test", "-confdir", export_dir.string()},
                  temporary.path, process_environment);
  INFO("export run-test stdout: " << check.out << " stderr: " << check.err);
  REQUIRE(check.code == 0);

  RunningXray exported_core;
  REQUIRE(exported_core.start(binary, export_dir, temporary.path,
                              process_environment));
  CHECK(loopback_accepts(*port));
  CHECK(exported_core.stop());
}
