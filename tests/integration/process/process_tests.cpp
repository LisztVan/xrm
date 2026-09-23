#include <catch2/catch_test_macros.hpp>
#include <csignal>

#include "infra/process.hpp"
#include "xray/process_adapter.hpp"

#include <uv.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <chrono>

namespace {

struct Loop {
  uv_loop_t value{};
  Loop() { REQUIRE(uv_loop_init(&value) == 0); }
  ~Loop() { uv_loop_close(&value); }
};

xrm::Result<xrm::ProcessResult> run(xrm::ProcessService& service,
                                    uv_loop_t& loop,
                                    xrm::ProcessSpec spec,
                                    xrm::Milliseconds timeout = std::chrono::seconds(3)) {
  std::optional<xrm::Result<xrm::ProcessResult>> result;
  auto accepted = service.execute(std::move(spec), timeout,
                                 [&](auto value) { result.emplace(std::move(value)); });
  REQUIRE(accepted);
  uv_run(&loop, UV_RUN_DEFAULT);
  REQUIRE(result.has_value());
  return std::move(*result);
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE("PROC-01 preserves argv, cwd, and the child environment", "[PROC-01]") {
  const auto temp = std::filesystem::temp_directory_path() /
                    ("xrm process cwd " + std::to_string(
                        std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(temp);
  const auto executable = temp / "fake core executable";
  const auto record = temp / "argv record.txt";
  const auto cwd_record = temp / "cwd record.txt";
  std::filesystem::copy_file(XRM_TEST_FAKE_CORE, executable,
                             std::filesystem::copy_options::overwrite_existing);
  Loop loop;
  xrm::ProcessService process(loop.value);
  xrm::ProcessSpec spec;
  spec.executable = executable;
  spec.arguments = {"argument with spaces", "quote'\"", "semi;colon"};
  spec.cwd = temp;
  spec.environment = {{"XRM_FAKE_CORE_ARGV", record.string()},
                      {"XRM_FAKE_CORE_CWD", cwd_record.string()},
                      {"XRM_FAKE_CORE_STDOUT", "stdout-only"},
                      {"XRM_FAKE_CORE_STDERR", "stderr-only"}};
  auto result = run(process, loop.value, std::move(spec));
  REQUIRE(result);
  REQUIRE(result->exit.exit_code == 0);
  REQUIRE(result->stdout_text == "stdout-only");
  REQUIRE(result->stderr_text == "stderr-only");
  REQUIRE(read_file(record) == "0\t" + executable.string() + "\n"
                               "1\targument with spaces\n"
                               "2\tquote'\"\n"
                               "3\tsemi;colon\n");
  REQUIRE(read_file(cwd_record) == temp.string());
  std::filesystem::remove_all(temp);
}

TEST_CASE("PROC-02 returns nonzero exit and reports execution deadlines", "[PROC-02]") {
  Loop loop;
  xrm::ProcessService process(loop.value);
  xrm::ProcessSpec failing;
  failing.executable = XRM_TEST_FAKE_CORE;
  failing.environment["XRM_FAKE_CORE_EXIT"] = "23";
  auto failed = run(process, loop.value, std::move(failing));
  REQUIRE(failed);
  REQUIRE(failed->exit.exit_code == 23);

  xrm::ProcessSpec signalled;
  signalled.executable = XRM_TEST_FAKE_CORE;
  signalled.environment["XRM_FAKE_CORE_SIGNAL"] = "TERM";
  auto signalled_result = run(process, loop.value, std::move(signalled));
  REQUIRE(signalled_result);
  REQUIRE(signalled_result->exit.term_signal == SIGTERM);

  xrm::ProcessSpec hanging;
  hanging.executable = XRM_TEST_FAKE_CORE;
  hanging.environment["XRM_FAKE_CORE_HANG"] = "1";
  auto timed_out = run(process, loop.value, std::move(hanging), std::chrono::milliseconds(60));
  REQUIRE_FALSE(timed_out);
  REQUIRE(timed_out.error().code == xrm::ErrorCode::timed_out);
}

TEST_CASE("PROC-03 bounds captured output while preserving stream identity", "[PROC-03]") {
  Loop loop;
  xrm::ProcessService process(loop.value);
  xrm::ProcessSpec spec;
  spec.executable = XRM_TEST_FAKE_CORE;
  spec.environment["XRM_FAKE_CORE_STDOUT_BYTES"] = "1100000";
  spec.environment["XRM_FAKE_CORE_STDERR_BYTES"] = "1100000";
  auto result = run(process, loop.value, std::move(spec));
  REQUIRE(result);
  REQUIRE(result->output_truncated);
  REQUIRE(result->stdout_text.size() == 1024 * 1024);
  REQUIRE(result->stderr_text.size() == 1024 * 1024);
}

TEST_CASE("Xray version adapter constructs argv and rejects unsuccessful output", "[PROC-02]") {
  auto command = xrm::version_command("/path with spaces/xray");
  REQUIRE(command.arguments == std::vector<std::string>{"version"});
  REQUIRE(xrm::read_core_version({{0, 0}, "Xray 26.9.9 (fixture)\n", "", false}) ==
          "Xray 26.9.9 (fixture)");
  REQUIRE_FALSE(xrm::read_core_version({{7, 0}, "", "failure", false}));
}
