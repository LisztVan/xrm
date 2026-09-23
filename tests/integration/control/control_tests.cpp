#include "xray/control.hpp"

#include <catch2/catch_test_macros.hpp>
#include <uv.h>

#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <thread>

using namespace xrm;

namespace {
std::string unique_name() {
  return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

void spin(uv_loop_t& loop, const std::function<bool()>& done) {
  for (int i = 0; i < 1000 && !done(); ++i) {
    uv_run(&loop, UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

std::filesystem::path fixture(const std::filesystem::path& dir, std::string bo_action,
                              std::string selected = "exit-b") {
  const auto path = dir / "xray-fixture";
  std::ofstream script(path);
  script << "#!/bin/sh\n"
            "printf '%s\\n' \"$*\" >> \"$XRM_CONTROL_RECORD\"\n"
            "case \" $* \" in\n"
            "  *' bo '*) " << bo_action << " ;;\n"
            "  *' bi '*) ";
  if (selected.empty())
    script << "printf '%s\\n' '{\"balancer\":{\"override\":{},\"principleTarget\":{\"tag\":[\"exit-a\",\"exit-b\"]}}}' ;;\n";
  else
    script << "printf '%s\\n' '{\"balancer\":{\"override\":{\"target\":\"" << selected
           << "\"},\"principleTarget\":{\"tag\":[\"exit-a\",\"exit-b\"]}}}' ;;\n";
  script << "esac\n";
  script.close();
  ::chmod(path.c_str(), 0700);
  return path;
}
}  // namespace

TEST_CASE("Xray API override uses argv and does not shell-escape user tags",
          "[integration][CTRL-01]") {
  auto dir = std::filesystem::temp_directory_path() / ("xrm-control-" + unique_name());
  std::filesystem::create_directories(dir);
  const auto argv_path = dir / "argv.txt";
  const auto binary = fixture(dir, "exit 0");
  uv_loop_t loop;
  REQUIRE(uv_loop_init(&loop) == 0);
  ProcessService process(loop);
  XrayControl control(process);
  RunSpec run;
  run.binary = binary;
  run.cwd = dir;
  run.environment["XRM_CONTROL_RECORD"] = argv_path.string();
  std::optional<Result<OverrideResult>> result;
  REQUIRE(control.override_outbound({run, "127.0.0.1:18080", Milliseconds{1000}}, "lb", "exit-b",
                                    [&](auto value) { result = std::move(value); }));
  spin(loop, [&] { return result.has_value(); });
  REQUIRE(result);
  REQUIRE(*result);
  CHECK((*result)->confirmed);
  CHECK((*result)->actual_outbound == "exit-b");

  std::ifstream input(argv_path);
  const std::string argv((std::istreambuf_iterator<char>(input)), {});
  CHECK(argv.find("api bo --server=127.0.0.1:18080") != std::string::npos);
  CHECK(argv.find("api bi --server=127.0.0.1:18080") != std::string::npos);
  CHECK(argv.find("--json") != std::string::npos);
  CHECK(argv.find("-b lb exit-b") != std::string::npos);
  process.shutdown([&] {});
  uv_run(&loop, UV_RUN_DEFAULT);
  REQUIRE(uv_loop_close(&loop) == 0);
  std::filesystem::remove_all(dir);
}

TEST_CASE("control timeout reports unknown and performs at most one confirmation query",
          "[integration][CTRL-02]") {
  auto dir = std::filesystem::temp_directory_path() / ("xrm-control-timeout-" + unique_name());
  std::filesystem::create_directories(dir);
  const auto calls = dir / "calls.txt";
  const auto binary = fixture(dir, "exec sleep 1", "old-node");
  uv_loop_t loop;
  REQUIRE(uv_loop_init(&loop) == 0);
  ProcessService process(loop);
  XrayControl control(process);
  RunSpec run;
  run.binary = binary;
  run.cwd = dir;
  run.environment["XRM_CONTROL_RECORD"] = calls.string();
  std::optional<Result<OverrideResult>> result;
  REQUIRE(control.override_outbound({run, "127.0.0.1:18080", Milliseconds{50}}, "lb", "exit-b",
                                    [&](auto value) { result = std::move(value); }));
  spin(loop, [&] { return result.has_value(); });
  REQUIRE(result);
  REQUIRE(*result);
  CHECK_FALSE((*result)->confirmed);
  std::ifstream input(calls);
  std::string line;
  int count = 0;
  while (std::getline(input, line)) ++count;
  CHECK(count == 2);
  process.shutdown([&] {});
  uv_run(&loop, UV_RUN_DEFAULT);
  REQUIRE(uv_loop_close(&loop) == 0);
  std::filesystem::remove_all(dir);
}

TEST_CASE("clearing a runtime override uses the official remove flag",
          "[integration][CTRL-01]") {
  auto dir = std::filesystem::temp_directory_path() / ("xrm-control-clear-" + unique_name());
  std::filesystem::create_directories(dir);
  const auto calls = dir / "calls.txt";
  const auto binary = fixture(dir, "exit 0", "");
  uv_loop_t loop;
  REQUIRE(uv_loop_init(&loop) == 0);
  ProcessService process(loop);
  XrayControl control(process);
  RunSpec run;
  run.binary = binary;
  run.cwd = dir;
  run.environment["XRM_CONTROL_RECORD"] = calls.string();
  std::optional<Result<OverrideResult>> result;
  REQUIRE(control.clear_override({run, "127.0.0.1:18080", Milliseconds{1000}}, "lb",
                                 [&](auto value) { result = std::move(value); }));
  spin(loop, [&] { return result.has_value(); });
  REQUIRE(result);
  REQUIRE(*result);
  CHECK((*result)->confirmed);
  std::ifstream input(calls);
  const std::string argv((std::istreambuf_iterator<char>(input)), {});
  CHECK(argv.find("api bo --server=127.0.0.1:18080") != std::string::npos);
  CHECK(argv.find("-b lb -r") != std::string::npos);
  CHECK(argv.find("api bi --server=127.0.0.1:18080") != std::string::npos);
  process.shutdown([&] {});
  uv_run(&loop, UV_RUN_DEFAULT);
  REQUIRE(uv_loop_close(&loop) == 0);
  std::filesystem::remove_all(dir);
}
