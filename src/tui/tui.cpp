#include "tui.h"

#include "app/app.h"
#include "ble/ble_client.h"
#include "colors.h"
#include "command.h"
#include "keybinds.h"
#include "mesh/event.h"
#include "mesh/mesh_service.h"
#include "stream/stream_client.h"
#include "util/log.h"

#ifdef _WIN32
#include <curses.h>
#else
#include <ncurses.h>
#endif

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <cctype>
#include <locale.h>
#ifndef _WIN32
#include <poll.h>
#endif
#include <set>
#include <sstream>
#include <string>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <variant>

namespace meshcli {

namespace {
double haversine(double lat1, double lon1, double lat2, double lon2) {
    constexpr double R = 6371.0;
    double dLat = (lat2 - lat1) * 3.14159265358979323846 / 180.0;
    double dLon = (lon2 - lon1) * 3.14159265358979323846 / 180.0;
    lat1 = lat1 * 3.14159265358979323846 / 180.0;
    lat2 = lat2 * 3.14159265358979323846 / 180.0;
    double a = std::sin(dLat/2) * std::sin(dLat/2) +
               std::cos(lat1) * std::cos(lat2) * std::sin(dLon/2) * std::sin(dLon/2);
    double c = 2 * std::atan2(std::sqrt(a), std::sqrt(1-a));
    return R * c;
}

std::pair<std::optional<double>, std::optional<double>> get_my_location(
    MeshService& service, bool unified, const std::string& nodelist_device) {
    std::optional<double> my_lat, my_lon;
    if (!unified) {
        if (const NodeDb* db = service.db_for(nodelist_device)) {
            if (auto me = db->get(db->my_node_num())) {
                my_lat = me->latitude;
                my_lon = me->longitude;
            }
        }
    } else {
        for (const auto& id : service.device_ids()) {
            if (const NodeDb* db = service.db_for(id)) {
                if (auto me = db->get(db->my_node_num())) {
                    if (me->latitude && me->longitude) {
                        my_lat = me->latitude;
                        my_lon = me->longitude;
                        break;
                    }
                }
            }
        }
    }
    return {my_lat, my_lon};
}

double get_dist(const Node& n, std::optional<double> my_lat, std::optional<double> my_lon) {
    if (my_lat && my_lon && n.latitude && n.longitude)
        return haversine(*my_lat, *my_lon, *n.latitude, *n.longitude);
    return 999999.0;
}
} // namespace

time_t TuiApp::s_last_reconnect_attempt = 0;
TuiApp* TuiApp::s_instance_ = nullptr;

TuiApp::TuiApp(MeshService& service, ConcurrentQueue<MeshEvent>& queue, EventFd& wake,
               AppConfig& config)
    : service_(service), queue_(queue), wake_(wake), wm_(service),
      config_(config) { s_instance_ = this; }

TuiApp::~TuiApp() {
    stop_scan();
    s_instance_ = nullptr;
    if (!config_.history_path.empty())
        input_.save_history(config_.history_path);
    teardown_ncurses();
}

#ifndef _WIN32
void TuiApp::on_sigwinch(int) {

    if (s_instance_) {
        endwin();
        refresh();
        clearok(stdscr, TRUE);
        s_instance_->need_redraw_ = true;
    }
}
#endif

void TuiApp::handle_resize() {
    need_redraw_ = true;
}

void TuiApp::init_ncurses() {
    setlocale(LC_ALL, "");
    if (!::initscr()) {
        ncurses_ok_ = false;
        return;
    }
    ncurses_ok_ = true;
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    set_tabsize(4);
    if (has_colors()) {
        start_color();
        use_default_colors();
        current_theme_ = &builtin_themes()[0];  // dark
        apply_theme(*current_theme_);
    }
    Logger::instance().set_console(false);
    Logger::instance().set_level(LogLevel::Info);
    if (!config_.history_path.empty())
        input_.load_history(config_.history_path);
    #ifndef _WIN32
    std::signal(SIGWINCH, on_sigwinch);
#endif
    clear();
    refresh();
}

void TuiApp::teardown_ncurses() {
    if (ncurses_ok_) {
        ::endwin();
        ncurses_ok_ = false;
    }
    Logger::instance().set_console(true);
}

std::string TuiApp::connection_info() const {
    auto devices = service_.device_ids();
    if (devices.empty()) return "no device";
    std::string s;
    if (devices.size() > 1) {
        std::string active_name = service_.display_name_for(active_device_);
        if (!active_name.empty())
            s += "[" + active_name + "] ";
    }
    for (size_t i = 0; i < devices.size(); ++i) {
        if (i) s += ",";
        const NodeDb* db = service_.db_for(devices[i]);
        std::string fw = service_.firmware_for(devices[i]);
        if (db) {
            auto me = db->get(db->my_node_num());
            if (me) {
                s += me->long_name.empty() ? "node" : me->long_name;
                if (me->battery_level)
                    s += " " + std::to_string(*me->battery_level) + "%";
            } else {
                s += "conn";
            }
        } else {
            s += "conn";
        }
        if (!fw.empty()) s += " | fw=" + fw;
    }
    return s;
}

void TuiApp::maybe_reconnect() {
    if (reconnect_attempts_.empty()) return;
    time_t now = std::time(nullptr);
    if (now - s_last_reconnect_attempt < kReconnectIntervalS) return;
    s_last_reconnect_attempt = now;

    auto ids = service_.device_ids();
    std::vector<std::string> to_remove;
    for (auto& [device_id, attempts] : reconnect_attempts_) {
        bool already = false;
        for (const auto& id : ids)
            if (id == device_id) { already = true; break; }
        if (already) {
            wm_.append_status("*** " + device_id + " reconnected", tui_color::INFO);
            to_remove.push_back(device_id);
            continue;
        }
        ++attempts;
        if (attempts > reconnect_max_attempts_) {
            wm_.append_status("*** Auto-reconnect gave up for " + device_id
                              + " after " + std::to_string(reconnect_max_attempts_) + " attempts",
                              tui_color::ERROR);
            to_remove.push_back(device_id);
            continue;
        }
        wm_.append_status("*** Reconnect attempt " +
                          std::to_string(attempts) + "/" +
                          std::to_string(reconnect_max_attempts_) +
                          " for " + device_id + "...",
                          tui_color::INFO);
        if (service_.reconnect_device(device_id)) {
            wm_.append_status("*** " + device_id + " reconnected!", tui_color::INFO);
            to_remove.push_back(device_id);
        }
    }
    for (const auto& id : to_remove)
        reconnect_attempts_.erase(id);
    if (!to_remove.empty())
        need_redraw_ = true;
}

// --- connection wizard -------------------------------------------------------

void TuiApp::enter_wizard() {
    mode_ = Mode::ConnectWizard_Tab;
    wizard_transport_ = ConnTransport::BLE;
    wizard_field_ = 0;
    wizard_field_cursor_[0] = 0;
    wizard_field_cursor_[1] = 0;
    scan_entries_.clear();
    scan_selection_ = 0;
    scan_entries_offset_ = 0;
    scan_complete_ = false;
    wizard_pin_ = "123456";
}

void TuiApp::exit_wizard() {
    stop_scan();
    mode_ = Mode::Normal;
    erase();
    clearok(stdscr, TRUE);
    need_redraw_ = true;
}

void TuiApp::start_scan() {
    stop_scan();
    scan_entries_.clear();
    scan_selection_ = 0;
    scan_complete_ = false;
    scan_running_ = true;
    BleClient::scan_async(15, [this](const std::string& name, const std::string& addr) {
        // Dedup is done on the TUI thread in the event handler.
        EvBleDeviceFound ev;
        ev.device = addr; // using addr as device_path abstraction
        ev.name = name;
        ev.address = addr;
        queue_.push(std::move(ev));
        wake_.notify();
    }, [this]() {
        scan_running_ = false;
        EvBleDeviceFound done;
        done.scan_complete = true;
        queue_.push(std::move(done));
        wake_.notify();
    });
}

void TuiApp::stop_scan() {
    scan_running_ = false;
    BleClient::stop_scan();
}

bool TuiApp::handle_wizard_key(int ch) {
    using M = Mode;
    bool handled = true;

    if (ch == 27) { // ESC — go back or exit
        if (mode_ == M::ConnectWizard_Tab) { exit_wizard(); }
        else { mode_ = M::ConnectWizard_Tab; need_redraw_ = true; }
        return true;
    }

    switch (mode_) {
    case M::ConnectWizard_Tab:
        if (ch == KEY_UP || ch == 'k') {
            int t = static_cast<int>(wizard_transport_);
            t = (t - 1 + 4) % 4;
            wizard_transport_ = static_cast<ConnTransport>(t);
            need_redraw_ = true;
        } else if (ch == KEY_DOWN || ch == 'j' || ch == '\t') {
            int t = static_cast<int>(wizard_transport_);
            t = (t + 1) % 4;
            wizard_transport_ = static_cast<ConnTransport>(t);
            need_redraw_ = true;
        } else if (ch == '\n' || ch == KEY_ENTER) {
            if (wizard_transport_ == ConnTransport::BLE) {
                start_scan();
                mode_ = M::ConnectWizard_BLE;
                wizard_field_ = 0;
                wizard_field_cursor_[0] = 0;
                need_redraw_ = true;
            } else if (wizard_transport_ == ConnTransport::TCP) {
                mode_ = M::ConnectWizard_TCP;
                wizard_field_ = 0;
                wizard_field_cursor_[0] = static_cast<int>(wizard_tcp_host_.size());
                wizard_field_cursor_[1] = static_cast<int>(wizard_tcp_port_.size());
                need_redraw_ = true;
            } else if (wizard_transport_ == ConnTransport::Mesh) {
                mode_ = M::ConnectWizard_Mesh;
                wizard_field_ = 0;
                wizard_field_cursor_[0] = static_cast<int>(wizard_mesh_host_.size());
                wizard_field_cursor_[1] = static_cast<int>(wizard_mesh_port_.size());
                wizard_field_cursor_[2] = static_cast<int>(wizard_mesh_user_.size());
                wizard_field_cursor_[3] = static_cast<int>(wizard_mesh_password_.size());
                need_redraw_ = true;
            } else {
                mode_ = M::ConnectWizard_Serial;
                wizard_field_ = 0;
                wizard_field_cursor_[0] = static_cast<int>(wizard_serial_path_.size());
                wizard_field_cursor_[1] = static_cast<int>(wizard_serial_baud_.size());
                need_redraw_ = true;
            }
        } else { handled = false; }
        break;

    case M::ConnectWizard_BLE:
        if (ch == '\t') {
            wizard_field_ = (wizard_field_ == 0) ? 1 : 0;
            need_redraw_ = true;
        } else if (ch == '\n' || ch == KEY_ENTER) {
            if (scan_selection_ >= 0 && static_cast<size_t>(scan_selection_) < scan_entries_.size()) {
                const auto& e = scan_entries_[scan_selection_];
                BleDeviceSpec spec;
                spec.name = e.name;
                spec.address = e.address;
                spec.pin = wizard_pin_;
                service_.connect_device(spec, false);
            }
            exit_wizard();
        } else if (ch == KEY_UP || ch == 'k') {
            if (scan_selection_ > 0) {
                --scan_selection_;
                if (scan_selection_ < scan_entries_offset_)
                    scan_entries_offset_ = scan_selection_;
                need_redraw_ = true;
            }
        } else if (ch == KEY_DOWN || ch == 'j') {
            if (static_cast<size_t>(scan_selection_ + 1) < scan_entries_.size()) {
                ++scan_selection_;
                int max_lines = std::max(1, LINES - 7);
                if (scan_selection_ >= scan_entries_offset_ + max_lines)
                    scan_entries_offset_ = scan_selection_ - max_lines + 1;
                need_redraw_ = true;
            }
        } else if (wizard_field_ == 1) {
            // Edit PIN field
            if (ch == KEY_BACKSPACE || ch == 127 || ch == 8 || ch == '\b') {
                if (!wizard_pin_.empty()) wizard_pin_.pop_back();
                need_redraw_ = true;
            } else if (ch >= 32 && ch < 127 && wizard_pin_.size() < 6) {
                wizard_pin_ += static_cast<char>(ch);
                need_redraw_ = true;
            } else { handled = false; }
        } else { handled = false; }
        break;

    case M::ConnectWizard_TCP: {
        auto* field = (wizard_field_ == 0) ? &wizard_tcp_host_ : &wizard_tcp_port_;
        if (ch == '\t') {
            wizard_field_ = (wizard_field_ + 1) % 2;
            need_redraw_ = true;
        } else if (ch == '\n' || ch == KEY_ENTER) {
            if (!wizard_tcp_host_.empty()) {
                BleDeviceSpec spec;
                spec.tcp_host = wizard_tcp_host_ + ":" + wizard_tcp_port_;
                service_.connect_device(spec, false);
            }
            exit_wizard();
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8 || ch == '\b') {
            if (!field->empty()) field->pop_back();
            need_redraw_ = true;
        } else if (ch >= 32 && ch < 127 && field->size() < 60) {
            *field += static_cast<char>(ch);
            need_redraw_ = true;
        } else { handled = false; }
        break;
    }

    case M::ConnectWizard_Serial: {
        auto* field = (wizard_field_ == 0) ? &wizard_serial_path_ : &wizard_serial_baud_;
        if (ch == '\t') {
            wizard_field_ = (wizard_field_ + 1) % 2;
            need_redraw_ = true;
        } else if (ch == '\n' || ch == KEY_ENTER) {
            if (!wizard_serial_path_.empty()) {
                BleDeviceSpec spec;
                spec.serial_port = wizard_serial_path_;
                spec.serial_baud = std::atoi(wizard_serial_baud_.c_str());
                if (spec.serial_baud <= 0) spec.serial_baud = 115200;
                service_.connect_device(spec, false);
            }
            exit_wizard();
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8 || ch == '\b') {
            if (!field->empty()) field->pop_back();
            need_redraw_ = true;
        } else if (ch >= 32 && ch < 127 && field->size() < 60) {
            *field += static_cast<char>(ch);
            need_redraw_ = true;
        } else { handled = false; }
        break;
    }

    case M::ConnectWizard_Mesh: {
        std::string* fields[] = {&wizard_mesh_host_, &wizard_mesh_port_, &wizard_mesh_user_, &wizard_mesh_password_};
        auto* field = fields[wizard_field_];
        if (ch == '\t') {
            wizard_field_ = (wizard_field_ + 1) % 4;
            need_redraw_ = true;
        } else if (ch == '\n' || ch == KEY_ENTER) {
            if (!wizard_mesh_host_.empty()) {
                BleDeviceSpec spec;
                spec.mesh_host = wizard_mesh_host_ + ":" + wizard_mesh_port_;
                spec.mesh_user = wizard_mesh_user_;
                spec.mesh_password = wizard_mesh_password_;
                service_.connect_device(spec, false);
            }
            exit_wizard();
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8 || ch == '\b') {
            if (!field->empty()) field->pop_back();
            need_redraw_ = true;
        } else if (ch >= 32 && ch < 127 && field->size() < 60) {
            *field += static_cast<char>(ch);
            need_redraw_ = true;
        } else { handled = false; }
        break;
    }

    default:
        handled = false;
        break;
    }
    return handled;
}

bool TuiApp::handle_server_config_key(int ch) {
    if (ch == 27) { // ESC
        mode_ = Mode::Normal;
        erase();
        clearok(stdscr, TRUE);
        need_redraw_ = true;
        return true;
    }

    std::string* fields[] = {&wizard_server_port_, &wizard_server_user_, &wizard_server_password_};
    auto* field = fields[wizard_field_];

    if (ch == '\t') {
        wizard_field_ = (wizard_field_ + 1) % 3;
        need_redraw_ = true;
    } else if (ch == '\n' || ch == KEY_ENTER) {
        config_.server_mode = true;
        config_.server_port = std::atoi(wizard_server_port_.c_str());
        if (config_.server_port <= 0) config_.server_port = 4404;
        config_.server_user = wizard_server_user_;
        config_.server_password = wizard_server_password_;
        save_config(config_);

        service_.start_stream_server(config_.server_port, config_.server_user, config_.server_password);
        wm_.append_status("Started Mesh Sync Stream Server on port " + std::to_string(config_.server_port), tui_color::INFO);

        mode_ = Mode::Normal;
        erase();
        clearok(stdscr, TRUE);
        need_redraw_ = true;
    } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8 || ch == '\b') {
        if (!field->empty()) field->pop_back();
        need_redraw_ = true;
    } else if (ch >= 32 && ch < 127 && field->size() < 60) {
        *field += static_cast<char>(ch);
        need_redraw_ = true;
    }
    return true;
}

