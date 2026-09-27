/// @file
/// @brief Checks native env, launch environment, and core-version boundaries.

#include <catch2/catch_test_macros.hpp>

#include "xray/runtime_assembler.hpp"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <poll.h>
#include <string>
#include <vector>

namespace {
using namespace std::chrono_literals;
using namespace xrm;

struct TempDirectory {
  std::filesystem::path path;
  TempDirectory() {
    static std::atomic<unsigned> sequence{0};
    path = std::filesystem::temp_directory_path() /
           ("xrm-real-core-rc09-" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directories(path);
  }
  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

std::filesystem::path xray_binary() {
  if (const char *value = std::getenv("XRM_TEST_XRAY"); value && *value)
    return std::filesystem::path(value);
#ifdef XRM_TEST_XRAY_CONFIGURED
  return XRM_TEST_XRAY_CONFIGURED;
#else
  return {};
#endif
}

struct CommandResult {
  int code = -1;
  std::string out;
  std::string err;
  bool timed_out = false;
};

std::string read_file(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

CommandResult run_xray(const std::filesystem::path &binary,
                       const std::vector<std::string> &arguments,
                       const std::filesystem::path &cwd,
                       const std::map<std::string, std::string> &environment,
                       const std::filesystem::path &log_root,
                       std::chrono::seconds timeout = 20s) {
  static std::atomic<unsigned> sequence{0};
  const auto number = sequence.fetch_add(1);
  const auto out_path = log_root / ("rc09-" + std::to_string(number) + ".out");
  const auto err_path = log_root / ("rc09-" + std::to_string(number) + ".err");

  std::vector<std::string> argv_storage{binary.string()};
  argv_storage.insert(argv_storage.end(), arguments.begin(), arguments.end());
  std::vector<char *> argv;
  argv.reserve(argv_storage.size() + 1);
  for (auto &value : argv_storage)
    argv.push_back(value.data());
  argv.push_back(nullptr);

  std::vector<std::string> env_storage;
  env_storage.reserve(environment.size());
  for (const auto &[key, value] : environment)
    env_storage.push_back(key + "=" + value);
  std::vector<char *> envp;
  envp.reserve(env_storage.size() + 1);
  for (auto &value : env_storage)
    envp.push_back(value.data());
  envp.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0)
    return {};
  const int cwd_result =
      posix_spawn_file_actions_addchdir_np(&actions, cwd.c_str());
  const int out_result = posix_spawn_file_actions_addopen(
      &actions, STDOUT_FILENO, out_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY,
      0600);
  const int err_result = posix_spawn_file_actions_addopen(
      &actions, STDERR_FILENO, err_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY,
      0600);
  if (cwd_result != 0 || out_result != 0 || err_result != 0) {
    posix_spawn_file_actions_destroy(&actions);
    return {};
  }

  pid_t pid = -1;
  const int spawned = posix_spawn(&pid, binary.c_str(), &actions, nullptr,
                                  argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&actions);
  if (spawned != 0)
    return {};

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  int status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto waited = ::waitpid(pid, &status, WNOHANG);
    if (waited == pid) {
      return {WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status),
              read_file(out_path), read_file(err_path), false};
    }
    if (waited < 0)
      break;
    ::poll(nullptr, 0, 10);
  }
  ::kill(pid, SIGKILL);
  ::waitpid(pid, &status, 0);
  return {-1, read_file(out_path), read_file(err_path), true};
}

void enable(BuildInput &input, std::string key, std::string filename,
            Json body) {
  input.settings.profile.modules.push_back(ModuleSetting{key, filename, true});
  input.settings.modules.push_back(ModuleDocument{
      std::move(key), std::move(filename), Json(body).dump(2) + "\n"});
}

BuildInput
base_input(const std::filesystem::path &cwd,
           std::map<std::string, std::string> process_environment,
           std::optional<std::filesystem::path> native_asset_directory,
           std::string min_version, std::string max_version) {
  BuildInput input;
  input.settings.profile.id = "rc09-profile";
  input.settings.profile.name = "RC-09 isolated context fixture";
  input.settings.profile.core_id = "rc09-core";
  input.settings.profile.environment.cwd = cwd;
  input.settings.profile.environment.process_environment =
      std::move(process_environment);

  Json native_env = Json::object();
  if (native_asset_directory)
    native_env["xray.location.asset"] = native_asset_directory->string();
  enable(input, "env", "12_env.json", Json{{"env", std::move(native_env)}});
  enable(input, "version", "13_version.json",
         Json{{"version", Json{{"min", std::move(min_version)},
                               {"max", std::move(max_version)}}}});
  enable(
      input, "geodata", "11_geodata.json",
      Json{{"geodata",
            Json{{"assets", Json::array({Json{
                                {"url", "https://fixture.invalid/geodata.dat"},
                                {"file", "rc09-fixture.dat"}}})}}}});
  return input;
}

std::optional<std::filesystem::path>
assemble_confdir(const BuildInput &input, const std::filesystem::path &root,
                 std::string name) {
  auto prepared = assemble(input);
  if (!prepared)
    return std::nullopt;
  const auto directory = root / std::move(name);
  std::filesystem::create_directories(directory);
  for (const auto &file : prepared->files) {
    const auto destination = directory / file.relative_name;
    std::filesystem::create_directories(destination.parent_path());
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output)
      return std::nullopt;
    output.write(file.bytes.data(),
                 static_cast<std::streamsize>(file.bytes.size()));
    if (!output)
      return std::nullopt;
  }
  return directory;
}

