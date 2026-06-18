#pragma once

#include "input_line.h"
#include "mesh/mesh_service.h"
#include "status_bar.h"
#include "util/event_loop.h"
#include "window_manager.h"

#include <atomic>
#include <map>
#include <string>

namespace meshcli {

struct AppConfig;

// The ncurses application: owns the screen, the window manager, the input
// line, and the main poll loop. Runs on the main thread.
class TuiApp {
public:
    TuiApp(MeshService& service, ConcurrentQueue<MeshEvent>& queue, EventFd& wake,
           const std::string& history_path);
    ~TuiApp();

    // Enter the ncurses event loop. Returns when the user quits. Returns an
    // exit code (0 = clean).
    int run();
    // Returns true if ncurses was successfully initialized.
    [[nodiscard]] bool ncurses_ok() const { return ncurses_ok_; }

private:
    void init_ncurses();
    void teardown_ncurses();
    void render();
    void render_scrollback(const Window& w, int top, int height, int width);
    void render_input(int row, int width);
    void process_events();
    void handle_event(const MeshEvent& ev);
    std::string connection_info() const;
    void maybe_reconnect();    // called every ~1s from the poll loop

    MeshService& service_;
    ConcurrentQueue<MeshEvent>& queue_;
    EventFd& wake_;

    WindowManager wm_;
    InputLine input_;
    StatusBar status_bar_;
    bool quit_ = false;
    bool ncurses_ok_ = false;
    bool need_redraw_ = true;
    std::string history_path_;

    // Active device for context-sensitive commands (cycled with Ctrl+X).
    std::string active_device_;

    // Auto-reconnect state (per-device attempt counters)
    std::map<std::string, int> reconnect_attempts_;
    int reconnect_delay_s_ = 5;
    int reconnect_max_attempts_ = 6;
    static constexpr int kReconnectIntervalS = 5;
    static time_t s_last_reconnect_attempt;
};

} // namespace meshcli