// --- themes ---

bool TuiApp::set_theme(const std::string& name) {
    const ColorTheme* t = find_theme(name);
    if (!t) return false;
    current_theme_ = t;
    apply_theme(*current_theme_);
    need_redraw_ = true;
    return true;
}

// --- nodelist interactive mode -----------------------------------------------

bool TuiApp::handle_nodelist_key(int ch) {
    if (auto* w = wm_.current_window())
        nodelist_device_ = w->target().device;

    bool unified = (nodelist_device_ == "*");

    // Count total nodes across all devices (or single device).
    auto count_nodes = [&]() -> int {
        if (unified) {
            std::set<uint32_t> seen;
            for (const auto& id : service_.device_ids()) {
                const NodeDb* db = service_.db_for(id);
                if (!db) continue;
                for (const auto& n : db->all()) seen.insert(n.node_num);
            }
            return static_cast<int>(seen.size());
        } else {
            const NodeDb* db = service_.db_for(nodelist_device_);
            return db ? static_cast<int>(db->all().size()) : 0;
        }
    };

    if (ch == KEY_UP || ch == 'k') {
        if (nodelist_cursor_ > 0) {
            --nodelist_cursor_;
            if (nodelist_cursor_ < nodelist_offset_)
                nodelist_offset_ = nodelist_cursor_;
            need_redraw_ = true;
        }
        return true;
    }
    if (ch == KEY_DOWN || ch == 'j') {
        int max = count_nodes() - 1;
        if (nodelist_cursor_ < max) {
            ++nodelist_cursor_;
            need_redraw_ = true;
        }
        return true;
    }
    if (ch == '\n' || ch == KEY_ENTER) {
        auto get_nth_node = [&](int idx) -> std::pair<std::string, std::optional<Node>> {
            std::map<uint32_t, std::pair<std::string, Node>> merged;
            auto devices = service_.device_ids();
            if (!unified && !nodelist_device_.empty()) devices = {nodelist_device_};
            for (const auto& id : devices) {
                const NodeDb* db = service_.db_for(id);
                if (!db) continue;
                for (const auto& n : db->all()) {
                    if (!merged.count(n.node_num))
                        merged[n.node_num] = {id, n};
                }
            }
            std::vector<std::pair<std::string, Node>> sorted;
            for (auto& [num, p] : merged) sorted.push_back(std::move(p));
            auto [my_lat, my_lon] = get_my_location(service_, unified, nodelist_device_);
            std::sort(sorted.begin(), sorted.end(), [this, my_lat, my_lon](const auto& a, const auto& b) {
                switch (nodelist_sort_) {
                case NodeListSort::LastHeard:
                    return a.second.last_heard.value_or(0) > b.second.last_heard.value_or(0);
                case NodeListSort::NodeId:
                    return a.second.node_id < b.second.node_id;
                case NodeListSort::Battery:
                    return a.second.battery_level.value_or(0) > b.second.battery_level.value_or(0);
                case NodeListSort::Hops:
                    return a.second.hops_away.value_or(99) < b.second.hops_away.value_or(99);
                case NodeListSort::Distance:
                    return get_dist(a.second, my_lat, my_lon) < get_dist(b.second, my_lat, my_lon);
                default:
                    return a.second.long_name < b.second.long_name;
                }
            });
            if (idx >= 0 && static_cast<size_t>(idx) < sorted.size())
                return {sorted[idx].first, sorted[idx].second};
            return {"", std::nullopt};
        };

        auto [dev, n] = get_nth_node(nodelist_cursor_);
        if (n) {
            popup_device_ = dev;
            popup_node_ = *n;
            popup_selection_ = 0;
            popup_active_ = true;
        }
        need_redraw_ = true;
        return true;
    }
    if (ch == 's') {
        int s = static_cast<int>(nodelist_sort_);
        s = (s + 1) % 6;
        nodelist_sort_ = static_cast<NodeListSort>(s);
        nodelist_cursor_ = 0;
        nodelist_offset_ = 0;
        const char* names[] = {"Name", "Last heard", "Node ID", "Battery", "Hops", "Distance"};
        wm_.append_status("Sort by: " + std::string(names[s]), tui_color::INFO);
        need_redraw_ = true;
        return true;
    }
    return false;
}

