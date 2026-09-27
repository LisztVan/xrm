#pragma once

#include "model/runtime.hpp"

#include <deque>
#include <array>

namespace xrm {

struct LogEntry {
  std::uint64_t cursor = 0;
  LogStream stream = LogStream::server;
  std::string text;
};

struct LogPage {
  std::vector<LogEntry> entries;
  std::uint64_t next_cursor = 0;
  bool truncated = false;
};

class LogBuffer {
 public:
  explicit LogBuffer(std::size_t byte_limit = 4 * 1024 * 1024);
  void append(LogStream stream, std::string_view bytes);
  void finish(LogStream stream);
  LogPage read_after(std::uint64_t cursor, std::size_t byte_limit) const;
  std::uint64_t cursor() const;
  std::size_t bytes_retained() const;

 private:
  std::size_t byte_limit_;
  std::size_t bytes_retained_ = 0;
  std::uint64_t next_cursor_ = 1;
  std::deque<LogEntry> entries_;
  // Only incomplete UTF-8 suffixes: at most three bytes per output stream.
  std::array<std::string, 3> pending_utf8_;
};

}  // namespace xrm