std::optional<std::string> parent_environment_value(const char *name) {
  const char *value = std::getenv(name);
  if (!value)
    return std::nullopt;
  return std::string(value);
}

} // namespace

TEST_CASE("RC-09 real Xray separates child launch env, native env module, "
          "version limits, and asset context",
          "[real_core][RC-09]") {
  const auto binary = xray_binary();
  REQUIRE_FALSE(binary.empty());
  REQUIRE(binary.is_absolute());
  REQUIRE(std::filesystem::is_regular_file(binary));

  TempDirectory temporary;
  const auto process_assets = temporary.path / "process-assets";
  const auto native_assets = temporary.path / "native-assets";
  const auto missing_assets = temporary.path / "missing-assets";
  std::filesystem::create_directories(process_assets);
  std::filesystem::create_directories(native_assets);
  {
    std::ofstream(process_assets / "rc09-fixture.dat", std::ios::binary)
        << "process asset fixture\n";
    std::ofstream(native_assets / "rc09-fixture.dat", std::ios::binary)
        << "native asset fixture\n";
  }

  const auto parent_asset_before =
      parent_environment_value("xray.location.asset");
  const auto parent_marker_before =
      parent_environment_value("XRM_RC09_LAUNCH_CONTEXT");
  const auto version = run_xray(binary, {"version"}, temporary.path,
                                {{"PATH", "/usr/bin:/bin"}}, temporary.path);
  INFO("Xray version stdout: " << version.out);
  INFO("Xray version stderr: " << version.err);
  REQUIRE_FALSE(version.timed_out);
  REQUIRE(version.code == 0);
  REQUIRE(version.out.find("26.9.9") != std::string::npos);

  SECTION("process launch environment resolves resources without changing the "
          "test process") {
    auto input =
        base_input(temporary.path,
                   {{"xray.location.asset", process_assets.string()},
                    {"XRM_RC09_LAUNCH_CONTEXT", "isolated-child-only"}},
                   std::nullopt, "26.9.9", "26.9.9");
    const auto confdir =
        assemble_confdir(input, temporary.path, "process-env-confdir");
    REQUIRE(confdir.has_value());
    const auto env_file = read_file(*confdir / "12_env.json");
    CHECK(env_file.find(process_assets.string()) == std::string::npos);
    CHECK(input.settings.profile.environment.process_environment.at(
              "XRM_RC09_LAUNCH_CONTEXT") == "isolated-child-only");

    const auto result = run_xray(
        binary, {"run", "-test", "-confdir", confdir->string()},
        input.settings.profile.environment.cwd,
        input.settings.profile.environment.process_environment, temporary.path);
    INFO("process-env check exit: " << result.code);
    INFO("process-env check stderr: " << result.err);
    CHECK_FALSE(result.timed_out);
    CHECK(result.code == 0);
    CHECK(parent_environment_value("xray.location.asset") ==
          parent_asset_before);
    CHECK(parent_environment_value("XRM_RC09_LAUNCH_CONTEXT") ==
          parent_marker_before);
  }

  SECTION("native env module overrides only the child launch value and selects "
          "its own asset directory") {
    auto input =
        base_input(temporary.path,
                   {{"xray.location.asset", missing_assets.string()},
                    {"XRM_RC09_LAUNCH_CONTEXT", "isolated-child-only"}},
                   native_assets, "26.9.9", "26.9.9");
    const auto confdir =
        assemble_confdir(input, temporary.path, "native-env-confdir");
    REQUIRE(confdir.has_value());
    const auto env_file = Json::parse(read_file(*confdir / "12_env.json"));
    REQUIRE(env_file.at("env").at("xray.location.asset") ==
            native_assets.string());
    CHECK(env_file.dump().find(missing_assets.string()) == std::string::npos);

    const auto result = run_xray(
        binary, {"run", "-test", "-confdir", confdir->string()},
        input.settings.profile.environment.cwd,
        input.settings.profile.environment.process_environment, temporary.path);
    INFO("native-env check exit: " << result.code);
    INFO("native-env check stdout: " << result.out);
    INFO("native-env check stderr: " << result.err);
    CHECK_FALSE(result.timed_out);
    CHECK(result.code == 0);
    CHECK(parent_environment_value("xray.location.asset") ==
          parent_asset_before);
    CHECK(parent_environment_value("XRM_RC09_LAUNCH_CONTEXT") ==
          parent_marker_before);
  }

  SECTION("wrong native asset directory is attributed to the geodata file "
          "reference") {
    auto input = base_input(temporary.path,
                            {{"xray.location.asset", process_assets.string()}},
                            missing_assets, "26.9.9", "26.9.9");
    const auto confdir =
        assemble_confdir(input, temporary.path, "bad-native-env-confdir");
    REQUIRE(confdir.has_value());

    const auto result = run_xray(
        binary, {"run", "-test", "-confdir", confdir->string()},
        input.settings.profile.environment.cwd,
        input.settings.profile.environment.process_environment, temporary.path);
    INFO("bad-resource check stderr: " << result.err);
    CHECK_FALSE(result.timed_out);
    CHECK(result.code != 0);
    const auto diagnostic = result.out + "\n" + result.err;
    CHECK(diagnostic.find("invalid geodata asset file") != std::string::npos);
    CHECK(diagnostic.find("rc09-fixture.dat") != std::string::npos);
  }

  SECTION("fixed core accepts matching min/max and rejects incompatible "
          "version bounds") {
    const auto run_version_case =
        [&](std::string min_version, std::string max_version,
            std::string name) -> std::optional<CommandResult> {
      auto input = base_input(
          temporary.path, {{"xray.location.asset", missing_assets.string()}},
          native_assets, std::move(min_version), std::move(max_version));
      const auto confdir =
          assemble_confdir(input, temporary.path, std::move(name));
      if (!confdir)
        return std::nullopt;
      return run_xray(binary, {"run", "-test", "-confdir", confdir->string()},
                      input.settings.profile.environment.cwd,
                      input.settings.profile.environment.process_environment,
                      temporary.path);
    };

    const auto accepted =
        run_version_case("26.9.9", "26.9.9", "exact-version-confdir");
    REQUIRE(accepted.has_value());
    INFO("accepted version stderr: " << accepted->err);
    REQUIRE_FALSE(accepted->timed_out);
    REQUIRE(accepted->code == 0);

    const auto too_old =
        run_version_case("27.0.0", "", "minimum-version-confdir");
    REQUIRE(too_old.has_value());
    INFO("too-old version stderr: " << too_old->err);
    REQUIRE_FALSE(too_old->timed_out);
    CHECK(too_old->code != 0);
    CHECK((too_old->out + too_old->err).find("27.0.0 or higher") !=
          std::string::npos);

    const auto too_new =
        run_version_case("", "26.9.8", "maximum-version-confdir");
    REQUIRE(too_new.has_value());
    INFO("too-new version stderr: " << too_new->err);
    REQUIRE_FALSE(too_new->timed_out);
    CHECK(too_new->code != 0);
    CHECK((too_new->out + too_new->err).find("26.9.8 or lower") !=
          std::string::npos);
  }

  CHECK(parent_environment_value("xray.location.asset") == parent_asset_before);
  CHECK(parent_environment_value("XRM_RC09_LAUNCH_CONTEXT") ==
        parent_marker_before);
}
