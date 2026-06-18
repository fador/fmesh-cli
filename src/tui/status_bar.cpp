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
    attron(COLOR_PAIR(tui_color::STATUS));
    mvhline(LINES - 1, 0, ' ', cols);

    int x = 0;
    int cur = wm.current_index();
    Window* cw = wm.current_window();
    std::string cur_label = cw ? window_label(*cw) : "?";
    int cur_color = cw ? color_for(*cw) : 0;

    attron(COLOR_PAIR(cur_color) | A_BOLD);
    mvprintw(LINES - 1, x, "[%d:%s] ", cur, cur_label.c_str());
    x += static_cast<int>(std::snprintf(nullptr, 0, "[%d:%s] ", cur, cur_label.c_str()));
    attrset(COLOR_PAIR(tui_color::STATUS));

    // Compute the right-side text so we know how much space to leave.
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    ::localtime_r(&t, &tm);
    char clk[8];
    std::snprintf(clk, sizeof(clk), "%02d:%02d", tm.tm_hour, tm.tm_min);
    std::string right = clk;
    if (!connection_info.empty()) right = connection_info + "  " + right;
    int right_w = static_cast<int>(right.size());
    int right_x = cols - right_w;

    const auto& wins = wm.windows();
    for (size_t i = 0; i < wins.size(); ++i) {
        int idx = static_cast<int>(i + 1);
        const Window& w = *wins[i];
        char mark = ' ';
        int act = w.activity();
        if (act >= 2)      mark = '*';
        else if (act == 1) mark = '#';
        int unread = w.unread();

        std::string label = window_label(w);
        int label_w;
        if (unread > 0) {
            label_w = static_cast<int>(std::snprintf(nullptr, 0,
                "%d%c%d%s ", idx, mark, unread, label.c_str()));
        } else {
            label_w = static_cast<int>(std::snprintf(nullptr, 0,
                "%d%c%s ", idx, mark, label.c_str()));
        }
        // Stop before the clock area.
        if (x + label_w >= right_x) break;

        attron(COLOR_PAIR(color_for(w)) | A_REVERSE);
        if (unread > 0)
            mvprintw(LINES - 1, x, "%d%c%d%s ", idx, mark, unread, label.c_str());
        else
            mvprintw(LINES - 1, x, "%d%c%s ", idx, mark, label.c_str());
        x += label_w;
        attrset(COLOR_PAIR(tui_color::STATUS));
    }

    if (wins.empty()) {
        mvprintw(LINES - 1, x, "(empty)");
        x += 7;
    }

    if (right_x > x)
        mvprintw(LINES - 1, right_x, "%s", right.c_str());

    attroff(COLOR_PAIR(tui_color::STATUS));
}

} // namespace meshcli
