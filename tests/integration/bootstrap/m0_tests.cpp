/// @file
/// @brief Exercises daemon startup, first node persistence, and
/// unavailable-server CLI errors.

#include <catch2/catch_test_macros.hpp>

#include "model/common.hpp"

#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

extern char **environ;

namespace {

class TempDirectory {
public:
  TempDirectory() {
    auto pattern =
        (std::filesystem::temp_directory_path() / "xrm-m0-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    path_ = ::mkdtemp(writable.data());
  }
  ~TempDirectory() {
    if (!path_.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(path_, ignored);
    }
  }
  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

struct Captured {
  int exit_code = -1;
  std::string out;
  std::string err;
  bool timed_out = false;
};

std::string read_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

Captured run_manager(const std::filesystem::path &directory,
                     const std::vector<std::string> &arguments) {
  static unsigned sequence = 0;
  const auto out_path = directory / ("stdout-" + std::to_string(++sequence));
  const auto err_path = directory / ("stderr-" + std::to_string(sequence));
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, out_path.c_str(),
                                   O_CREAT | O_TRUNC | O_WRONLY, 0600);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, err_path.c_str(),
                                   O_CREAT | O_TRUNC | O_WRONLY, 0600);
  std::vector<std::string> storage{XRM_TEST_MANAGER};
  storage.insert(storage.end(), arguments.begin(), arguments.end());
  std::vector<char *> argv;
  for (auto &value : storage)
    argv.push_back(value.data());
  argv.push_back(nullptr);
  std::array<char *, 1> environment{nullptr};
  pid_t pid = -1;
  const int spawn_status =
      posix_spawn(&pid, XRM_TEST_MANAGER, &actions, nullptr, argv.data(),
                  environment.data());
  posix_spawn_file_actions_destroy(&actions);
  if (spawn_status != 0)
    return {};

  int status = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto waited = ::waitpid(pid, &status, WNOHANG);
    if (waited == pid) {
      Captured result;
      result.exit_code =
          WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
      result.out = read_file(out_path);
      result.err = read_file(err_path);
      return result;
    }
    if (waited < 0)
      break;
    ::poll(nullptr, 0, 10);
  }
  ::kill(pid, SIGKILL);
  ::waitpid(pid, &status, 0);
  Captured result;
  result.timed_out = true;
  result.out = read_file(out_path);
  result.err = read_file(err_path);
  return result;
}

class ServerProcess {
public:
  bool start(const std::filesystem::path &socket,
             const std::filesystem::path &database) {
    std::vector<std::string> storage{XRM_TEST_SERVER, "--foreground",
                                     "--socket",      socket.string(),
                                     "--database",    database.string()};
    std::vector<char *> argv;
    for (auto &value : storage)
      argv.push_back(value.data());
    argv.push_back(nullptr);
    std::array<char *, 1> environment{nullptr};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null",
                                     O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                     O_WRONLY, 0);
    const int status = posix_spawn(&pid_, XRM_TEST_SERVER, &actions, nullptr,
                                   argv.data(), environment.data());
    posix_spawn_file_actions_destroy(&actions);
    return status == 0;
  }

  bool stop() {
    if (pid_ <= 0)
      return true;
    ::kill(pid_, SIGTERM);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto waited = ::waitpid(pid_, &status, WNOHANG);
      if (waited == pid_) {
        pid_ = -1;
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
      }
      if (waited < 0) {
        pid_ = -1;
        return false;
      }
      ::poll(nullptr, 0, 10);
    }
    ::kill(pid_, SIGKILL);
    ::waitpid(pid_, &status, 0);
    pid_ = -1;
    return false;
  }

  ~ServerProcess() {
    if (pid_ > 0) {
      ::kill(pid_, SIGTERM);
      ::waitpid(pid_, nullptr, 0);
    }
  }

private:
  pid_t pid_ = -1;
};

bool wait_for_socket(const std::filesystem::path &path) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
      return false;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto value = path.string();
    if (value.size() >= sizeof(address.sun_path)) {
      ::close(fd);
      return false;
    }
    std::copy(value.begin(), value.end(), address.sun_path);
    address.sun_path[value.size()] = '\0';
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address),
                  sizeof(address)) == 0) {
      ::close(fd);
      return true;
    }
    ::close(fd);
    ::poll(nullptr, 0, 10);
  }
  return false;
}

