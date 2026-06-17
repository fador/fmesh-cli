#include "status_bar.h"

#include <ncurses.h>

#include <cstdio>
#include <ctime>
#include <string>

namespace meshcli {

namespace {
}

std::string StatusBar::window_label(const Window& w) {
    if (w.target().kind == "status") return "status";
    return w.title();
}

void StatusBar::render(WindowManager& wm, int cols, const std::string& connection_info) {
    int y = LINES - 1;
    (void)y;

    // Build the window list segment: "Act: 1:status 2:#primary 3:Bob"
    std::string left = "Act: ";
    bool any_active = false;
    const auto& wins = wm.windows();
    for (size_t i = 0; i < wins.size(); ++i) {
        int idx = static_cast<int>(i + 1);
        const Window& w = *wins[i];
        if (w.activity() == 0) continue;
        any_active = true;
        char mark = (w.activity() >= 2) ? '*' : '#';
        left += std::to_string(idx) + mark + window_label(w) + " ";
    }
    if (!any_active) left += "(none)";

    // Clock.
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    ::localtime_r(&t, &tm);
    char clk[8];
    std::snprintf(clk, sizeof(clk), "%02d:%02d", tm.tm_hour, tm.tm_min);

    std::string right = clk;
    if (!connection_info.empty()) right = connection_info + "  " + right;

    // Render with the active window number highlighted.
    attron(A_REVERSE);
    mvhline(LINES - 1, 0, ' ', cols);

    // Active window highlight on the left of the bar.
    std::string active_str = "[" + std::to_string(wm.current_index()) + ":" +
                             (wm.current_window() ? window_label(*wm.current_window()) : "?") + "] ";
    mvprintw(LINES - 1, 0, "%s", active_str.c_str());
    int x = static_cast<int>(active_str.size());
    mvprintw(LINES - 1, x, "%s", left.c_str());
    x += static_cast<int>(left.size());

    // Right-aligned clock/connection.
    int right_x = cols - static_cast<int>(right.size());
    if (right_x > x) {
        mvprintw(LINES - 1, right_x, "%s", right.c_str());
    }
    attroff(A_REVERSE);
}

} // namespace meshcli
