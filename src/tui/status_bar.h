#pragma once

#include "window_manager.h"

#include <string>

namespace meshcli {

// Bottom status bar: window list with activity marks + clock + connection.
class StatusBar {
public:
    StatusBar() = default;

    // Render to the last line of the screen. `cols` is the screen width.
    void render(WindowManager& wm, int cols, const std::string& connection_info);

private:
    static std::string window_label(const Window& w);
    static int color_for(const Window& w);
};

} // namespace meshcli
