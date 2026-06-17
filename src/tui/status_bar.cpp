#include "status_bar.h"
#include "colors.h"

#include <ncurses.h>

#include <cstdio>
#include <ctime>
#include <string>

namespace meshcli {

namespace {
} // namespace

std::string StatusBar::window_label(const Window& w) {
    if (w.target().kind == "status") return "status";
    return w.title();
}

int StatusBar::color_for(const Window& w) {
    if (w.target().kind == "status")  return tui_color::META;
    if (w.target().kind == "channel") return tui_color::CHANNEL;
    if (w.target().kind == "dm")      return tui_color::DM;
    return 0;
}

void StatusBar::render(WindowManager& wm, int cols, const std::string& connection_info) {
    attron(A_REVERSE);
    mvhline(LINES - 1, 0, ' ', cols);

    // Current window indicator: [N:name]  (bold, colored)
    int x = 0;
    int cur = wm.current_index();
    Window* cw = wm.current_window();
    std::string cur_label = cw ? window_label(*cw) : "?";
    int cur_color = cw ? color_for(*cw) : 0;

    attron(COLOR_PAIR(cur_color) | A_BOLD);
    mvprintw(LINES - 1, x, "[%d:%s] ", cur, cur_label.c_str());
    x += static_cast<int>(std::snprintf(nullptr, 0, "[%d:%s] ", cur, cur_label.c_str()));
    attrset(A_REVERSE);   // reset to base reversed bar

    // Activity list: all windows with color-coded markers.
    //   active (message)   = N*  (mention = highest priority)
    //   active (unread)    = N#
    //   inactive           = N
    const auto& wins = wm.windows();
    for (size_t i = 0; i < wins.size(); ++i) {
        int idx = static_cast<int>(i + 1);
        const Window& w = *wins[i];
        char mark = ' ';
        int act = w.activity();
        if (act >= 2)      mark = '*';    // mention
        else if (act == 1) mark = '#';    // unread message
        int unread = w.unread();

        attron(COLOR_PAIR(color_for(w)) | A_REVERSE);
        if (unread > 0)
            mvprintw(LINES - 1, x, "%d%c%d%s ", idx, mark, unread, window_label(w).c_str());
        else
            mvprintw(LINES - 1, x, "%d%c%s ", idx, mark, window_label(w).c_str());
        x += static_cast<int>(std::snprintf(nullptr, 0,
                unread > 0 ? "%d%c%d%s " : "%d%c%s ",
                idx, mark, unread, window_label(w).c_str()));
        attrset(A_REVERSE);
    }

    // If no windows at all, show placeholder.
    if (wins.empty()) {
        mvprintw(LINES - 1, x, "(empty)");
        x += 7;
    }

    // Clock + connection on the right.
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    ::localtime_r(&t, &tm);
    char clk[8];
    std::snprintf(clk, sizeof(clk), "%02d:%02d", tm.tm_hour, tm.tm_min);
    std::string right = clk;
    if (!connection_info.empty()) right = connection_info + "  " + right;

    int right_x = cols - static_cast<int>(right.size());
    if (right_x > x)
        mvprintw(LINES - 1, right_x, "%s", right.c_str());

    attroff(A_REVERSE);
}

} // namespace meshcli
