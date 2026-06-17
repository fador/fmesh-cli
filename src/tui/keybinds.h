#pragma once

#include <ncurses.h>

namespace meshcli {

// Centralized key bindings. Values are ncurses key codes (or ASCII).
// All bindings are overridable here for consistency.
namespace keybind {
    constexpr int WINDOW_1   = '1' | 0x80;   // Alt+1 .. handled via ESC prefix
    constexpr int NEXT_ACTIVE = 'a' | 0x80;  // Alt+a
    constexpr int WINDOW_PREV = 'p' | 0x80;  // Alt+p
    constexpr int WINDOW_NEXT = 'n' | 0x80;  // Alt+n
    constexpr int PAGE_UP     = KEY_PPAGE;
    constexpr int PAGE_DOWN   = KEY_NPAGE;
    constexpr int REDRAW      = 12;          // Ctrl-L
    constexpr int QUIT        = 3;           // Ctrl-C (also /quit)
} // namespace keybind

} // namespace meshcli