void TuiApp::render_nodelist(const Window& w, int top, int height, int width) {
    (void)w;
    bool unified = (nodelist_device_ == "*");
    std::vector<Node> nodes;

    if (unified) {
        // Merge nodes from ALL devices, tracking which device heard each.
        std::map<uint32_t, Node> merged;
        std::map<uint32_t, std::vector<std::string>> heard_by; // node_num -> device names
        for (const auto& id : service_.device_ids()) {
            const NodeDb* db = service_.db_for(id);
            if (!db) continue;
            for (const auto& n : db->all()) {
                auto& mn = merged[n.node_num];
                mn.node_num = n.node_num;
                if (mn.node_id.empty()) mn.node_id = n.node_id;
                if (mn.long_name.empty() || mn.long_name == "?")
                    mn.long_name = n.long_name;
                if (mn.short_name.empty()) mn.short_name = n.short_name;
                if (!n.battery_level && !mn.battery_level)
                    mn.battery_level = n.battery_level;
                else if (n.battery_level && (!mn.battery_level || *n.battery_level > *mn.battery_level))
                    mn.battery_level = n.battery_level;
                if (!n.hops_away || (mn.hops_away && *n.hops_away < *mn.hops_away))
                    mn.hops_away = n.hops_away;
                if (!n.last_heard || (mn.last_heard && *n.last_heard > *mn.last_heard))
                    mn.last_heard = n.last_heard;
                heard_by[n.node_num].push_back(service_.display_name_for(id));
            }
        }
        nodes.reserve(merged.size());
        for (auto& [num, node] : merged) nodes.push_back(std::move(node));
        // Store heard_by for display
        (void)heard_by; // used indirectly via service_
    } else {
        const NodeDb* db = service_.db_for(nodelist_device_);
        if (!db) { mvprintw(top, 0, "(no device selected)"); return; }
        nodes = db->all();
    }

    if (nodes.empty()) {
        mvprintw(top, 0, "(no nodes known)");
        return;
    }
    // Sort
    auto [my_lat, my_lon] = get_my_location(service_, unified, nodelist_device_);
    std::sort(nodes.begin(), nodes.end(), [this, my_lat, my_lon](const Node& a, const Node& b) {
        switch (nodelist_sort_) {
        case NodeListSort::LastHeard:
            return a.last_heard.value_or(0) > b.last_heard.value_or(0);
        case NodeListSort::NodeId:
            return a.node_id < b.node_id;
        case NodeListSort::Battery:
            return a.battery_level.value_or(0) > b.battery_level.value_or(0);
        case NodeListSort::Hops:
            return a.hops_away.value_or(99) < b.hops_away.value_or(99);
        case NodeListSort::Distance:
            return get_dist(a, my_lat, my_lon) < get_dist(b, my_lat, my_lon);
        default:
            return a.long_name < b.long_name;
        }
    });

    int total = static_cast<int>(nodes.size());
    if (nodelist_cursor_ >= total) nodelist_cursor_ = std::max(0, total - 1);
    if (nodelist_offset_ > nodelist_cursor_) nodelist_offset_ = nodelist_cursor_;
    if (nodelist_offset_ < nodelist_cursor_ - height + 2)
        nodelist_offset_ = std::max(0, nodelist_cursor_ - height + 2);

    // Header
    attron(A_BOLD);
    const char* sort_label = "name";
    switch (nodelist_sort_) {
    case NodeListSort::LastHeard: sort_label = "last heard"; break;
    case NodeListSort::NodeId:    sort_label = "node id"; break;
    case NodeListSort::Battery:   sort_label = "battery"; break;
    case NodeListSort::Hops:      sort_label = "hops"; break;
    case NodeListSort::Distance:  sort_label = "distance"; break;
    default: break;
    }
    std::string title = unified
        ? " All Nodes (" + std::to_string(total) + ")  sort: " + sort_label
        + "  arrows=select  enter=info  s=cycle sort"
        : " Nodes (" + std::to_string(total) + ")  sort: " + sort_label
        + "  arrows=select  enter=info  s=cycle sort";
    mvhline(top, 0, ' ', width);
    mvprintw(top, 0, "%s", title.c_str());
    attroff(A_BOLD);

    int row = top + 1;
    for (int i = nodelist_offset_; i < total && row < top + height; ++i, ++row) {
        const Node& n = nodes[i];
        bool sel = (i == nodelist_cursor_);
        mvhline(row, 0, ' ', width);
        if (sel) attron(A_REVERSE);

        std::string sid = n.node_id;
        if (sid.size() > 10) sid = sid.substr(0, 10);
        int batt = n.battery_level.value_or(-1);
        int hops = n.hops_away.value_or(0);
        double dist = get_dist(n, my_lat, my_lon);
        char dist_str[16];
        if (dist < 999999.0) std::snprintf(dist_str, sizeof(dist_str), "%.1fkm", dist);
        else std::snprintf(dist_str, sizeof(dist_str), "???");

        char buf[256];
        if (unified) {
            // Build "heard by" string
            std::string devices_str;
            for (const auto& id : service_.device_ids()) {
                const NodeDb* db = service_.db_for(id);
                if (!db) continue;
                if (db->get(n.node_num)) {
                    if (!devices_str.empty()) devices_str += ",";
                    std::string dname = service_.display_name_for(id);
                    if (dname.size() > 12) dname = dname.substr(0, 10) + "..";
                    devices_str += dname;
                }
            }
            std::snprintf(buf, sizeof(buf), "  %-18s %-6s %-10s  batt=%d%%  hops=%d  dist=%-7s  [%s]",
                          n.long_name.c_str(), n.short_name.c_str(),
                          sid.c_str(), batt, hops, dist_str, devices_str.c_str());
        } else {
            std::snprintf(buf, sizeof(buf), "  %-20s %-8s %-12s  batt=%d%%  hops=%d  dist=%-7s",
                          n.long_name.c_str(), n.short_name.c_str(),
                          sid.c_str(), batt, hops, dist_str);
        }
        if (sel) attron(COLOR_PAIR(tui_color::CHANNEL));
        mvprintw(row, 0, "%s", buf);
        if (sel) attroff(COLOR_PAIR(tui_color::CHANNEL));
        if (sel) attroff(A_REVERSE);
    }
}