xrm::Json json_result(const Captured &captured) {
  if (captured.exit_code != 0 || captured.timed_out || !captured.err.empty())
    return {};
  try {
    return xrm::Json::parse(captured.out);
  } catch (const xrm::Json::exception &) {
    return {};
  }
}

} // namespace

TEST_CASE("E2E-01 xrm to xrmd persists a core and manual node over restart",
          "[E2E-01]") {
  TempDirectory temp;
  REQUIRE_FALSE(temp.path().empty());
  const auto socket = temp.path() / "server.sock";
  const auto database = temp.path() / "server.sqlite";
  ServerProcess server;

  REQUIRE(server.start(socket, database));
  REQUIRE(wait_for_socket(socket));
  const auto server_info = run_manager(
      temp.path(), {"--socket", socket.string(), "--json", "server", "info"});
  REQUIRE(server_info.exit_code == 0);
  REQUIRE(json_result(server_info).at("protocol") == 1);
  REQUIRE(json_result(server_info).at("boot_id").is_string());
  const auto core_register = run_manager(
      temp.path(), {"--socket", socket.string(), "--json", "core", "register",
                    XRM_TEST_FAKE_CORE, "--name", "fixture"});
  REQUIRE_FALSE(core_register.timed_out);
  REQUIRE(core_register.exit_code == 0);
  const auto core_value = json_result(core_register);
  REQUIRE(core_value.at("name") == "fixture");
  REQUIRE(core_value.at("reported_version").get<std::string>().find("26.9.9") !=
          std::string::npos);
  REQUIRE(core_value.at("path") == XRM_TEST_FAKE_CORE);

  const auto invalid_core = run_manager(
      temp.path(),
      {"--socket", socket.string(), "--json", "core", "register",
       (temp.path() / "missing-xray").string(), "--name", "broken"});
  REQUIRE_FALSE(invalid_core.timed_out);
  REQUIRE(invalid_core.exit_code == 1);
  const auto invalid_core_error = xrm::Json::parse(invalid_core.err);
  REQUIRE(invalid_core_error.at("error").at("code") == "invalid_argument");

  const std::string outbound =
      R"({"protocol":"vless","settings":{"vnext":[{"address":"example.invalid","port":443,"users":[{"id":"fixture-secret"}]}]}})";
  const auto node_add = run_manager(
      temp.path(), {"--socket", socket.string(), "--json", "node", "add",
                    "--name", "manual", "--outbound-json", outbound});
  REQUIRE_FALSE(node_add.timed_out);
  REQUIRE(node_add.exit_code == 0);
  REQUIRE(json_result(node_add).at("name") == "manual");

  const auto nodes_before = run_manager(
      temp.path(), {"--socket", socket.string(), "--json", "node", "list"});
  REQUIRE(nodes_before.exit_code == 0);
  const auto node_page = json_result(nodes_before);
  REQUIRE(node_page.at("total") == 1);
  REQUIRE(node_page.at("items").at(0).at("protocol") == "vless");
  REQUIRE(nodes_before.out.find("fixture-secret") == std::string::npos);

  const auto cores_before = run_manager(
      temp.path(), {"--socket", socket.string(), "--json", "core", "list"});
  REQUIRE(cores_before.exit_code == 0);
  REQUIRE(json_result(cores_before).size() == 1);
  REQUIRE(json_result(cores_before).at(0).at("path") == XRM_TEST_FAKE_CORE);
  REQUIRE(server.stop());

  REQUIRE(server.start(socket, database));
  REQUIRE(wait_for_socket(socket));
  const auto nodes_after = run_manager(
      temp.path(), {"--socket", socket.string(), "--json", "node", "list"});
  REQUIRE(nodes_after.exit_code == 0);
  REQUIRE(json_result(nodes_after).at("items").at(0).at("name") == "manual");
  const auto cores_after = run_manager(
      temp.path(), {"--socket", socket.string(), "--json", "core", "list"});
  REQUIRE(cores_after.exit_code == 0);
  REQUIRE(json_result(cores_after).at(0).at("reported_version") ==
          core_value.at("reported_version"));
  REQUIRE(server.stop());
}

