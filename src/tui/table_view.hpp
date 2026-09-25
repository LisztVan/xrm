#pragma once

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/string.hpp>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace xrm::tui {

// A small presentational table. The same resolved column layout renders the
// header and every row; the caller owns selection, paging, and business data.
template <class Row>
class TableView {
 public:
  struct Column {
    std::string title;
    int min_width;
    int priority;       // Larger values disappear first in narrow terminals.
    int stretch_weight; // Extra width goes to readable text, normally Name.
    bool right_align;
    std::function<std::string(const Row&)> value;
  };

  TableView(std::vector<Column> columns, int available_width,
            bool selection_marker = true)
      : columns_(std::move(columns)), selection_marker_(selection_marker) {
    if (selection_marker_) available_width -= 3;
    widths_.resize(columns_.size());
    for (std::size_t i = 0; i < columns_.size(); ++i)
      widths_[i] = std::max(1, columns_[i].min_width);
    auto used = [&] {
      int total = 0;
      for (auto width : widths_) total += width;
      return total;
    };
    while (used() > available_width) {
      auto hidden = columns_.size();
      for (std::size_t i = 0; i < columns_.size(); ++i)
        if (widths_[i] && (hidden == columns_.size() ||
            columns_[i].priority > columns_[hidden].priority)) hidden = i;
      if (hidden == columns_.size() || columns_[hidden].priority == 0) break;
      widths_[hidden] = 0;
    }
    while (used() < available_width) {
      auto stretch = columns_.size();
      for (std::size_t i = 0; i < columns_.size(); ++i)
        if (widths_[i] && columns_[i].stretch_weight > 0 &&
            (stretch == columns_.size() ||
             columns_[i].stretch_weight > columns_[stretch].stretch_weight)) stretch = i;
      if (stretch == columns_.size()) break;
      widths_[stretch] += available_width - used();
    }
  }

  ftxui::Element header() const {
    ftxui::Elements cells;
    if (selection_marker_) cells.push_back(cell("", 3, false));
    for (std::size_t i = 0; i < columns_.size(); ++i)
      if (widths_[i]) cells.push_back(cell(columns_[i].title, widths_[i],
                                          columns_[i].right_align));
    return ftxui::hbox(std::move(cells)) | ftxui::bold;
  }

  ftxui::Element row(const Row& value, bool selected) const {
    ftxui::Elements cells;
    if (selection_marker_) cells.push_back(cell(selected ? "▸" : "", 3, false));
    for (std::size_t i = 0; i < columns_.size(); ++i)
      if (widths_[i]) cells.push_back(cell(columns_[i].value(value), widths_[i],
                                          columns_[i].right_align));
    auto line = ftxui::hbox(std::move(cells));
    if (selected) line = line | ftxui::inverted | ftxui::focus;
    return line;
  }

 private:
  static std::string clipped(std::string_view value, int available) {
    if (available <= 0) return {};
    if (ftxui::string_width(value) <= available) return std::string(value);
    std::string result;
    int occupied = 0;
    for (const auto& glyph : ftxui::Utf8ToGlyphs(value)) {
      const int glyph_width = ftxui::string_width(glyph);
      if (occupied + glyph_width > available - 1) break;
      result += glyph;
      occupied += glyph_width;
    }
    result += "…";
    return result;
  }
  static ftxui::Element cell(std::string value, int width, bool right_align) {
    const int content_width = std::max(0, width - 1);
    auto content = ftxui::text(clipped(value, content_width));
    if (right_align) content = ftxui::align_right(std::move(content));
    return ftxui::hbox({std::move(content) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, content_width),
                       ftxui::text(" ")});
  }
  std::vector<Column> columns_;
  std::vector<int> widths_;
  bool selection_marker_ = true;
};

}  // namespace xrm::tui