// --- popup -------------------------------------------------------------------

void TuiApp::render_popup() {
    const Node& n = popup_node_;
    int popup_w = 44;
    int popup_h = 12;
    int start_y = std::max(0, (LINES - popup_h) / 2);
    int start_x = std::max(0, (COLS - popup_w) / 2);

    // Dim the background by drawing the box first, then overlay
    attron(COLOR_PAIR(tui_color::TITLE));
    for (int y = 0; y < popup_h; ++y) {
        mvhline(start_y + y, start_x, ' ', popup_w);
    }
    attroff(COLOR_PAIR(tui_color::TITLE));

    // Border
    attron(A_BOLD | COLOR_PAIR(tui_color::TITLE));
    mvhline(start_y, start_x, ' ', popup_w);
    std::string title = " Node Info ";
    int title_start = start_x + (popup_w - static_cast<int>(title.size())) / 2;
    mvprintw(start_y, title_start, "%s", title.c_str());
    attroff(A_BOLD);
    attroff(COLOR_PAIR(tui_color::TITLE));

    int row = start_y + 1;

    auto draw_line = [&](const std::string& label, const std::string& value) {
        mvprintw(row, start_x + 2, "%s", label.c_str());
        int val_start = start_x + popup_w - 2 - static_cast<int>(value.size());
        mvprintw(row, std::max(start_x + 2 + static_cast<int>(label.size()) + 1, val_start), "%s", value.c_str());
        ++row;
    };

    std::string display = n.long_name;
    if (!n.short_name.empty() && n.short_name != n.long_name)
        display += " (" + n.short_name + ")";
    draw_line("Name:", display);

    std::string sid = n.node_id;
    if (sid.size() > 16) sid = sid.substr(0, 13) + "...";
    draw_line("ID:", sid + "  #" + std::to_string(n.node_num));

    if (n.battery_level) {
        draw_line("Battery:", std::to_string(*n.battery_level) + "%");
    }
    if (n.last_heard) {
        std::time_t t = static_cast<std::time_t>(*n.last_heard);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", std::localtime(&t));
        draw_line("Last heard:", std::string(buf));
    }
    if (n.hops_away)
        draw_line("Hops:", std::to_string(*n.hops_away));
    if (n.snr) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.1f dB", *n.snr);
        draw_line("SNR:", std::string(buf));
    }

    // Heard by
    std::string heard;
    for (const auto& id : service_.device_ids()) {
        const NodeDb* db = service_.db_for(id);
        if (db && db->get(n.node_num)) {
            if (!heard.empty()) heard += ", ";
            heard += service_.display_name_for(id);
        }
    }
    if (!heard.empty())
        draw_line("Heard by:", heard);

    // Separator
    ++row;
    for (int x = 0; x < popup_w; ++x)
        mvaddch(start_y + 7, start_x + x, ' ');

    // Options
    std::string opt_dm = " Send DM ";
    std::string opt_close = " Close ";
    int total_opt_w = static_cast<int>(opt_dm.size() + opt_close.size() + 3);
    int opt_start = start_x + (popup_w - total_opt_w) / 2;

    if (popup_selection_ == 0) {
        attron(A_REVERSE);
        mvprintw(start_y + popup_h - 3, opt_start, "%s", opt_dm.c_str());
        attroff(A_REVERSE);
        mvprintw(start_y + popup_h - 3, opt_start + static_cast<int>(opt_dm.size()), " | ");
        mvprintw(start_y + popup_h - 3, opt_start + static_cast<int>(opt_dm.size()) + 3, "%s", opt_close.c_str());
    } else {
        mvprintw(start_y + popup_h - 3, opt_start, "%s", opt_dm.c_str());
        mvprintw(start_y + popup_h - 3, opt_start + static_cast<int>(opt_dm.size()), " | ");
        attron(A_REVERSE);
        mvprintw(start_y + popup_h - 3, opt_start + static_cast<int>(opt_dm.size()) + 3, "%s", opt_close.c_str());
        attroff(A_REVERSE);
    }

    mvprintw(start_y + popup_h - 2, start_x + 2, "arrows/tab=select  enter=confirm  esc=close");
}

