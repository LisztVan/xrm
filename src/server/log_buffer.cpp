#include "server/log_buffer.hpp"

#include <algorithm>

namespace xrm {

LogBuffer::LogBuffer(std::size_t byte_limit) : byte_limit_(std::max<std::size_t>(1, byte_limit)) {}

void LogBuffer::append(LogStream stream, std::string_view bytes) {
  const auto chunk_limit = std::min<std::size_t>(16 * 1024, byte_limit_);
  while (!bytes.empty()) {
    const auto count = std::min(chunk_limit, bytes.size());
    entries_.push_back(LogEntry{next_cursor_++, stream, std::string(bytes.substr(0, count))});
    bytes_retained_ += count;
    bytes.remove_prefix(count);
    while (bytes_retained_ > byte_limit_ && !entries_.empty()) {
      bytes_retained_ -= entries_.front().text.size();
      entries_.pop_front();
    }
  }
}

LogPage LogBuffer::read_after(std::uint64_t cursor, std::size_t byte_limit) const {
  LogPage page;
  page.next_cursor = cursor;
  if (!entries_.empty() && cursor + 1 < entries_.front().cursor) page.truncated = true;
  std::size_t bytes = 0;
  for (const auto& entry : entries_) {
    if (entry.cursor <= cursor) continue;
    if (!page.entries.empty() && (entry.text.size() > byte_limit - std::min(byte_limit, bytes))) break;
    if (entry.text.size() > byte_limit && page.entries.empty()) {
      auto clipped = entry;
      clipped.text.resize(byte_limit);
      page.entries.push_back(std::move(clipped));
      page.next_cursor = entry.cursor;
      page.truncated = true;
      break;
    }
    bytes += entry.text.size();
    page.entries.push_back(entry);
    page.next_cursor = entry.cursor;
  }
  return page;
}

std::uint64_t LogBuffer::cursor() const { return next_cursor_ - 1; }
std::size_t LogBuffer::bytes_retained() const { return bytes_retained_; }

}  // namespace xrm
