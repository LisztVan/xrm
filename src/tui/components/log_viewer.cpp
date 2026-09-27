#include "tui/components/widgets.hpp"
#include "tui/components/log_viewer.hpp"
#include "tui/components/style.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <string_view>
#include <utility>

#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>

namespace xrm::tui::ui {
using namespace ftxui;
std::string LogTextForDisplay(std::string_view value, bool multiline) {
  constexpr char hex[] = "0123456789ABCDEF";
  std::string result;
  auto escaped_byte = [&](unsigned char ch) {
    result += "\\x";
    result += hex[ch >> 4];
    result += hex[ch & 15];
  };
  for (std::size_t i = 0; i < value.size();) {
    const auto ch = static_cast<unsigned char>(value[i]);
    if (ch < 128) {
      ++i;
      if (ch == '\n' && multiline)
        result += '\n';
      else if (ch == '\t')
        result += "    ";
      else if (ch < 32 || ch == 127) {
        result += '^';
        result += ch == 127 ? '?' : static_cast<char>(ch + 64);
      } else
        result += static_cast<char>(ch);
      continue;
    }
    const std::size_t count = ch >= 0xc2 && ch <= 0xdf ? 2 :
                              ch >= 0xe0 && ch <= 0xef ? 3 :
                              ch >= 0xf0 && ch <= 0xf4 ? 4 : 0;
    bool valid = count && i + count <= value.size();
    for (std::size_t j = 1; valid && j < count; ++j) {
      const auto next = static_cast<unsigned char>(value[i + j]);
      valid = next >= 0x80 && next <= 0xbf;
      if (j == 1)
        valid = valid && !(ch == 0xe0 && next < 0xa0) &&
                !(ch == 0xed && next >= 0xa0) &&
                !(ch == 0xf0 && next < 0x90) &&
                !(ch == 0xf4 && next >= 0x90);
    }
    if (!valid) {
      escaped_byte(ch);
      ++i;
    } else if (ch == 0xc2 && static_cast<unsigned char>(value[i + 1]) <= 0x9f) {
      // Unicode C1 controls include CSI/OSC/ST; show their code point.
      result += "\\u00";
      const auto control = static_cast<unsigned char>(value[i + 1]);
      result += hex[control >> 4];
      result += hex[control & 15];
      i += count;
    } else {
      result.append(value, i, count);
      i += count;
    }
  }
  return result;
}
namespace {
std::string visible_text(std::string_view value) {
  return LogTextForDisplay(value);
}

bool digits(std::string_view value) {
  return !value.empty() && std::all_of(value.begin(), value.end(), [](char ch) {
    return ch >= '0' && ch <= '9';
  });
}

LogRecord parse_record(std::string raw, std::string source,
                       std::uint64_t sequence, std::size_t line, bool partial) {
  LogRecord result;
  result.sequence = sequence;
  result.line = line;
  result.source = std::move(source);
  result.raw = raw;
  result.partial = partial;
  std::string_view rest(raw);
  if (rest.ends_with('\r')) rest.remove_suffix(1);
  // Recognize Xray's actual date/time prefix, without assigning arrival time
  // to records that have no native timestamp.
  if (rest.size() >= 19 && digits(rest.substr(0, 4)) &&
      (rest[4] == '/' || rest[4] == '-') && rest[7] == rest[4] &&
      digits(rest.substr(5, 2)) && digits(rest.substr(8, 2)) &&
      (rest[10] == ' ' || rest[10] == 'T') &&
      digits(rest.substr(11, 2)) && rest[13] == ':' &&
      digits(rest.substr(14, 2)) && rest[16] == ':' &&
      digits(rest.substr(17, 2))) {
    std::size_t end = 19;
    if (end < rest.size() && rest[end] == '.') {
      ++end;
      while (end < rest.size() && std::isdigit(static_cast<unsigned char>(rest[end])))
        ++end;
    }
    if (end < rest.size() && rest[end] == 'Z')
      ++end;
    if (end == rest.size() || rest[end] == ' ') {
      result.timestamp = rest.substr(0, end);
      rest.remove_prefix(end);
      while (rest.starts_with(' '))
        rest.remove_prefix(1);
    }
  }
  if (rest.starts_with('[')) {
    const auto end = rest.find(']');
    if (end != std::string_view::npos) {
      const auto level = rest.substr(1, end - 1);
      if (level == "Debug" || level == "Info" || level == "Warning" ||
          level == "Error" || level == "Fatal") {
        result.level = level;
        rest.remove_prefix(end + 1);
        while (rest.starts_with(' '))
          rest.remove_prefix(1);
      }
    }
  }
  // Only a recognized severity prefix authorizes parsing Xray's subsystem.
  // Numeric request IDs remain visible in the message.
  std::string request;
  if (!result.level.empty()) {
    auto body = rest;
    if (body.starts_with('[')) {
      const auto end = body.find(']');
      if (end != std::string_view::npos && digits(body.substr(1, end - 1))) {
        request = std::string(body.substr(0, end + 1)) + " ";
        body.remove_prefix(end + 1);
        while (body.starts_with(' '))
          body.remove_prefix(1);
      }
    }
    const auto colon = body.find(": ");
    if (colon != std::string_view::npos) {
      const auto subsystem = body.substr(0, colon);
      if (subsystem.find('/') != std::string_view::npos &&
          subsystem.find_first_of(" \t[]") == std::string_view::npos) {
        result.subsystem = subsystem;
        rest = body.substr(colon + 2);
      } else
        request.clear();
    } else
      request.clear();
  }
  result.message = request + std::string(rest);
  return result;
}

// Wrap by terminal cells, including single tokens (URLs, hashes, addresses).
// FTXUI paragraph only wraps between words and otherwise clips long tokens.
std::vector<std::string> wrap(std::string_view message, int width) {
  std::vector<std::string> lines;
  for (const auto &part : SplitLines(std::string(message))) {
    std::string current;
    int used = 0;
    for (const auto &glyph : Utf8ToGlyphs(visible_text(part))) {
      if (glyph.empty())
        continue;
      const int cells = string_width(glyph);
      if (used + cells > width && !current.empty()) {
        const auto space = current.rfind(' ');
        if (space != std::string::npos &&
            string_width(current.substr(0, space)) >= width / 2) {
          lines.push_back(current.substr(0, space + 1));
          current.erase(0, space + 1);
          used = string_width(current);
        } else {
          lines.push_back(std::move(current));
          current.clear();
          used = 0;
        }
      }
      current += glyph;
      used += cells;
    }
    lines.push_back(std::move(current));
  }
  if (lines.empty())
    lines.emplace_back();
  return lines;
}

Element column(std::string value, int width, style::Role role) {
  // Reserve the final cell as a gutter even when a cursor or subsystem exceeds
  // its column. Do not rely on clipping: adjacent metadata must stay separate.
  const int available = std::max(0, width - 1);
  if (string_width(value) > available) {
    std::string shortened;
    int used = 0;
    for (const auto &glyph : Utf8ToGlyphs(value)) {
      if (glyph.empty())
        continue;
      const int cells = string_width(glyph);
      if (used + cells > available - 1)
        break;
      shortened += glyph;
      used += cells;
    }
    value = std::move(shortened) + "…";
  }
  value.append(std::max(0, width - string_width(value)), ' ');
  return text(std::move(value)) | style::ink(role) |
         size(WIDTH, EQUAL, width);
}
std::string source_label(const std::string &source) {
  if (source == "core_stdout")
    return "stdout";
  if (source == "core_stderr")
    return "stderr";
  return visible_text(source);
}
style::Role severity(const std::string &level) {
  if (level == "Error" || level == "Fatal")
    return style::Role::Error;
  if (level == "Warning")
    return style::Role::Warning;
  return style::Role::Muted;
}
bool same_identity(const LogRecord &a, const LogRecord &b) {
  return a.sequence == b.sequence && a.line == b.line && a.source == b.source;
}
} // namespace

std::vector<LogRecord> ParseLogChunks(const std::vector<LogChunk> &chunks) {
  struct Pending {
    std::string bytes;
    std::uint64_t sequence = 0;
    std::size_t line = 0;
  };
  std::map<std::string, Pending> streams;
  std::vector<LogRecord> records;
  std::uint64_t previous = 0;
  for (const auto &chunk : chunks) {
    if (previous && chunk.sequence > previous + 1) {
      // A lost transport span must not join unrelated partial text.
      for (auto &[source, pending] : streams) {
        if (!pending.bytes.empty())
          records.push_back(parse_record(std::move(pending.bytes), source,
                                        pending.sequence, pending.line, true));
        pending.bytes.clear();
      }
    }
    previous = chunk.sequence;
    auto &pending = streams[chunk.source];
    std::size_t offset = 0, chunk_line = 0;
    while (offset < chunk.text.size()) {
      if (pending.bytes.empty()) {
        pending.sequence = chunk.sequence;
        pending.line = chunk_line;
      }
      auto end = chunk.text.find('\n', offset);
      pending.bytes.append(chunk.text, offset,
                           end == std::string::npos ? end : end - offset);
      if (end == std::string::npos)
        break;
      records.push_back(parse_record(std::move(pending.bytes), chunk.source,
                                    pending.sequence, pending.line, false));
      pending.bytes.clear();
      offset = end + 1;
      ++chunk_line;
    }
  }
  for (auto &[source, pending] : streams)
    if (!pending.bytes.empty())
      records.push_back(parse_record(std::move(pending.bytes), source,
                                    pending.sequence, pending.line, true));
  std::stable_sort(records.begin(), records.end(), [](const auto &a, const auto &b) {
    return a.sequence < b.sequence ||
           (a.sequence == b.sequence && a.line < b.line);
  });
  return records;
}

void LogViewer::set_lines(std::vector<std::string> lines) {
  std::vector<LogRecord> records;
  for (std::size_t i = 0; i < lines.size(); ++i)
    records.push_back(parse_record(std::move(lines[i]), "", 0, i, false));
  set_records(std::move(records));
}

void LogViewer::set_records(std::vector<LogRecord> records) {
  if (!follow_) {
    // A paused screen is a frozen bounded snapshot; do not shift its viewport
    // when the daemon ring advances or a partial line finishes.
    auto last = incoming_.empty()
                    ? records.end()
                    : std::find_if(records.begin(), records.end(), [&](const auto &entry) {
                        return same_identity(entry, incoming_.back());
                      });
    unseen_ += last == records.end() ? records.size()
                                    : std::size_t(records.end() - last - 1);
    incoming_ = std::move(records);
    return;
  }
  records_ = std::move(records);
  cursor_ = std::max(0, int(records_.size()) - 1);
  unseen_ = 0;
}

void LogViewer::set_follow(bool follow) {
  if (follow == follow_)
    return;
  if (follow) {
    records_ = std::move(incoming_);
    cursor_ = std::max(0, int(records_.size()) - 1);
    unseen_ = 0;
  } else
    incoming_ = records_;
  follow_ = follow;
}

const LogRecord *LogViewer::selected_record() const {
  return records_.empty() ? nullptr : &records_[cursor_];
}

Element LogViewer::OnRender() {
  if (records_.empty())
    return text("No log entries received") | style::ink(style::Role::Muted);
  const int width = std::max(30, Terminal::Size().dimx - 5);
  const bool wide = width >= 110;
  const int metadata_width = 9 + 9 + 8;
  const int subsystem_width = wide ? 22 : 0;
  const int message_width = std::max(8, width - metadata_width - subsystem_width - 2);
  auto metadata = [&](std::string time, std::string source,
                      std::string level, style::Role level_role) {
    return Elements{column(std::move(time), 9, style::Role::Muted),
                    column(std::move(source), 9, style::Role::Muted),
                    column(std::move(level), 8, level_role)};
  };
  auto headers = metadata("TIME", "SOURCE", "LEVEL", style::Role::Muted);
  if (wide)
    headers.push_back(column("SUBSYSTEM", subsystem_width, style::Role::Muted));
  headers.push_back(text("  MESSAGE") | style::ink(style::Role::Muted) | xflex);
  Elements rows;
  for (std::size_t i = 0; i < records_.size(); ++i) {
    const auto &record = records_[i];
    const std::string message = !wide && !record.subsystem.empty()
                                    ? record.subsystem + ": " + record.message
                                    : record.message;
    auto parts = wrap(message, message_width);
    const bool active = static_cast<int>(i) == cursor_;
    std::size_t message_offset = 0;
    for (std::size_t p = 0; p < parts.size(); ++p) {
      Elements row;
      if (p == 0) {
        std::string time = record.timestamp.size() >= 19
                               ? record.timestamp.substr(11, 8) : "-";
        row = metadata(std::move(time),
                       source_label(record.source), record.level, severity(record.level));
      } else
        row.push_back(text("") | size(WIDTH, EQUAL, metadata_width));
      if (wide)
        row.push_back(column(p == 0 ? visible_text(record.subsystem) : "",
                             subsystem_width, style::Role::Muted));
      row.push_back(text(p ? "| " : "  ") | style::ink(style::Role::Muted));
      Element message_element;
      const std::size_t prefix_bytes = !wide && !record.subsystem.empty()
                                           ? visible_text(record.subsystem).size() + 2 : 0;
      if (message_offset < prefix_bytes) {
        const auto muted_bytes = std::min(parts[p].size(), prefix_bytes - message_offset);
        message_element = hbox({text(parts[p].substr(0, muted_bytes)) |
                                    style::ink(style::Role::Muted),
                                text(parts[p].substr(muted_bytes)) |
                                    style::ink(style::Role::Text) | xflex});
      } else
        message_element = text(parts[p]) | style::ink(style::Role::Text) | xflex;
      message_offset += parts[p].size();
      // The compact first line contains a nested hbox for the muted subsystem.
      // Give that outer box the same flex as continuation text, otherwise FTXUI
      // distributes spare cells between the metadata columns on this row only.
      message_element = message_element | xflex;
      if (record.level == "Error" || record.level == "Fatal")
        message_element = message_element | bold;
      row.push_back(std::move(message_element));
      auto rendered = hbox(std::move(row));
      if (active && !follow_)
        rendered = rendered | style::selected(Focused());
      if (active && p == (follow_ ? parts.size() - 1 : 0))
        rendered = rendered | focus;
      rows.push_back(std::move(rendered));
    }
  }
  return vbox({hbox(std::move(headers)), separator() | style::rule(),
               vbox(std::move(rows)) | vscroll_indicator | frame | flex});
}

bool LogViewer::OnEvent(Event event) {
  if (event == Event::Character('p')) {
    set_follow(!follow_);
    return true;
  }
  int movement = 0;
  if (event == Event::ArrowUp)
    movement = -1;
  if (event == Event::ArrowDown)
    movement = 1;
  if (event == Event::PageUp)
    movement = -10;
  if (event == Event::PageDown)
    movement = 10;
  if (event == Event::Home)
    movement = -static_cast<int>(records_.size());
  if (event == Event::End) {
    set_follow(true);
    return true;
  }
  if (movement) {
    set_follow(false);
    cursor_ = std::clamp(cursor_ + movement, 0,
                         std::max(0, static_cast<int>(records_.size()) - 1));
    return true;
  }
  return false;
}
} // namespace xrm::tui::ui
