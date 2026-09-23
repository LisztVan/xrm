#include "server/log_buffer.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace xrm;

TEST_CASE("log chunks are bounded and old cursors are explicitly truncated",
          "[integration][LOG-01]") {
  LogBuffer long_line(32 * 1024);
  long_line.append(LogStream::core_stdout, std::string(20 * 1024, 'x'));
  auto split = long_line.read_after(0, 32 * 1024);
  REQUIRE(split.entries.size() == 2);
  CHECK(split.entries[0].text.size() == 16 * 1024);
  CHECK(split.entries[0].stream == LogStream::core_stdout);
  CHECK(split.entries[1].text.size() == 4 * 1024);

  LogBuffer logs(5);
  logs.append(LogStream::core_stdout, "1234567890");
  CHECK(logs.bytes_retained() <= 5);
  const auto page = logs.read_after(0, 5);
  CHECK(page.truncated);
  REQUIRE_FALSE(page.entries.empty());
  CHECK(page.entries.front().stream == LogStream::core_stdout);
}

TEST_CASE("an old log cursor does not block another bounded buffer read",
          "[integration][log-buffer]") {
  LogBuffer logs(1024);
  for (int i = 0; i < 1000; ++i) logs.append(LogStream::core_stderr, "event\n");
  CHECK(logs.cursor() == 1000);
  CHECK(logs.read_after(0, 1024).truncated);
  const auto recent = logs.read_after(990, 1024);
  REQUIRE(recent.entries.size() == 10);
  CHECK_FALSE(recent.truncated);
}