bool TuiApp::handle_popup_key(int ch) {
    if (ch == 27) { // ESC
        popup_active_ = false;
        need_redraw_ = true;
        return true;
    }
    if (ch == '\t') {
        popup_selection_ = (popup_selection_ + 1) % 2;
        need_redraw_ = true;
        return true;
    }
    if (ch == KEY_LEFT || ch == KEY_RIGHT || ch == 'h' || ch == 'l') {
        popup_selection_ = (popup_selection_ + 1) % 2;
        need_redraw_ = true;
        return true;
    }
    if (ch == '\n' || ch == KEY_ENTER) {
        if (popup_selection_ == 0) {
            // Send DM
            std::string nick = popup_node_.short_name.empty()
                ? popup_node_.long_name : popup_node_.short_name;
            int win_idx = wm_.ensure_dm(popup_device_, popup_node_.node_num, nick);
            wm_.select(win_idx);
            wm_.append_status("Opened DM with " + popup_node_.long_name +
                " (" + popup_node_.node_id + ")", tui_color::INFO);
        }
        popup_active_ = false;
        need_redraw_ = true;
        return true;
    }
    return false;
}

// --- rendering ---------------------------------------------------------------

void TuiApp::render() {
    int rows = LINES;
    int cols = COLS;

    if (mode_ != Mode::Normal) {
        // Wizard takes over the full screen.
        erase();
        switch (mode_) {
        case Mode::ConnectWizard_Tab:    render_wizard_tab(); break;
        case Mode::ConnectWizard_BLE:    render_wizard_ble(); break;
        case Mode::ConnectWizard_TCP:    render_wizard_tcp(); break;
        case Mode::ConnectWizard_Serial: render_wizard_serial(); break;
        case Mode::ConnectWizard_Mesh:   render_wizard_mesh(); break;
        case Mode::ServerConfig:         render_server_config(); break;
        default: break;
        }
        refresh();
        return;
    }

    // Minimum terminal size guard.
    if (rows < 4 || cols < 20) {
        erase();
        const char* msg = "terminal too small";
        mvprintw(rows / 2, std::max(0, (cols - 18) / 2), "%s", msg);
        refresh();
        return;
    }

    erase();

    Window* w = wm_.current_window();
    if (!w) {
        mvprintw(0, 0, "(no window)");
        refresh();
        return;
    }

    int scrollback_top = 1;
    int scrollback_h = std::max(1, rows - 3);

    // Title bar with theme color.
    attron(COLOR_PAIR(tui_color::TITLE));
    mvhline(0, 0, ' ', cols);
    std::string title = "[" + std::to_string(wm_.current_index()) + ":" + w->title() + "]";
    std::string conn = connection_info();
    int title_w = static_cast<int>(title.size());
    int conn_w = static_cast<int>(conn.size());
    int avail = cols - title_w - 1;
    if (conn_w > avail && avail > 3) {
        conn = conn.substr(0, avail - 3) + "...";
        conn_w = static_cast<int>(conn.size());
    }
    if (conn_w <= avail) {
        mvprintw(0, 0, "%s", title.c_str());
        mvprintw(0, std::max(title_w + 1, cols - conn_w), "%s", conn.c_str());
    } else {
        mvprintw(0, 0, "%s", title.c_str());
    }
    attroff(A_REVERSE);

    bool is_nodelist = (w->target().kind == "nodelist");
    if (is_nodelist) {
        nodelist_device_ = w->target().device;
        render_nodelist(*w, scrollback_top, scrollback_h, cols);
    } else {
        render_scrollback(*w, scrollback_top, scrollback_h, cols);
    }
    render_input(rows - 2, cols);
    status_bar_.render(wm_, cols, conn);

    Window* cw = wm_.current_window();
    std::string cur_prompt;
    const std::string& ib = input_.buf();
    if (!ib.empty() && ib[0] == '/')
        cur_prompt = "cmd> ";
    else if (cw && cw->target().kind == "channel")
        cur_prompt = cw->title() + "> ";
    else if (cw && cw->target().kind == "dm")
        cur_prompt = cw->title() + "> ";
    else
        cur_prompt = "status> ";
    move(rows - 2, static_cast<int>(cur_prompt.size() + input_.cursor_visual()));
    if (popup_active_) render_popup();
    refresh();
}

void TuiApp::render_wizard_tab() {
    int rows = LINES, cols = COLS;
    if (rows < 8 || cols < 30) {
        mvprintw(rows / 2, std::max(0, (cols - 18) / 2), "terminal too small");
        return;
    }
    int mid = rows / 2 - 3;
    attron(A_REVERSE);
    mvhline(mid - 2, std::max(0, (cols - 48) / 2), ' ', std::min(48, cols));
    std::string hdr = " Connect Device ";
    mvprintw(mid - 2, std::max(0, (cols - static_cast<int>(hdr.size())) / 2), "%s", hdr.c_str());
    attroff(A_REVERSE);
    auto draw = [&](int y, ConnTransport t, const char* label, const char* desc) {
        int cx = std::max(0, (cols - 48) / 2);
        if (wizard_transport_ == t) {
            attron(A_REVERSE);
            mvprintw(mid + y, cx, " %c %-8s %-36s", 0xE2, label, desc[0] ? desc : "");
            attroff(A_REVERSE);
        } else {
            mvprintw(mid + y, cx, "   %-8s %-36s", label, desc);
        }
    };
    draw(0, ConnTransport::BLE, "BLE", "Nearby Meshtastic radios");
    draw(1, ConnTransport::TCP, "TCP", "Remote device over network");
    draw(2, ConnTransport::Mesh, "Mesh", "Fador's Mesh CLI network (TLS)");
    draw(3, ConnTransport::Serial, "Serial", "Local serial port");

    mvprintw(mid + 6, std::max(0, (cols - 48) / 2),
             " arrow keys or Tab to switch, ENTER to select, ESC to cancel ");
}

void TuiApp::render_wizard_ble() {
    int rows = LINES, cols = COLS;
    if (rows < 6 || cols < 30) {
        mvprintw(rows / 2, std::max(0, (cols - 18) / 2), "terminal too small");
        return;
    }
    int cx = std::max(0, (cols - 56) / 2);
    // Title
    attron(A_REVERSE);
    mvhline(0, cx, ' ', std::min(56, cols - cx));
    mvprintw(0, cx, " BLE Scan - select device with arrows, ENTER to connect ");
    attroff(A_REVERSE);
    // Device list
    int list_top = 1, list_h = std::max(1, rows - 5);
    if (scan_entries_.empty()) {
        if (scan_complete_)
            mvprintw(2, cx, "  (no devices found)");
        else
            mvprintw(2, cx, "  Scanning...");
    } else {
        size_t end = std::min(scan_entries_.size(), static_cast<size_t>(scan_entries_offset_ + list_h));
        for (size_t i = scan_entries_offset_; i < end; ++i) {
            int y = list_top + static_cast<int>(i - scan_entries_offset_);
            const auto& e = scan_entries_[i];
            if (static_cast<int>(i) == scan_selection_) {
                attron(A_REVERSE);
                mvprintw(y, cx, "> %-24s %s", e.name.c_str(), e.address.c_str());
                if (e.rssi != 0) printw("  %d dBm", e.rssi);
    attroff(A_REVERSE);
            } else {
                mvprintw(y, cx, "  %-24s %s", e.name.c_str(), e.address.c_str());
                if (e.rssi != 0) printw("  %d dBm", e.rssi);
            }
        }
    }
    // Footer
    int footer = rows - 2;
    mvprintw(footer, cx, " PIN: [%s%s]", wizard_pin_.c_str(),
             wizard_field_ == 1 ? "|" : "");
    std::string status;
    if (scan_complete_)
        status = "Scan complete - " + std::to_string(scan_entries_.size()) + " devices";
    else if (scan_running_)
        status = "Scanning...";
    else
        status = "Scan stopped";
    mvprintw(footer, cx + 40, "%s", status.c_str());
    mvprintw(rows - 1, cx, " ENTER=connect  TAB=edit PIN  ESC=back");
}

