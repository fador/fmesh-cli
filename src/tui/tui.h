#pragma once

#include "input_line.h"
#include "mesh/mesh_service.h"
#include "status_bar.h"
#include "theme.h"
#include "util/event_loop.h"
#include "window_manager.h"

#include <atomic>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace meshcli {

struct AppConfig;

enum class Mode { Normal, ConnectWizard_Tab, ConnectWizard_BLE,
                  ConnectWizard_TCP, ConnectWizard_Serial, ConnectWizard_Mesh };

enum class ConnTransport { BLE, TCP, Serial, Mesh };

enum class NodeListSort { Name, LastHeard, NodeId, Battery, Hops, Distance };

struct BleScanEntry {
    std::string name;
    std::string address;
    std::string device_path;
    int16_t rssi = 0;
};

class TuiApp {
public:
    TuiApp(MeshService& service, ConcurrentQueue<MeshEvent>& queue, EventFd& wake,
           const std::string& history_path);
    ~TuiApp();

    int run();
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
    void maybe_reconnect();

    // --- resize support ---
#ifndef _WIN32
    static void on_sigwinch(int);
#endif
    static TuiApp* s_instance_;
    void handle_resize();

    // --- connection wizard ---
    void enter_wizard();
    void exit_wizard();
    void start_scan();
    void stop_scan();
    bool handle_wizard_key(int ch);
    void render_wizard_tab();
    void render_wizard_ble();
    void render_wizard_tcp();
    void render_wizard_serial();
    void render_wizard_mesh();

    // --- nodelist ---
    void render_nodelist(const Window& w, int top, int height, int width);
    bool handle_nodelist_key(int ch);

    // --- popup ---
    void render_popup();
    bool handle_popup_key(int ch);

    // --- themes ---
    [[nodiscard]] const ColorTheme& current_theme() const { return *current_theme_; }
    bool set_theme(const std::string& name);

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

    std::string active_device_;

    // Auto-reconnect state
    std::map<std::string, int> reconnect_attempts_;
    int reconnect_delay_s_ = 5;
    int reconnect_max_attempts_ = 6;
    static constexpr int kReconnectIntervalS = 5;
    static time_t s_last_reconnect_attempt;

    // --- wizard state ---
    Mode mode_ = Mode::Normal;
    ConnTransport wizard_transport_ = ConnTransport::BLE;
    std::vector<BleScanEntry> scan_entries_;
    int scan_selection_ = 0;
    int scan_entries_offset_ = 0;
    std::thread scan_thread_;
    std::atomic<bool> scan_running_{false};
    bool scan_complete_ = false;
    // Wizard form fields
    std::string wizard_pin_ = "123456";
    std::string wizard_tcp_host_ = "";
    std::string wizard_tcp_port_ = "4403";
    std::string wizard_serial_path_ = "/dev/ttyUSB0";
    std::string wizard_serial_baud_ = "115200";
    std::string wizard_mesh_host_ = "";
    std::string wizard_mesh_port_ = "4404";
    std::string wizard_mesh_user_ = "admin";
    std::string wizard_mesh_password_ = "";
    int wizard_field_ = 0;  // 0=host, 1=port (tcp) or 0=pin (ble) or 0=path,1=baud (serial) or 0..3 (mesh)
    int wizard_field_cursor_[4] = {0, 0, 0, 0};

    // --- nodelist state ---
    std::string nodelist_device_;       // which device's nodelist is shown
    int nodelist_cursor_ = 0;
    int nodelist_offset_ = 0;
    NodeListSort nodelist_sort_ = NodeListSort::Name;

    // --- popup state ---
    bool popup_active_ = false;
    std::string popup_device_;
    Node popup_node_;
    int popup_selection_ = 0;  // 0=Send DM, 1=Close

    // --- theming ---
    const ColorTheme* current_theme_ = nullptr;
};

} // namespace meshcli
