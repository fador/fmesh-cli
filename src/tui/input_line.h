#pragma once

#include <ncurses.h>

#include <cstddef>
#include <string>
#include <vector>

namespace meshcli {

// Single-line input editor with cursor + history. Backed by ncurses getch.
class InputLine {
public:
    static constexpr size_t kMaxLineSize = 8192;
    static constexpr size_t kMaxHistory = 10000;
    InputLine() = default;

    // Process one keystroke. Returns true if the input should be submitted
    // (and `out` filled with the line); false otherwise.
    bool handle_key(int ch, std::string& out);

    [[nodiscard]] const std::string& buf() const { return buf_; }
    [[nodiscard]] size_t cursor() const { return cursor_; }

    void clear() { buf_.clear(); cursor_ = 0; history_pos_ = history_.size(); }

    // Persist history to/from a file.
    void load_history(const std::string& path);
    void save_history(const std::string& path) const;

private:
    std::string buf_;
    size_t cursor_ = 0;
    std::vector<std::string> history_;
    size_t history_pos_ = 0;
};

} // namespace meshcli
