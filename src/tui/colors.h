// Shared ncurses color-pair indices. Must match init_pair() order in tui.cpp.
#pragma once

namespace meshcli {
namespace tui_color {
    constexpr int META    = 1;   // cyan     (status/meta messages)
    constexpr int CHANNEL = 2;   // default  (channel broadcasts)
    constexpr int DM      = 3;   // green    (direct messages)
    constexpr int MENTION = 4;   // yellow   (mentions)
    constexpr int ERROR   = 5;   // red      (errors)
    constexpr int INFO    = 6;   // cyan     (info lines)
}  // namespace tui_color
}  // namespace meshcli