void TuiApp::render_wizard_tcp() {
    int rows = LINES, cols = COLS;
    if (rows < 8 || cols < 30) {
        mvprintw(rows / 2, std::max(0, (cols - 18) / 2), "terminal too small");
        return;
    }
    int cx = std::max(0, (cols - 50) / 2);
    int mid = rows / 2 - 1;
    attron(A_REVERSE);
    mvhline(mid - 2, cx, ' ', std::min(50, cols - cx));
    mvprintw(mid - 2, cx, " TCP Connection ");
    attroff(A_REVERSE);
    mvprintw(mid,     cx, " Host: [%s%s]:[%s%s]",
             wizard_tcp_host_.c_str(), wizard_field_ == 0 ? "|" : "",
             wizard_tcp_port_.c_str(), wizard_field_ == 1 ? "|" : "");
    mvprintw(mid + 1, cx, " Press ENTER to connect  ");
}

void TuiApp::render_wizard_mesh() {
    int rows = LINES, cols = COLS;
    if (rows < 10 || cols < 30) {
        mvprintw(rows / 2, std::max(0, (cols - 18) / 2), "terminal too small");
        return;
    }
    int cx = std::max(0, (cols - 60) / 2);
    int mid = rows / 2 - 2;
    attron(A_REVERSE);
    mvhline(mid - 2, cx, ' ', std::min(60, cols - cx));
    mvprintw(mid - 2, cx, " Mesh Connection (TLS) ");
    attroff(A_REVERSE);
    mvprintw(mid,     cx, " Host:     [%s%s]:[%s%s]",
             wizard_mesh_host_.c_str(), wizard_field_ == 0 ? "|" : "",
             wizard_mesh_port_.c_str(), wizard_field_ == 1 ? "|" : "");
    mvprintw(mid + 1, cx, " User:     [%s%s]",
             wizard_mesh_user_.c_str(), wizard_field_ == 2 ? "|" : "");
    mvprintw(mid + 2, cx, " Password: [%s%s]",
             wizard_mesh_password_.c_str(), wizard_field_ == 3 ? "|" : "");
    mvprintw(mid + 4, cx, " TAB to cycle, ENTER to connect, ESC to cancel");
}

void TuiApp::render_wizard_serial() {
    int rows = LINES, cols = COLS;
    if (rows < 8 || cols < 30) {
        mvprintw(rows / 2, std::max(0, (cols - 18) / 2), "terminal too small");
        return;
    }
    int cx = std::max(0, (cols - 50) / 2);
    int mid = rows / 2 - 2;
    attron(A_REVERSE);
    mvhline(mid - 2, cx, ' ', std::min(50, cols - cx));
    mvprintw(mid - 2, cx, " Serial Connection ");
    attroff(A_REVERSE);
    mvprintw(mid,     cx, " Device: [%s%s]",
             wizard_serial_path_.c_str(), wizard_field_ == 0 ? "|" : "");
    mvprintw(mid + 1, cx, " Baud:   [%s%s]",
             wizard_serial_baud_.c_str(), wizard_field_ == 1 ? "|" : "");
    mvprintw(mid + 2, cx, " Press ENTER to connect  ");
    mvprintw(mid + 4, cx, " ENTER=connect  TAB=next field  ESC=back");
}

void TuiApp::render_server_config() {
    int rows = LINES, cols = COLS;
    if (rows < 10 || cols < 30) {
        mvprintw(rows / 2, std::max(0, (cols - 18) / 2), "terminal too small");
        return;
    }
    int cx = std::max(0, (cols - 50) / 2);
    int mid = rows / 2 - 2;
    attron(A_REVERSE);
    mvhline(mid - 2, cx, ' ', std::min(50, cols - cx));
    mvprintw(mid - 2, cx, " Mesh Sync Stream Server ");
    attroff(A_REVERSE);
    mvprintw(mid,     cx, " Port:     [%s%s]",
             wizard_server_port_.c_str(), wizard_field_ == 0 ? "|" : "");
    mvprintw(mid + 1, cx, " User:     [%s%s]",
             wizard_server_user_.c_str(), wizard_field_ == 1 ? "|" : "");
    mvprintw(mid + 2, cx, " Password: [%s%s]",
             wizard_server_password_.c_str(), wizard_field_ == 2 ? "|" : "");
    mvprintw(mid + 4, cx, " TAB to cycle, ENTER to start server, ESC to cancel");
}

void TuiApp::render_scrollback(const Window& w, int top, int height, int width) {
    const auto& lines = w.lines();
    if (lines.empty()) {
        mvprintw(top, 0, "(empty)");
        return;
    }
    int total = static_cast<int>(lines.size());
    int end_idx = total - w.scroll_offset();
    int start_idx = std::max(0, end_idx - height);
    int row = top + (height - (end_idx - start_idx));
    for (int i = start_idx; i < end_idx; ++i, ++row) {
        const Line& ln = lines[i];
        if (ln.color_pair) attron(COLOR_PAIR(ln.color_pair));
        if (ln.is_meta) attron(A_DIM);
        std::string s = ln.text;
        if (static_cast<int>(s.size()) > width) s = s.substr(0, width);
        mvhline(row, 0, ' ', width);
        mvprintw(row, 0, "%s", s.c_str());
        if (ln.is_meta) attroff(A_DIM);
        if (ln.color_pair) attroff(COLOR_PAIR(ln.color_pair));
    }
}

void TuiApp::render_input(int row, int width) {
    mvhline(row, 0, ' ', width);
    Window* w = wm_.current_window();
    std::string prompt;
    int prompt_color = 0;
    const std::string& buf = input_.buf();
    if (!buf.empty() && buf[0] == '/') {
        prompt = "cmd> ";
        prompt_color = tui_color::META;
    } else if (w && w->target().kind == "channel") {
        prompt = w->title() + "> ";
        prompt_color = tui_color::CHANNEL;
    } else if (w && w->target().kind == "dm") {
        prompt = w->title() + "> ";
        prompt_color = tui_color::DM;
    } else {
        prompt = "status> ";
        prompt_color = tui_color::META;
    }
    if (prompt_color) attron(COLOR_PAIR(prompt_color) | A_BOLD);
    mvprintw(row, 0, "%s", prompt.c_str());
    if (prompt_color) attroff(COLOR_PAIR(prompt_color) | A_BOLD);
    int prompt_w = static_cast<int>(prompt.size());
    std::string s = buf;
    if (static_cast<int>(s.size()) > width - prompt_w) {
        size_t start = s.size() - (width - prompt_w);
        s = s.substr(start);
    }
    mvprintw(row, prompt_w, "%s", s.c_str());
}

// --- event processing --------------------------------------------------------

