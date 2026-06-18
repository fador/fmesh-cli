#pragma once

#include <ncurses.h>
#include <string>
#include <vector>

namespace meshcli {

struct ColorTheme {
    std::string name;
    std::string description;
    // Foreground and background (-1 = default terminal bg)
    short meta_fg,    meta_bg;
    short channel_fg, channel_bg;
    short dm_fg,      dm_bg;
    short mention_fg, mention_bg;
    short error_fg,   error_bg;
    short info_fg,    info_bg;
    short title_fg,   title_bg;
    short status_fg,  status_bg;
};

// Pair indices — must be kept in sync with apply_theme().
namespace tui_color {
    constexpr int META    = 1;
    constexpr int CHANNEL = 2;
    constexpr int DM      = 3;
    constexpr int MENTION = 4;
    constexpr int ERROR   = 5;
    constexpr int INFO    = 6;
    constexpr int TITLE   = 7;
    constexpr int STATUS  = 8;
    constexpr int MAX_PAIR = 8;
}

// Apply a theme to ncurses color pairs. Call on init or theme switch.
inline void apply_theme(const ColorTheme& t) {
    init_pair(tui_color::META,    t.meta_fg,    t.meta_bg);
    init_pair(tui_color::CHANNEL, t.channel_fg, t.channel_bg);
    init_pair(tui_color::DM,      t.dm_fg,      t.dm_bg);
    init_pair(tui_color::MENTION, t.mention_fg, t.mention_bg);
    init_pair(tui_color::ERROR,   t.error_fg,   t.error_bg);
    init_pair(tui_color::INFO,    t.info_fg,    t.info_bg);
    init_pair(tui_color::TITLE,   t.title_fg,   t.title_bg);
    init_pair(tui_color::STATUS,  t.status_fg,  t.status_bg);
}

// Built-in themes.
inline const std::vector<ColorTheme>& builtin_themes() {
    static const std::vector<ColorTheme> themes = {
        {
            "dark", "Dark (default)",
            COLOR_CYAN,    -1,
            -1,            -1,
            COLOR_GREEN,   -1,
            COLOR_YELLOW,  -1,
            COLOR_RED,     -1,
            COLOR_CYAN,    -1,
            COLOR_WHITE,   COLOR_BLUE,
            COLOR_WHITE,   COLOR_BLUE,
        },
        {
            "light", "Light terminal",
            COLOR_BLUE,    -1,
            -1,            -1,
            COLOR_GREEN,   -1,
            COLOR_RED,     -1,
            COLOR_RED,     -1,
            COLOR_BLUE,    -1,
            COLOR_BLACK,   COLOR_WHITE,
            COLOR_BLACK,   COLOR_WHITE,
        },
        {
            "solarized", "Solarized palette",
            COLOR_CYAN,    -1,
            COLOR_CYAN,    -1,
            COLOR_GREEN,   -1,
            COLOR_YELLOW,  -1,
            COLOR_RED,     -1,
            COLOR_BLUE,    -1,
            COLOR_WHITE,   COLOR_BLUE,
            COLOR_WHITE,   COLOR_BLUE,
        },
        {
            "monokai", "Monokai palette",
            COLOR_YELLOW,  -1,
            COLOR_WHITE,   -1,
            COLOR_GREEN,   -1,
            COLOR_MAGENTA, -1,
            COLOR_RED,     -1,
            COLOR_CYAN,    -1,
            COLOR_WHITE,   COLOR_MAGENTA,
            COLOR_WHITE,   COLOR_MAGENTA,
        },
        {
            "dracula", "Dracula palette",
            COLOR_CYAN,    -1,
            COLOR_WHITE,   -1,
            COLOR_GREEN,   -1,
            COLOR_YELLOW,  -1,
            COLOR_RED,     -1,
            COLOR_MAGENTA, -1,
            COLOR_WHITE,   COLOR_MAGENTA,
            COLOR_WHITE,   COLOR_MAGENTA,
        },
        {
            "ocean", "Ocean blues",
            COLOR_CYAN,    -1,
            COLOR_WHITE,   -1,
            COLOR_GREEN,   -1,
            COLOR_YELLOW,  -1,
            COLOR_RED,     -1,
            COLOR_BLUE,    -1,
            COLOR_WHITE,   COLOR_BLUE,
            COLOR_WHITE,   COLOR_BLUE,
        },
    };
    return themes;
}

// Find a theme by name (case-insensitive prefix match). Returns nullptr if not found.
inline const ColorTheme* find_theme(const std::string& name) {
    std::string lower = name;
    for (auto& c : lower) c = static_cast<char>(std::tolower(c));
    for (const auto& t : builtin_themes()) {
        std::string tn = t.name;
        for (auto& c : tn) c = static_cast<char>(std::tolower(c));
        if (tn == lower || tn.find(lower) == 0) return &t;
    }
    return nullptr;
}

} // namespace meshcli
