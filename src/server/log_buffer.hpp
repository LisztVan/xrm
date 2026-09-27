/// @file
/// @brief Declares bounded log buffering and cursor reads.

#pragma once

#include "model/runtime.hpp"

#include <array>
#include <deque>

namespace xrm {

struct LogEntry {
  // Transport cursor for buffered chunks, not a user-visible log record ID.
  std::uint64_t cursor = 0;
  LogStream stream = LogStream::server;
  std::string text;
};

struct LogPage {
  std::vector<LogEntry> entries;
  std::uint64_t next_cursor = 0;
  bool truncated = false;
};

/// @brief Keeps a bounded byte stream for daemon and core output.
///
/// Entries are transport chunks, not parsed log records. A reader resumes
/// with the opaque cursor returned by read_after().
class LogBuffer {
public:
  explicit LogBuffer(std::size_t byte_limit = 4 * 1024 * 1024);
  /// Appends bytes; chunk boundaries need not match visible log lines.
  void append(LogStream stream, std::string_view bytes);
  /// Flushes an incomplete UTF-8 suffix when one output stream closes.
  void finish(LogStream stream);
  /// Returns subsequent chunks within a byte budget; truncated also signals
  /// that an older cursor has fallen out of the ring.
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

} // namespace xrm
