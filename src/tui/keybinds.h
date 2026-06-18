#pragma once

#ifdef _WIN32
#include <curses.h>
#else
#include <ncurses.h>
#endif

namespace meshcli {

// Centralized key bindings. Values are ncurses key codes (or ASCII).
// All bindings are overridable here for consistency.
namespace keybind {
    constexpr int WINDOW_1    = '1' | 0x80;   // Alt+1 .. Alt+0 = windows 1-10
    constexpr int WINDOW_11   = 'q' | 0x80;   // Alt+q..p = windows 11-20
    constexpr int NEXT_ACTIVE = 'a' | 0x80;   // Alt+a = next active window
    constexpr int WINDOW_NEXT = 'n' | 0x80;   // Alt+n = next window (also Alt+Right)
    constexpr int WINDOW_PREV = KEY_LEFT;     // Alt+Left = previous window
    constexpr int PAGE_UP     = KEY_PPAGE;
    constexpr int PAGE_DOWN   = KEY_NPAGE;
    constexpr int REDRAW      = 12;           // Ctrl-L
    constexpr int DEVICE_CYCLE = 24;          // Ctrl-X (cycle active device)
    constexpr int QUIT        = 3;            // Ctrl-C (also /quit)
} // namespace keybind

} // namespace meshcli
