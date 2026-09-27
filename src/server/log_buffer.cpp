/// @file
/// @brief Stores the bounded in-memory daemon and core log stream.

#include "server/log_buffer.hpp"

#include <algorithm>

namespace xrm {
namespace {
struct Unit {
  std::size_t bytes;
  bool valid;
};
// A zero length is an unfinished, otherwise valid UTF-8 prefix.
Unit utf8_unit(std::string_view text) {
  const auto lead = static_cast<unsigned char>(text.front());
  if (lead < 0x80)
    return {1, true};
  const std::size_t size = lead >= 0xc2 && lead <= 0xdf   ? 2
                           : lead >= 0xe0 && lead <= 0xef ? 3
                           : lead >= 0xf0 && lead <= 0xf4 ? 4
                                                          : 0;
  if (!size)
    return {1, false};
  for (std::size_t i = 1; i < std::min(size, text.size()); ++i) {
    const auto ch = static_cast<unsigned char>(text[i]);
    if (ch < 0x80 || ch > 0xbf ||
        (i == 1 &&
         ((lead == 0xe0 && ch < 0xa0) || (lead == 0xed && ch >= 0xa0) ||
          (lead == 0xf0 && ch < 0x90) || (lead == 0xf4 && ch >= 0x90))))
      return {1, false};
  }
  return text.size() < size ? Unit{0, false} : Unit{size, true};
}

// JSON strings cannot carry malformed UTF-8. Keep raw bytes in the ring and
// expose a visible byte spelling for malformed input in the text protocol.
std::string transport_text(std::string_view raw) {
  constexpr char hex[] = "0123456789ABCDEF";
  std::string text;
  text.reserve(raw.size());
  while (!raw.empty()) {
    const auto unit = utf8_unit(raw);
    if (unit.valid) {
      text.append(raw.substr(0, unit.bytes));
      raw.remove_prefix(unit.bytes);
    } else {
      const auto ch = static_cast<unsigned char>(raw.front());
      text += "\\x";
      text += hex[ch >> 4];
      text += hex[ch & 15];
      raw.remove_prefix(1);
    }
  }
  return text;
}
} // namespace

LogBuffer::LogBuffer(std::size_t byte_limit)
    : byte_limit_(std::max<std::size_t>(1, byte_limit)) {}

void LogBuffer::append(LogStream stream, std::string_view bytes) {
  if (bytes.empty())
    return;
  auto &pending = pending_utf8_[static_cast<std::size_t>(stream)];
  std::string joined;
  if (!pending.empty()) {
    joined = std::move(pending);
    joined.append(bytes);
    bytes = joined;
    pending.clear();
  }
  const auto chunk_limit = std::min<std::size_t>(16 * 1024, byte_limit_);
  while (!bytes.empty()) {
    std::size_t count = 0;
    while (count < bytes.size()) {
      const auto unit = utf8_unit(bytes.substr(count));
      if (!unit.bytes)
        break;
      if (count && count + unit.bytes > chunk_limit)
        break;
      count += unit.bytes;
      if (count >= chunk_limit)
        break;
    }
    if (!count) {
      pending = bytes;
      break;
    }
    entries_.push_back(
        LogEntry{next_cursor_++, stream, std::string(bytes.substr(0, count))});
    bytes_retained_ += count;
    bytes.remove_prefix(count);
    while (bytes_retained_ > byte_limit_ && !entries_.empty()) {
      bytes_retained_ -= entries_.front().text.size();
      entries_.pop_front();
    }
  }
}

void LogBuffer::finish(LogStream stream) {
  auto &pending = pending_utf8_[static_cast<std::size_t>(stream)];
  if (pending.empty())
    return;
  bytes_retained_ += pending.size();
  entries_.push_back(LogEntry{next_cursor_++, stream, std::move(pending)});
  pending.clear();
  while (bytes_retained_ > byte_limit_ && !entries_.empty()) {
    bytes_retained_ -= entries_.front().text.size();
    entries_.pop_front();
  }
}

LogPage LogBuffer::read_after(std::uint64_t cursor,
                              std::size_t byte_limit) const {
  LogPage page;
  page.next_cursor = cursor;
  if (!entries_.empty() && cursor + 1 < entries_.front().cursor)
    page.truncated = true;
  std::size_t bytes = 0;
  for (const auto &entry : entries_) {
    if (entry.cursor <= cursor)
      continue;
    auto visible = entry;
    visible.text = transport_text(entry.text);
    if (!page.entries.empty() &&
        (visible.text.size() > byte_limit - std::min(byte_limit, bytes)))
      break;
    if (visible.text.size() > byte_limit && page.entries.empty()) {
      auto clipped = std::move(visible);
      std::size_t length = 0;
      while (length < clipped.text.size()) {
        const auto unit =
            utf8_unit(std::string_view(clipped.text).substr(length));
        if (!unit.bytes || length + unit.bytes > byte_limit)
          break;
        length += unit.bytes;
      }
      clipped.text.resize(length);
      page.entries.push_back(std::move(clipped));
      page.next_cursor = entry.cursor;
      page.truncated = true;
      break;
    }
    bytes += visible.text.size();
    page.entries.push_back(std::move(visible));
    page.next_cursor = entry.cursor;
  }
  return page;
}

std::uint64_t LogBuffer::cursor() const { return next_cursor_ - 1; }
std::size_t LogBuffer::bytes_retained() const { return bytes_retained_; }

} // namespace xrm
