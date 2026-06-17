#pragma once

#include <ncurses.h>

#include <string>
#include <vector>

namespace meshcli {

// Single-line input editor with cursor + history. Backed by ncurses getch.
class InputLine {
public:
    InputLine() = default;

    // Process one keystroke. Returns true if the input should be submitted
    // (and `out` filled with the line); false otherwise.
    bool handle_key(int ch, std::string& out);

    [[nodiscard]] const std::string& buf() const { return buf_; }
    [[nodiscard]] size_t cursor() const { return cursor_; }

    void clear() { buf_.clear(); cursor_ = 0; history_pos_ = history_.size(); }

private:
    std::string buf_;
    size_t cursor_ = 0;
    std::vector<std::string> history_;
    size_t history_pos_ = 0;
};

} // namespace meshcli