int TuiApp::run() {
    init_ncurses();
    if (!ncurses_ok_) {
        LOG_ERROR() << "ncurses initialization failed";
        return 1;
    }

    // Load offline history
    service_.load_offline_history();
    for (const auto& dev : service_.device_ids()) {
        wm_.ensure_nodelist(dev);
        auto windows = service_.database().get_all_windows(dev);
        for (const auto& w : windows) {
            if (w.kind == "channel") {
                wm_.ensure_channel(w.device, w.target, "");
            } else if (w.kind == "dm") {
                wm_.ensure_dm(w.device, w.target, "");
            }
        }
    }

    while (!quit_) {
        if (need_redraw_) {
            render();
            need_redraw_ = false;
        }

#ifndef _WIN32
        struct pollfd pfds[2];
        pfds[0].fd = STDIN_FILENO;
        pfds[0].events = POLLIN;
        pfds[1].fd = wake_.fd();
        pfds[1].events = POLLIN;
        int pr = ::poll(pfds, 2, 1000);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pfds[1].revents & POLLIN) {
            wake_.drain();
            process_events();
            need_redraw_ = true;
        }
        if (pfds[0].revents & POLLIN) {
            int ch = getch();
#else
        if (!queue_.empty()) {
            process_events();
            need_redraw_ = true;
        }
        int ch = getch();
        if (ch == ERR) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } else {
#endif
            while (ch != ERR) {
                // In wizard mode, handle keys differently.
                if (mode_ != Mode::Normal) {
                    if (ch == 3) { quit_ = true; break; }  // Ctrl-C still quits
                    if (mode_ == Mode::ServerConfig) {
                        if (!handle_server_config_key(ch)) { /* unhandled */ }
                    } else {
                        if (!handle_wizard_key(ch)) { /* unhandled */ }
                    }
                    ch = getch();
                    continue;
                }
                // Alt+key handling
#ifdef _WIN32
                if (ch >= ALT_0 && ch <= ALT_9) {
                    int idx = (ch == ALT_0) ? 10 : (ch - ALT_0);
                    wm_.select(idx);
                    need_redraw_ = true;
                    ch = getch();
                    continue;
                }
                if (ch == KEY_ALT_L || ch == KEY_ALT_R) { ch = getch(); continue; } // Ignore bare alt keys
                if (ch == ALT_A) { wm_.select_next_active(); need_redraw_ = true; ch = getch(); continue; }
                if (ch == ALT_N) { wm_.select_relative(1); need_redraw_ = true; ch = getch(); continue; }
                if (ch == ALT_Q) { wm_.select(11); need_redraw_ = true; ch = getch(); continue; }
                if (ch == ALT_W) { wm_.select(12); need_redraw_ = true; ch = getch(); continue; }
                if (ch == ALT_E) { wm_.select(13); need_redraw_ = true; ch = getch(); continue; }
                if (ch == ALT_R) { wm_.select(14); need_redraw_ = true; ch = getch(); continue; }
                if (ch == ALT_T) { wm_.select(15); need_redraw_ = true; ch = getch(); continue; }
                if (ch == ALT_Y) { wm_.select(16); need_redraw_ = true; ch = getch(); continue; }
                if (ch == ALT_U) { wm_.select(17); need_redraw_ = true; ch = getch(); continue; }
                if (ch == ALT_I) { wm_.select(18); need_redraw_ = true; ch = getch(); continue; }
                if (ch == ALT_O) { wm_.select(19); need_redraw_ = true; ch = getch(); continue; }
                if (ch == ALT_P) { wm_.select(20); need_redraw_ = true; ch = getch(); continue; }
#endif
                if (ch == 27) {
                    timeout(50);
                    int ch2 = getch();
                    nodelay(stdscr, TRUE);
                    if (ch2 != ERR) {
                        if (ch2 >= '0' && ch2 <= '9') {
                            int idx = (ch2 == '0') ? 10 : (ch2 - '0');
                            wm_.select(idx);
                            need_redraw_ = true;
                        } else if (ch2 == 'a') {
                            wm_.select_next_active();
                            need_redraw_ = true;
                        } else if (ch2 == 'n') {
                            wm_.select_relative(1);
                            need_redraw_ = true;
                        } else if (ch2 == '[') {
                            int ch3 = getch();
                            if (ch3 == 'D') { wm_.select_relative(-1); need_redraw_ = true; }
                            else if (ch3 == 'C') { wm_.select_relative(1); need_redraw_ = true; }
                        } else if (ch2 == 27) {
                            int ch3 = getch();
                            if (ch3 == '[') {
                                int ch4 = getch();
                                if (ch4 == 'D') { wm_.select_relative(-1); need_redraw_ = true; }
                                else if (ch4 == 'C') { wm_.select_relative(1); need_redraw_ = true; }
                            }
                        } else if (ch2 == 'q' || ch2 == 'w' || ch2 == 'e' || ch2 == 'r' ||
                                   ch2 == 't' || ch2 == 'y' || ch2 == 'u' || ch2 == 'i' ||
                                   ch2 == 'o' || ch2 == 'p') {
                            static const char* qrow = "qwertyuiop";
                            int offset = 0;
                            while (offset < 10 && qrow[offset] != ch2) ++offset;
                            if (offset < 10) {
                                wm_.select(11 + offset);
                                need_redraw_ = true;
                            }
                        }
                        ch = getch();
                        continue;
                    }
                }
                if (ch == KEY_RESIZE) {
                    need_redraw_ = true;
                } else if (ch == 12) {  // Ctrl-L
                    need_redraw_ = true;
                } else if (ch == 24) {   // Ctrl-X
                    auto devices = service_.device_ids();
                    if (devices.size() > 1) {
                        int cur = 0;
                        for (size_t i = 0; i < devices.size(); ++i) {
                            if (devices[i] == active_device_) { cur = static_cast<int>(i); break; }
                        }
                        cur = (cur + 1) % static_cast<int>(devices.size());
                        active_device_ = devices[cur];
                        wm_.append_status("Active device: " +
                            service_.display_name_for(active_device_) + " (" + active_device_ + ")",
                            tui_color::INFO);
                        need_redraw_ = true;
                    }
                } else if (ch == 3) {   // Ctrl-C
                    quit_ = true;
                    break;
                } else if (ch == KEY_PPAGE) {
                    if (wm_.current_window() && wm_.current_window()->target().kind == "nodelist") {
                        int pg = std::max(1, LINES - 4);
                        nodelist_cursor_ = std::max(0, nodelist_cursor_ - pg);
                        nodelist_offset_ = std::max(0, nodelist_offset_ - pg);
                        need_redraw_ = true;
                    } else if (auto* w = wm_.current_window()) {
                        w->scroll_by(1);
                        need_redraw_ = true;
                    }
                } else if (ch == KEY_NPAGE) {
                    if (wm_.current_window() && wm_.current_window()->target().kind == "nodelist") {
                        const NodeDb* db = service_.db_for(nodelist_device_);
                        int total = db ? static_cast<int>(db->all().size()) : 0;
                        if (nodelist_device_ == "*") {
                            std::set<uint32_t> seen;
                            for (const auto& id : service_.device_ids()) {
                                const NodeDb* d = service_.db_for(id);
                                if (d) for (const auto& n : d->all()) seen.insert(n.node_num);
                            }
                            total = static_cast<int>(seen.size());
                        }
                        int pg = std::max(1, LINES - 4);
                        nodelist_cursor_ = std::min(total - 1, nodelist_cursor_ + pg);
                        nodelist_offset_ = std::max(0, nodelist_cursor_ - (LINES - 5));
                        need_redraw_ = true;
                    } else if (auto* w = wm_.current_window()) {
                        w->scroll_by(-1);
                        need_redraw_ = true;
                    }
                } else if (popup_active_ && handle_popup_key(ch)) {
                    // Popup key handled.
                } else if (popup_active_) {
                    // Ignore other keys while popup is active.
                } else if (wm_.current_window() && wm_.current_window()->target().kind == "nodelist"
                           && handle_nodelist_key(ch)) {
                    // Nodelist key handled (arrows, enter, 's').
                } else {
                    std::string submitted;
                    if (input_.handle_key(ch, submitted)) {
                        need_redraw_ = true;
                        CommandDispatcher disp(service_, wm_,
                            [this](const std::string& s, int c) { wm_.append_status(s, c); },
                            active_device_,
                            [this]() { enter_wizard(); },
                            [this](const std::string& name) { return set_theme(name); },
                            [this](bool turn_on) {
                                if (turn_on) {
                                    wizard_server_port_ = std::to_string(config_.server_port > 0 ? config_.server_port : 4404);
                                    wizard_server_user_ = config_.server_user;
                                    wizard_server_password_ = config_.server_password;
                                    wizard_field_ = 0;
                                    mode_ = Mode::ServerConfig;
                                    need_redraw_ = true;
                                } else {
                                    service_.stop_stream_server();
                                    config_.server_mode = false;
                                    save_config(config_);
                                    wm_.append_status("Mesh Sync Stream Server stopped.", tui_color::INFO);
                                }
                            });
                        auto res = disp.execute(submitted);
                        if (res.quit) quit_ = true;
                    } else {
                        need_redraw_ = true;
                    }
                }
                ch = getch();
            }
        }
        maybe_reconnect();
    }
    return 0;
}

