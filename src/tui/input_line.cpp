#include "input_line.h"

#include <algorithm>

namespace meshcli {

bool InputLine::handle_key(int ch, std::string& out) {
    if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
        if (buf_.empty()) return false;
        history_.push_back(buf_);
        history_pos_ = history_.size();
        out = buf_;
        buf_.clear();
        cursor_ = 0;
        return true;
    }
    if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
        if (cursor_ > 0) {
            buf_.erase(cursor_ - 1, 1);
            --cursor_;
        }
        return false;
    }
    if (ch == KEY_DC) {  // delete
        if (cursor_ < buf_.size()) buf_.erase(cursor_, 1);
        return false;
    }
    if (ch == KEY_LEFT)  { if (cursor_ > 0) --cursor_; return false; }
    if (ch == KEY_RIGHT) { if (cursor_ < buf_.size()) ++cursor_; return false; }
    if (ch == KEY_HOME || ch == 1) { cursor_ = 0; return false; }
    if (ch == KEY_END  || ch == 5) { cursor_ = buf_.size(); return false; }
    if (ch == KEY_UP) {
        if (!history_.empty() && history_pos_ > 0) {
            --history_pos_;
            buf_ = history_[history_pos_];
            cursor_ = buf_.size();
        }
        return false;
    }
    if (ch == KEY_DOWN) {
        if (history_pos_ < history_.size()) {
            ++history_pos_;
            if (history_pos_ < history_.size()) buf_ = history_[history_pos_];
            else buf_.clear();
            cursor_ = buf_.size();
        }
        return false;
    }
    if (ch == 23) {  // Ctrl-W: delete previous word
        if (cursor_ > 0) {
            size_t end = cursor_;
            size_t start = end;
            while (start > 0 && std::isspace(static_cast<unsigned char>(buf_[start - 1]))) --start;
            while (start > 0 && !std::isspace(static_cast<unsigned char>(buf_[start - 1]))) --start;
            buf_.erase(start, end - start);
            cursor_ = start;
        }
        return false;
    }
    if (ch == 21) {  // Ctrl-U: clear line
        buf_.clear();
        cursor_ = 0;
        return false;
    }
    if (ch == 11) {  // Ctrl-K: kill to end
        buf_.erase(cursor_);
        return false;
    }

    // Printable ASCII (skip control chars / multibyte for v1).
    if (ch >= 0x20 && ch < 0x7F) {
        buf_.insert(buf_.begin() + cursor_, static_cast<char>(ch));
        ++cursor_;
    }
    return false;
}

} // namespace meshcli
