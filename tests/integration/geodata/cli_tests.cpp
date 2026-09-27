// XRM: Tests cli in the client, daemon, persistence, and Xray integration
// suite.

#include "model/common.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef XRM_CLI_BINARY
#error XRM_CLI_BINARY must name the built xrm executable
#endif
#ifndef XRM_DAEMON_BINARY
#error XRM_DAEMON_BINARY must name the built xrmd executable
#endif

extern char **environ;
using namespace std::chrono_literals;
using xrm::Json;

namespace {

struct TempDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("xrm-geodata-cli-" +
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

struct Captured {
  int code = -1;
  std::string out;
  std::string err;
};

Captured run(const std::filesystem::path &binary,
             const std::vector<std::string> &arguments,
             const std::filesystem::path &root) {
  static unsigned sequence = 0;
  const auto out_path = root / ("out-" + std::to_string(++sequence));
  const auto err_path = root / ("err-" + std::to_string(sequence));
  std::vector<std::string> storage{binary.string()};
  storage.insert(storage.end(), arguments.begin(), arguments.end());
  std::vector<char *> argv;
  for (auto &item : storage)
    argv.push_back(item.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions{};
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, out_path.c_str(),
                                   O_CREAT | O_TRUNC | O_WRONLY, 0600);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, err_path.c_str(),
                                   O_CREAT | O_TRUNC | O_WRONLY, 0600);
  pid_t pid = -1;
  const int started = posix_spawn(&pid, binary.c_str(), &actions, nullptr,
                                  argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (started != 0)
    return {};
  int status = 0;
  const auto deadline = std::chrono::steady_clock::now() + 15s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (::waitpid(pid, &status, WNOHANG) == pid)
      return {WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status),
              read_file(out_path), read_file(err_path)};
    (void)::poll(nullptr, 0, 10);
  }
  ::kill(pid, SIGKILL);
  ::waitpid(pid, &status, 0);
  return {-1, read_file(out_path), read_file(err_path)};
}

Json parsed(const Captured &result) {
  if (result.code != 0)
    return {};
  try {
    return Json::parse(result.out);
  } catch (const Json::exception &) {
    return {};
  }
}

std::string protobuf_field(int number, std::string_view bytes) {
  return std::string{static_cast<char>((number << 3) | 2),
                     static_cast<char>(bytes.size())} +
         std::string(bytes);
}

std::string valid_geoip() {
  const auto cidr = protobuf_field(1, std::string{"\x7f\0\0\0", 4}) +
                    std::string{"\x10\x08", 2};
  return protobuf_field(1, protobuf_field(1, "XRM-TEST") +
                               protobuf_field(2, cidr));
}

struct Server {
  TempDirectory temp;
  std::filesystem::path socket = temp.path / "xrmd.sock";
  pid_t pid = -1;
  Server() {
    std::vector<std::string> storage{
        XRM_DAEMON_BINARY, "--foreground",
        "--database",      (temp.path / "server.sqlite").string(),
        "--socket",        socket.string()};
    std::vector<char *> argv;
    for (auto &item : storage)
      argv.push_back(item.data());
    argv.push_back(nullptr);
    const auto log = temp.path / "daemon.log";
    posix_spawn_file_actions_t actions{};
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log.c_str(),
                                     O_CREAT | O_TRUNC | O_WRONLY, 0600);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    const int started = posix_spawn(&pid, XRM_DAEMON_BINARY, &actions, nullptr,
                                    argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    REQUIRE(started == 0);
    for (int attempt = 0; attempt < 200; ++attempt) {
      if (std::filesystem::exists(socket))
        return;
      (void)::poll(nullptr, 0, 10);
    }
    FAIL("xrmd did not create the test socket: " + read_file(log));
  }
  ~Server() {
    if (pid > 0) {
      ::kill(pid, SIGTERM);
      int status = 0;
      ::waitpid(pid, &status, 0);
    }
  }
  Captured cli(std::initializer_list<std::string> arguments) const {
    std::vector<std::string> args{"--socket", socket.string(), "--json"};
    args.insert(args.end(), arguments.begin(), arguments.end());
    return run(XRM_CLI_BINARY, args, temp.path);
  }
};

} // namespace

TEST_CASE(
    "P5 CLI geodata records use exact name or ID and JSON stays undecorated") {
  Server server;
  auto empty = server.cli({"geodata", "list"});
  REQUIRE(empty.code == 0);
  CHECK(parsed(empty) == Json::array());
  CHECK(empty.out.find("\x1b[") == std::string::npos);
  const auto asset = server.temp.path / "geoip.dat";
  {
    std::ofstream out(asset, std::ios::binary);
    out << "fixture";
  }
  auto created = server.cli({"geodata", "add", "Geo Test", "geoip", "geoip.dat",
                             "--file", asset.string()});
  REQUIRE(created.code == 0);
  auto record = parsed(created);
  REQUIRE(record.is_object());
  CHECK(record.value("name", "") == "Geo Test");
  const auto id = record.value("id", "");
  REQUIRE_FALSE(id.empty());
  auto listed = server.cli({"geodata", "list"});
  REQUIRE(listed.code == 0);
  REQUIRE(parsed(listed).is_array());
  CHECK(parsed(listed).size() == 1);
  auto removed = server.cli({"geodata", "remove", "Geo Test"});
  REQUIRE(removed.code == 0);
  CHECK(parsed(removed).value("removed", "") == id);
  auto missing = server.cli({"geodata", "remove", id});
  CHECK(missing.code != 0);
  CHECK(missing.err.find("not_found") != std::string::npos);
}

