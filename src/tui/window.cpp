#include "window.h"

#include <string>

namespace meshcli {

Window::Window(WindowTarget t, std::string title)
    : target_(std::move(t)), title_(std::move(title)) {}

void Window::append_line(Line line) {
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

} // namespace meshcli
