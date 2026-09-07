#include "window.h"

#include <string>
#include <ctime>

namespace meshcli {

Window::Window(WindowTarget t, std::string title)
    : target_(std::move(t)), title_(std::move(title)) {}

void Window::append_line(Line line) {
    if (!line.is_meta && !lines_.empty()) {
        const auto& last = lines_.back();
        if (!last.is_meta && last.sender_node == line.sender_node) {
            auto pos_last = last.text.find("> ");
            auto pos_line = line.text.find("> ");
            if (pos_last != std::string::npos && pos_line != std::string::npos) {
                std::string body_last = last.text.substr(pos_last + 2);
                std::string body_line = line.text.substr(pos_line + 2);
                if (body_last.find(" [") != std::string::npos)
                    body_last = body_last.substr(0, body_last.rfind(" ["));
                if (body_line.find(" [") != std::string::npos)
                    body_line = body_line.substr(0, body_line.rfind(" ["));
                if (body_last == body_line) {
                    uint32_t dt = (line.ts >= last.ts) ? (line.ts - last.ts) : (last.ts - line.ts);
                    if (dt <= 2) return;
                }
            }
        }
    }

    if (line.ts != 0) {
        if (last_day_ts_ != 0) {
            std::time_t old_t = last_day_ts_;
            std::time_t new_t = line.ts;
            std::tm old_tm{}, new_tm{};
#ifdef _WIN32
            ::localtime_s(&old_tm, &old_t);
            ::localtime_s(&new_tm, &new_t);
#else
            ::localtime_r(&old_t, &old_tm);
            ::localtime_r(&new_t, &new_tm);
#endif
            if (old_tm.tm_mday != new_tm.tm_mday || 
                old_tm.tm_mon != new_tm.tm_mon || 
                old_tm.tm_year != new_tm.tm_year) {
                
                char datebuf[64];
                std::snprintf(datebuf, sizeof(datebuf), "--- %04d-%02d-%02d ---", 
                              new_tm.tm_year + 1900, new_tm.tm_mon + 1, new_tm.tm_mday);
                
                Line date_line;
                date_line.text = datebuf;
                date_line.is_meta = true;
                date_line.color_pair = 0; // default color
                date_line.ts = 0; // no ts for meta
                lines_.push_back(date_line);
            }
        } else {
            // First message with a timestamp
            std::time_t new_t = line.ts;
            std::tm new_tm{};
#ifdef _WIN32
            ::localtime_s(&new_tm, &new_t);
#else
            ::localtime_r(&new_t, &new_tm);
#endif
            char datebuf[64];
            std::snprintf(datebuf, sizeof(datebuf), "--- %04d-%02d-%02d ---", 
                          new_tm.tm_year + 1900, new_tm.tm_mon + 1, new_tm.tm_mday);
            Line date_line;
            date_line.text = datebuf;
            date_line.is_meta = true;
            date_line.color_pair = 0;
            date_line.ts = 0;
            lines_.push_back(date_line);
        }
        last_day_ts_ = line.ts;
    }
    
    lines_.push_back(std::move(line));
    // Cap scrollback to avoid unbounded memory.
    if (lines_.size() > 5000) {
        lines_.erase(lines_.begin(), lines_.begin() + (lines_.size() - 5000));
    }
}

void Window::append_meta(std::string text, int color_pair) {
    append_line(Line{std::move(text), color_pair, true});
}

void Window::rebuild_nick(uint32_t sender_node, const std::string& old_nick,
                          const std::string& new_nick) {
    if (old_nick == new_nick || old_nick.empty() || new_nick.empty()) return;
    std::string old_pattern = "<" + old_nick + "> ";
    std::string new_pattern = "<" + new_nick + "> ";
    std::string old_action = " * " + old_nick + " ";
    std::string new_action = " * " + new_nick + " ";
    for (auto& line : lines_) {
        if (line.sender_node != sender_node) continue;
        // Regular message: <nick> message
        size_t pos = line.text.find(old_pattern);
        if (pos != std::string::npos) {
            line.text.replace(pos, old_pattern.size(), new_pattern);
            continue;
        }
        // Action: * nick action
        pos = line.text.find(old_action);
        if (pos != std::string::npos) {
            line.text.replace(pos, old_action.size(), new_action);
        }
    }
}

void Window::clear() {
    lines_.clear();
    scroll_offset_ = 0;
}

void Window::scroll_by(int delta) {
    scroll_offset_ += delta;
    if (scroll_offset_ < 0) scroll_offset_ = 0;
    int max_offset = static_cast<int>(lines_.size());
    if (scroll_offset_ > max_offset) scroll_offset_ = max_offset;
}

void Window::scroll_to_bottom() {
    scroll_offset_ = 0;
}

std::vector<std::string> wrap_text(const std::string& text, int width) {
    std::vector<std::string> result;
    if (width <= 0) return result;
    if (text.empty()) {
        result.push_back("");
        return result;
    }

    if (text.find('\n') == std::string::npos && static_cast<int>(text.size()) <= width) {
        result.push_back(text);
        return result;
    }

    size_t line_start = 0;
    while (line_start <= text.size()) {
        size_t nl_pos = text.find('\n', line_start);
        std::string segment = (nl_pos == std::string::npos)
                                  ? text.substr(line_start)
                                  : text.substr(line_start, nl_pos - line_start);

        if (segment.empty()) {
            result.push_back("");
        } else if (static_cast<int>(segment.size()) <= width) {
            result.push_back(segment);
        } else {
            const std::string indent = (width > 15) ? "  " : "";
            const int indent_w = static_cast<int>(indent.size());
            size_t seg_pos = 0;
            bool first = true;

            while (seg_pos < segment.size()) {
                int avail = first ? width : (width - indent_w);
                if (avail <= 0) avail = 1;

                size_t remaining = segment.size() - seg_pos;
                if (static_cast<int>(remaining) <= avail) {
                    if (first) {
                        result.push_back(segment.substr(seg_pos));
                    } else {
                        result.push_back(indent + segment.substr(seg_pos));
                    }
                    break;
                }

                size_t max_chunk = static_cast<size_t>(avail);
                // Ensure we don't break in the middle of a multi-byte UTF-8 sequence
                while (max_chunk > 0 && (static_cast<unsigned char>(segment[seg_pos + max_chunk]) & 0xC0) == 0x80) {
                    max_chunk--;
                }
                if (max_chunk == 0) {
                    max_chunk = 1;
                    while (seg_pos + max_chunk < segment.size() &&
                           (static_cast<unsigned char>(segment[seg_pos + max_chunk]) & 0xC0) == 0x80) {
                        max_chunk++;
                    }
                }

                size_t brk = segment.rfind(' ', seg_pos + max_chunk);
                if (brk != std::string::npos && brk > seg_pos) {
                    std::string part = segment.substr(seg_pos, brk - seg_pos);
                    if (first) {
                        result.push_back(part);
                    } else {
                        result.push_back(indent + part);
                    }
                    seg_pos = brk;
                    while (seg_pos < segment.size() && segment[seg_pos] == ' ') {
                        seg_pos++;
                    }
                } else {
                    std::string part = segment.substr(seg_pos, max_chunk);
                    if (first) {
                        result.push_back(part);
                    } else {
                        result.push_back(indent + part);
                    }
                    seg_pos += max_chunk;
                }
                first = false;
            }
        }

        if (nl_pos == std::string::npos) break;
        line_start = nl_pos + 1;
    }
    return result;
}

} // namespace meshcli
