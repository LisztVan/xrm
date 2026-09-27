/// @file
/// @brief Defines human-readable CLI formatting helpers.

#pragma once

#include <ftxui/screen/string.hpp>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <numeric>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

namespace xrm::cli {

using HumanRows = std::vector<std::vector<std::string>>;

inline int terminal_width() {
  winsize size{};
  return ::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col
             ? std::max(20, static_cast<int>(size.ws_col) - 2)
             : 118;
}

inline std::string terminal_text(std::string value) {
  for (auto &ch : value)
    if (static_cast<unsigned char>(ch) < 32 || ch == 127)
      ch = ' ';
  return value;
}

inline std::vector<std::string> wrap_cell(const std::string &value, int width) {
  std::vector<std::string> lines(1);
  int used = 0;
  for (std::size_t start = 0; start <= value.size();) {
    const auto end = value.find('\n', start);
    const auto segment =
        value.substr(start, end == std::string::npos ? end : end - start);
    for (const auto &glyph : ftxui::Utf8ToGlyphs(terminal_text(segment))) {
      if (glyph.empty())
        continue;
      const int cells = ftxui::string_width(glyph);
      if (used && used + cells > width) {
        lines.emplace_back();
        used = 0;
      }
      lines.back() += glyph;
      used += cells;
    }
    if (end == std::string::npos)
      break;
    lines.emplace_back();
    used = 0;
    start = end + 1;
  }
  return lines;
}

inline void human_details(const std::string &title, const HumanRows &fields,
                          bool color = false) {
  if (color)
    std::cout << "\x1b[1;36m";
  for (const auto &line : wrap_cell(title, terminal_width()))
    std::cout << line << '\n';
  if (color)
    std::cout << "\x1b[0m";
  std::cout << '\n';
  int label_width = 0;
  for (const auto &field : fields)
    label_width = std::max(label_width, ftxui::string_width(field[0]));
  for (const auto &field : fields) {
    const auto lines =
        wrap_cell(field[1], std::max(8, terminal_width() - label_width - 4));
    for (std::size_t i = 0; i < lines.size(); ++i) {
      if (i)
        std::cout << std::string(static_cast<std::size_t>(label_width + 4),
                                 ' ');
      else
        std::cout << "  " << field[0]
                  << std::string(
                         static_cast<std::size_t>(
                             label_width - ftxui::string_width(field[0]) + 2),
                         ' ');
      std::cout << lines[i] << '\n';
    }
  }
}

// Terminal-only presentation. Callers keep their established pipe format.
// Full identifiers and paths wrap; they are never shortened or elided.
inline void human_table(const std::vector<std::string> &headings,
                        const HumanRows &rows, const std::string &empty,
                        bool color = false) {
  if (rows.empty()) {
    std::cout << empty << '\n';
    return;
  }
  const int available = terminal_width();
  std::vector<int> widths;
  for (const auto &heading : headings)
    widths.push_back(std::max(8, ftxui::string_width(heading)));
  auto minimums = widths;
  for (std::size_t i = 0; i < headings.size(); ++i)
    if (headings[i] == "ID" || headings[i].ends_with(" ID"))
      minimums[i] = widths[i] = 36;
  const int minimum = std::accumulate(minimums.begin(), minimums.end(), 0) +
                      2 * static_cast<int>(minimums.size() - 1);
  if (minimum > available) {
    bool first = true;
    for (const auto &row : rows) {
      if (!first)
        std::cout << '\n';
      first = false;
      if (color)
        std::cout << "\x1b[1;36m";
      for (const auto &part : wrap_cell(row.front(), available))
        std::cout << part << '\n';
      if (color)
        std::cout << "\x1b[0m";
      for (std::size_t i = 1; i < headings.size(); ++i) {
        auto label = headings[i];
        if (label != "ID") {
          const auto end =
              label.ends_with(" ID") ? label.size() - 3 : label.size();
          for (std::size_t n = 1; n < end; ++n)
            label[n] = static_cast<char>(
                std::tolower(static_cast<unsigned char>(label[n])));
        }
        const auto prefix = "  " + label + ": ";
        const auto parts = wrap_cell(
            row[i], std::max(8, available - static_cast<int>(prefix.size())));
        for (std::size_t y = 0; y < parts.size(); ++y)
          std::cout << (y ? std::string(prefix.size(), ' ') : prefix)
                    << parts[y] << '\n';
      }
    }
    return;
  }
  for (const auto &row : rows)
    for (std::size_t i = 0; i < headings.size(); ++i)
      widths[i] = std::max(
          widths[i], std::min(48, ftxui::string_width(terminal_text(row[i]))));
  auto total = [&] {
    return std::accumulate(widths.begin(), widths.end(), 0) +
           2 * static_cast<int>(widths.size() - 1);
  };
  while (total() > available) {
    std::size_t widest = 0;
    int spare = 0;
    for (std::size_t i = 0; i < widths.size(); ++i)
      if (widths[i] - minimums[i] > spare) {
        widest = i;
        spare = widths[i] - minimums[i];
      }
    if (!spare)
      break;
    --widths[widest];
  }
  const auto line = [&](const std::vector<std::string> &values, bool header) {
    std::vector<std::vector<std::string>> cells;
    std::size_t height = 1;
    for (std::size_t i = 0; i < values.size(); ++i) {
      cells.push_back(wrap_cell(values[i], widths[i]));
      height = std::max(height, cells.back().size());
    }
    if (header && color)
      std::cout << "\x1b[1;36m";
    for (std::size_t y = 0; y < height; ++y) {
      for (std::size_t i = 0; i < values.size(); ++i) {
        const auto text = y < cells[i].size() ? cells[i][y] : std::string{};
        std::cout << text;
        if (i + 1 != values.size())
          std::cout << std::string(
              static_cast<std::size_t>(
                  std::max(0, widths[i] - ftxui::string_width(text)) + 2),
              ' ');
      }
      std::cout << '\n';
    }
    if (header && color)
      std::cout << "\x1b[0m";
  };
  line(headings, true);
  for (const auto &row : rows)
    line(row, false);
}

} // namespace xrm::cli
