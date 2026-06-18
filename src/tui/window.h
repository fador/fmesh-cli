#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace meshcli {

// One logical irssi-style window: a scrollback buffer + a routing target.
//   kind="status"  : the singleton status window
//   kind="channel" : a broadcast channel window (target = channel index)
//   kind="dm"      : a direct-message window (target = peer node num)
struct WindowTarget {
    std::string device;     // DeviceId
    std::string kind;       // "status" | "channel" | "dm"
    uint32_t target = 0;    // channel idx or peer node num

    bool operator==(const WindowTarget& o) const {
        return device == o.device && kind == o.kind && target == o.target;
    }
};

// A single rendered line in the scrollback. We store formatted lines so the
// window can redraw cheaply and so scrollback search works on the rendered
// text rather than raw fields.
struct Line {
    std::string text;         // already formatted (timestamp + nick + body)
    int color_pair = 0;       // ncurses color pair index, 0 = default
    bool is_meta = false;     // *** join/part/info line
    uint32_t sender_node = 0; // node number of sender (for retroactive nick updates)
};

class Window {
public:
    explicit Window(WindowTarget t, std::string title);

    void append_line(Line line);
    void append_meta(std::string text, int color_pair = 0);
    void clear();

    [[nodiscard]] const std::vector<Line>& lines() const { return lines_; }
    [[nodiscard]] const WindowTarget& target() const { return target_; }
    [[nodiscard]] const std::string& title() const { return title_; }
    void set_title(std::string t) { title_ = std::move(t); }

    // Rebuild display nicks for all lines belonging to a given sender node.
    void rebuild_nick(uint32_t sender_node, const std::string& old_nick,
                      const std::string& new_nick);

    // Unread bookkeeping (driven by the window manager).
    void mark_read() { unread_ = 0; activity_ = 0; }
    void bump_activity(int level) { if (level > activity_) activity_ = level; ++unread_; }
    [[nodiscard]] int activity() const { return activity_; }
    [[nodiscard]] int unread() const { return unread_; }

    // Scroll offset (in lines from the bottom). 0 = tail.
    void scroll_by(int delta);
    void scroll_to_bottom();
    [[nodiscard]] int scroll_offset() const { return scroll_offset_; }

private:
    WindowTarget target_;
    std::string title_;
    std::vector<Line> lines_;
    int unread_ = 0;
    int activity_ = 0;   // 0 none, 1 message, 2 mention
    int scroll_offset_ = 0;
};

} // namespace meshcli