TEST_CASE("CLI-01 server unavailable is an error on stderr, not false success",
          "[CLI-01]") {
  TempDirectory temp;
  const auto missing = temp.path() / "missing.sock";
  const auto result = run_manager(
      temp.path(), {"--socket", missing.string(), "--json", "node", "list"});
  REQUIRE_FALSE(result.timed_out);
  REQUIRE(result.exit_code != 0);
  REQUIRE(result.out.empty());
  REQUIRE_FALSE(result.err.empty());
  const auto error = xrm::Json::parse(result.err);
  REQUIRE(error.contains("error"));
}

TEST_CASE("LOCAL-01 CLI creates typed and imported manual nodes without "
          "exposing credentials in list",
          "[LOCAL-01]") {
  TempDirectory temp;
  REQUIRE_FALSE(temp.path().empty());
  const auto socket = temp.path() / "server.sock";
  const auto database = temp.path() / "server.sqlite";
  ServerProcess server;
  REQUIRE(server.start(socket, database));
  REQUIRE(wait_for_socket(socket));

  const std::string settings =
      R"({"kind":"stream","endpoint":{"address":"127.0.0.1","port":443},"protocol":{"kind":"vless","id":"11111111-1111-4111-8111-111111111111","encryption":null,"flow":null,"level":null},"transport":{"kind":"raw"},"security":{"kind":"none"},"sockopt":null,"finalmask":null,"mux":null})";
  const auto form_added = run_manager(
      temp.path(), {"--socket", socket.string(), "--json", "node", "add",
                    "--name", "form node", "--settings-json", settings});
  REQUIRE(form_added.exit_code == 0);
  const auto form_id = json_result(form_added).at("id").get<std::string>();
  const auto form_details =
      run_manager(temp.path(), {"--socket", socket.string(), "--json", "node",
                                "show", form_id});
  REQUIRE(form_details.exit_code == 0);
  CHECK(json_result(form_details).at("source_id").is_null());
  CHECK(json_result(form_details).at("settings").at("kind") == "stream");
  CHECK(json_result(form_details).at("settings").at("protocol").at("id") ==
        "11111111-1111-4111-8111-111111111111");
  auto revised_settings = xrm::Json::parse(settings);
  revised_settings["endpoint"]["port"] = 8444;
  const auto revised =
      run_manager(temp.path(), {"--socket", socket.string(), "--json", "node",
                                "replace", form_id, "--name", "form revised",
                                "--settings-json", revised_settings.dump()});
  REQUIRE(revised.exit_code == 0);
  CHECK(json_result(revised).at("id") == form_id);
  const auto revised_details =
      run_manager(temp.path(), {"--socket", socket.string(), "--json", "node",
                                "show", form_id});
  REQUIRE(revised_details.exit_code == 0);
  CHECK(json_result(revised_details).at("original_name") == "form revised");
  CHECK(json_result(revised_details).at("settings").at("endpoint").at("port") ==
        8444);

  const auto uri_added =
      run_manager(temp.path(), {"--socket", socket.string(), "--json", "node",
                                "add", "--uri",
                                "vless://"
                                "22222222-2222-4222-8222-222222222222@127.0.0."
                                "1:8443?type=tcp&security=none#From%20URI"});
  REQUIRE(uri_added.exit_code == 0);
  const auto uri_id = json_result(uri_added).at("id").get<std::string>();
  const auto uri_details =
      run_manager(temp.path(), {"--socket", socket.string(), "--json", "node",
                                "show", uri_id});
  REQUIRE(uri_details.exit_code == 0);
  CHECK(json_result(uri_details).at("source_id").is_null());
  CHECK(json_result(uri_details).at("settings").at("kind") == "stream");

  const auto list = run_manager(
      temp.path(), {"--socket", socket.string(), "--json", "node", "list"});
  REQUIRE(list.exit_code == 0);
  CHECK(json_result(list).at("total") == 2);
  CHECK(list.out.find("11111111-1111-4111-8111-111111111111") ==
        std::string::npos);
  CHECK(list.out.find("22222222-2222-4222-8222-222222222222") ==
        std::string::npos);
  const auto removed =
      run_manager(temp.path(), {"--socket", socket.string(), "--json", "node",
                                "remove", uri_id});
  REQUIRE(removed.exit_code == 0);
  CHECK(json_result(removed).at("removed") == true);
  const auto after_remove = run_manager(
      temp.path(), {"--socket", socket.string(), "--json", "node", "list"});
  REQUIRE(after_remove.exit_code == 0);
  CHECK(json_result(after_remove).at("total") == 1);
  REQUIRE(server.stop());
}