TEST_CASE(
    "P5 CLI routing preset plans native JSON and saves only source modules") {
  Server server;
  auto created = server.cli({"profile", "create", "--name", "Home"});
  REQUIRE(created.code == 0);
  const auto profile = parsed(created);
  REQUIRE(profile.is_object());
  const auto id = profile.value("id", "");
  REQUIRE_FALSE(id.empty());
  auto plan = server.cli({"routing", "preset", "plan", "Home", "direct_only"});
  REQUIRE(plan.code == 0);
  auto planned = parsed(plan);
  REQUIRE(planned.is_object());
  CHECK(planned.contains("before"));
  CHECK(planned.contains("after"));
  CHECK(planned.contains("expected_text"));
  const auto base = server.temp.path / "routing-base.json";
  {
    std::ofstream out(base, std::ios::binary);
    out << planned.at("expected_text").get<std::string>();
  }
  auto saved = server.cli({"routing", "preset", "save", id, "direct_only",
                           "--expected-file", base.string()});
  REQUIRE(saved.code == 0);
  CHECK(parsed(saved).contains("runtime_changed"));
  CHECK_FALSE(parsed(saved).value("runtime_changed", true));
  // A second save from the reviewed old base must not silently re-plan an
  // edited source.
  auto stale = server.cli({"routing", "preset", "save", "Home", "direct_only",
                           "--expected-file", base.string()});
  CHECK(stale.code != 0);
  CHECK(stale.err.find("conflict") != std::string::npos);
  auto updated_plan =
      server.cli({"routing", "preset", "plan", "Home", "direct_only"});
  REQUIRE(updated_plan.code == 0);
  {
    std::ofstream out(base, std::ios::binary);
    out << parsed(updated_plan).at("expected_text").get<std::string>();
  }
  auto second = server.cli({"routing", "preset", "save", "Home", "direct_only",
                            "--expected-file", base.string()});
  REQUIRE(second.code == 0);
  CHECK_FALSE(parsed(second).value("changed", true));
  auto not_found =
      server.cli({"routing", "preset", "plan", "HOME", "direct_only"});
  CHECK(not_found.code != 0);
  CHECK(not_found.err.find("not_found") != std::string::npos);
}

TEST_CASE("P5 CLI geodata update validates with fixed Xray and binds by exact "
          "names") {
  const char *binary = std::getenv("XRM_TEST_XRAY");
  if (!binary || !std::filesystem::exists(binary))
    SKIP("fixed Xray unavailable");
  Server server;
  auto core = server.cli({"core", "register", binary, "--name", "Fixed Xray"});
  REQUIRE(core.code == 0);
  auto profile = server.cli({"profile", "create", "--name", "Home"});
  REQUIRE(profile.code == 0);
  const auto asset = server.temp.path / "geoip.dat";
  {
    std::ofstream out(asset, std::ios::binary);
    out << valid_geoip();
  }
  auto created = server.cli({"geodata", "add", "Local GeoIP", "geoip",
                             "geoip.dat", "--file", asset.string()});
  REQUIRE(created.code == 0);
  const auto id = parsed(created).value("id", "");
  REQUIRE_FALSE(id.empty());
  auto before = server.cli({"geodata", "bind", "Home", "Local GeoIP"});
  CHECK(before.code != 0);
  auto updated =
      server.cli({"geodata", "update", "Local GeoIP", "--core", "Fixed Xray"});
  INFO(updated.err);
  REQUIRE(updated.code == 0);
  auto detail = server.cli({"geodata", "list"});
  REQUIRE(detail.code == 0);
  REQUIRE(parsed(detail).is_array());
  REQUIRE(parsed(detail).size() == 1);
  CHECK_FALSE(parsed(detail).at(0).at("current").is_null());
  auto bound = server.cli({"geodata", "bind", "Home", id});
  INFO(bound.err);
  REQUIRE(bound.code == 0);
  auto shown = server.cli({"profile", "show", parsed(profile).value("id", "")});
  REQUIRE(shown.code == 0);
  CHECK(parsed(shown).at("geodata_resource_ids").at("geoip.dat") == id);
  auto blocked = server.cli({"geodata", "remove", "Local GeoIP"});
  CHECK(blocked.code != 0);
}
