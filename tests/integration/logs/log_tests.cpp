/// @file
/// @brief Checks bounded log chunks, old cursors, stream endings, and
/// truncation signals.

#include "contract/wire.hpp"
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
  for (int i = 0; i < 1000; ++i)
    logs.append(LogStream::core_stderr, "event\n");
  CHECK(logs.cursor() == 1000);
  CHECK(logs.read_after(0, 1024).truncated);
  const auto recent = logs.read_after(990, 1024);
  REQUIRE(recent.entries.size() == 10);
  CHECK_FALSE(recent.truncated);
}

TEST_CASE("split UTF8 logs remain valid through the JSON response boundary",
          "[integration][log-buffer][utf8]") {
  LogBuffer logs;
  const std::string chinese = "中文";
  logs.append(LogStream::core_stdout, chinese.substr(0, 2));
  CHECK(logs.read_after(0, 65536).entries.empty());
  logs.append(LogStream::core_stderr, "other\n");
  logs.append(LogStream::core_stdout, chinese.substr(2) + "\r\n");
  const auto page = logs.read_after(0, 65536);
  REQUIRE(page.entries.size() == 2);
  CHECK(page.entries[1].text == chinese + "\r\n");
  Json entries = Json::array();
  for (const auto &entry : page.entries)
    entries.push_back(Json{{"cursor", entry.cursor}, {"text", entry.text}});
  const auto encoded = encode_response(
      RpcResponse{1, 42, true,
                  Json{{"entries", entries}, {"next_cursor", page.next_cursor}},
                  std::nullopt});
  CHECK(Json::parse(encoded).at("result").at("entries").at(1).at("text") ==
        chinese + "\r\n");
}

TEST_CASE("log chunk and read limits never split a UTF8 codepoint",
          "[integration][log-buffer][utf8]") {
  LogBuffer logs(32 * 1024);
  const std::string original = std::string(16383, 'x') + "中文\n";
  logs.append(LogStream::core_stdout, original);
  const auto page = logs.read_after(0, 32768);
  REQUIRE(page.entries.size() == 2);
  CHECK(page.entries[0].text.size() == 16383);
  CHECK(page.entries[0].text + page.entries[1].text == original);
  for (const auto &entry : page.entries)
    CHECK_NOTHROW(Json(entry.text).dump());
  const auto clipped = logs.read_after(page.entries[0].cursor, 2);
  REQUIRE(clipped.entries.size() == 1);
  CHECK(clipped.entries[0].text.empty());
  CHECK(clipped.truncated);
  CHECK_NOTHROW(Json(clipped.entries[0].text).dump());
}

TEST_CASE(
    "malformed log bytes have visible transport spelling without crashing JSON",
    "[integration][log-buffer][utf8]") {
  LogBuffer logs(1024);
  logs.append(LogStream::core_stdout, "bad:\xff\xc0\xaf\n");
  const auto page = logs.read_after(0, 1024);
  REQUIRE(page.entries.size() == 1);
  CHECK(page.entries[0].text == "bad:\\xFF\\xC0\\xAF\n");
  CHECK(logs.bytes_retained() == 8);
  CHECK_NOTHROW(Json(page.entries[0].text).dump());
  const auto limited = logs.read_after(0, 5);
  REQUIRE(limited.entries.size() == 1);
  CHECK(limited.entries[0].text.size() <= 5);
  CHECK(limited.truncated);
}

TEST_CASE("stream EOF exposes incomplete UTF8 bytes once without joining the "
          "next run",
          "[integration][log-buffer][utf8]") {
  LogBuffer logs(1024);
  logs.append(LogStream::core_stdout, "tail:\xe4\xb8");
  logs.append(LogStream::core_stderr, "\xf0\x9f");
  logs.finish(LogStream::core_stdout);
  logs.finish(LogStream::core_stdout);
  auto page = logs.read_after(0, 1024);
  REQUIRE(page.entries.size() == 2);
  CHECK(page.entries[1].text == "\\xE4\\xB8");
  logs.append(LogStream::core_stderr, "\x98\x80");
  logs.append(LogStream::core_stdout, "new run\n");
  page = logs.read_after(page.next_cursor, 1024);
  REQUIRE(page.entries.size() == 2);
  CHECK(page.entries[0].text == "😀");
  CHECK(page.entries[1].text == "new run\n");
  for (const auto &entry : page.entries)
    CHECK_NOTHROW(Json(entry.text).dump());
}