void TuiApp::process_events() {
    auto events = queue_.drain_all();
    for (auto& ev : events) handle_event(ev);
    if (!events.empty()) need_redraw_ = true;
}

void TuiApp::handle_event(const MeshEvent& ev) {
    std::visit([this](const auto& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, EvConnected>) {
            wm_.append_status("*** Connected to " + e.display_name, tui_color::INFO);
            if (active_device_.empty()) {
                active_device_ = e.device;
            }
        } else if constexpr (std::is_same_v<T, EvDisconnected>) {
            wm_.append_status("*** Disconnected: " + e.reason, tui_color::ERROR);
            if (e.device == active_device_) {
                auto devices = service_.device_ids();
                active_device_.clear();
                for (const auto& id : devices) {
                    if (id != e.device) { active_device_ = id; break; }
                }
            }
            if (reconnect_attempts_.find(e.device) == reconnect_attempts_.end()) {
                reconnect_attempts_[e.device] = 0;
                s_last_reconnect_attempt = std::time(nullptr);
                wm_.append_status("*** Auto-reconnect for " + e.device + " in " +
                                  std::to_string(reconnect_delay_s_) + "s...",
                                  tui_color::INFO);
            }
        } else if constexpr (std::is_same_v<T, EvMyInfo>) {
            wm_.append_status("*** My node: " + node_num_to_id(e.my_node_num), tui_color::INFO);
        } else if constexpr (std::is_same_v<T, EvMetadata>) {
            wm_.append_status("*** Firmware: " + e.firmware_version +
                               "  HW: " + e.hw_model, tui_color::INFO);
        } else if constexpr (std::is_same_v<T, EvConfigComplete>) {
            wm_.append_status(e.rebooted ? "*** Config complete (device rebooted)"
                                          : "*** Config complete",
                              tui_color::INFO);
        } else if constexpr (std::is_same_v<T, EvNodeUpdated>) {
            const NodeDb* db = service_.db_for(e.device);
            wm_.append_status("*** Node updated: " + e.node.long_name +
                              " (" + e.node.node_id + ")", tui_color::INFO);
            std::string nick = e.node.short_name.empty()
                                   ? e.node.long_name : e.node.short_name;
            wm_.update_dm_nick(e.device, e.node.node_num, nick);
            // Rebuild nicks in existing messages if the node name changed.
            std::string old_nick = e.old_short_name.empty()
                                       ? e.old_long_name : e.old_short_name;
            if (!old_nick.empty() && old_nick != nick) {
                wm_.rebuild_all_nicks(e.device, e.node.node_num, old_nick, nick);
            }
            // Clamp nodelist cursor if viewing this device.
            if (nodelist_device_ == e.device && db) {
                int total = static_cast<int>(db->all().size());
                if (nodelist_cursor_ >= total && total > 0)
                    nodelist_cursor_ = total - 1;
            }
        } else if constexpr (std::is_same_v<T, EvChannelUpdated>) {
            std::string name = e.channel.name;
            if (e.channel.index == 0) {
                wm_.ensure_channel(e.device, e.channel.index, name);
            } else {
                wm_.update_channel_name(e.device, e.channel.index, name);
            }
            wm_.append_status("*** Channel " + std::to_string(e.channel.index) +
                              ": " + (name.empty() ? "(unnamed)" : name) +
                              " [" + e.channel.role + "]", tui_color::INFO);
        } else if constexpr (std::is_same_v<T, EvTextReceived>) {
            const NodeDb* db = service_.db_for(e.device);
            wm_.append_text(e.device, e.from_node, e.to_node, e.channel_idx,
                            e.broadcast, e.text, e.rx_time, db,
                            e.rx_snr, e.hop_start, e.hop_limit);
        } else if constexpr (std::is_same_v<T, EvAckReceived>) {
            std::string ack_text = "*** ACK " + std::to_string(e.packet_id) +
                (e.success ? " OK" : " FAIL: " + e.error_reason);
            int ack_color = e.success ? tui_color::INFO : tui_color::ERROR;
            auto m = service_.database().find_by_packet_id(e.packet_id);
            if (m && !m->window_kind.empty()) {
                wm_.append_meta(m->device, m->window_kind, m->window_target,
                                ack_text, ack_color);
            }
            wm_.append_status(ack_text, ack_color);
        } else if constexpr (std::is_same_v<T, EvLogLine>) {
            if (!e.message.empty())
                wm_.append_status("[" + e.source + "] " + e.message, tui_color::META);
        } else if constexpr (std::is_same_v<T, EvError>) {
            wm_.append_status("*** Error: " + e.message, tui_color::ERROR);
        } else if constexpr (std::is_same_v<T, EvNodeJoined>) {
            wm_.append_status("*** Node joined: " + e.node.long_name +
                              " (" + e.node.node_id + ")", tui_color::INFO);
        } else if constexpr (std::is_same_v<T, EvRawPacket>) {
            int idx = wm_.ensure_raw(e.device);
            Window& w = *wm_.windows()[idx - 1];
            char tsbuf[16];
            std::time_t secs = static_cast<std::time_t>(e.ts / 1000);
            std::tm tm{};
            #ifdef _WIN32
            ::localtime_s(&tm, &secs);
#else
            ::localtime_r(&secs, &tm);
#endif
            std::snprintf(tsbuf, sizeof(tsbuf), "%02d:%02d:%02d",
                          tm.tm_hour, tm.tm_min, tm.tm_sec);
            Line header;
            header.text = "[" + std::string(tsbuf) + "] " + e.summary +
                          "  " + std::to_string(e.hex.size()) + " bytes";
            header.is_meta = true;
            header.color_pair = tui_color::CHANNEL;
            w.append_line(header);
            std::istringstream iss(e.hex);
            std::string hline;
            while (std::getline(iss, hline)) {
                Line hexline;
                hexline.text = "  " + hline;
                hexline.color_pair = tui_color::META;
                w.append_line(hexline);
            }
            if (idx != wm_.current_index()) w.bump_activity(1);
            else { w.mark_read(); w.scroll_to_bottom(); }
        } else if constexpr (std::is_same_v<T, EvBleDeviceFound>) {
            if (e.scan_complete) {
                scan_complete_ = true;
            } else {
                // Deduplicate on the TUI thread (safe, single-threaded access).
                bool dup = false;
                for (const auto& existing : scan_entries_) {
                    if (existing.address == e.address) { dup = true; break; }
                }
                if (!dup) {
                    BleScanEntry entry;
                    entry.name = e.name;
                    entry.address = e.address;
                    entry.device_path = e.device;
                    entry.rssi = e.rssi;
                    scan_entries_.push_back(std::move(entry));
                }
            }
        }
    }, ev);
}

} // namespace meshcli
