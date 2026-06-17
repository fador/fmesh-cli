#include "tui.h"

#include "colors.h"
#include "command.h"
#include "keybinds.h"
#include "mesh/event.h"
#include "mesh/mesh_service.h"
#include "util/log.h"

#include <ncurses.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <poll.h>
#include <sstream>
#include <string>
#include <unistd.h>
#include <variant>

namespace meshcli {

namespace {

} // namespace

TuiApp::TuiApp(MeshService& service, ConcurrentQueue<MeshEvent>& queue, EventFd& wake)
    : service_(service), queue_(queue), wake_(wake), wm_(service) {}

TuiApp::~TuiApp() { teardown_ncurses(); }

void TuiApp::init_ncurses() {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    set_tabsize(4);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(tui_color::META,    COLOR_CYAN,  -1);
        init_pair(tui_color::CHANNEL, -1,          -1);
        init_pair(tui_color::DM,      COLOR_GREEN, -1);
        init_pair(tui_color::MENTION, COLOR_YELLOW,-1);
        init_pair(tui_color::ERROR,   COLOR_RED,   -1);
        init_pair(tui_color::INFO,    COLOR_CYAN,  -1);
    }
    // Disable logging to stderr now that ncurses owns the screen.
    Logger::instance().set_console(false);
    Logger::instance().set_level(LogLevel::Info);
    clear();
    refresh();
}

void TuiApp::teardown_ncurses() {
    endwin();
    Logger::instance().set_console(true);
}

std::string TuiApp::connection_info() const {
    auto devices = service_.device_ids();
    if (devices.empty()) return "no device";
    std::string s;
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
    if (reconnect_device_id_.empty()) return;

    static time_t last_attempt = 0;
    time_t now = std::time(nullptr);
    if (now - last_attempt < kReconnectIntervalS) return;
    last_attempt = now;

    ++reconnect_attempt_;
    if (reconnect_attempt_ > reconnect_max_attempts_) {
        wm_.append_status("*** Auto-reconnect gave up after " +
                          std::to_string(reconnect_max_attempts_) + " attempts",
                          tui_color::ERROR);
        reconnect_device_id_.clear();
        reconnect_attempt_ = 0;
        need_redraw_ = true;
        return;
    }

    wm_.append_status("*** Reconnect attempt " +
                      std::to_string(reconnect_attempt_) + "/" +
                      std::to_string(reconnect_max_attempts_) + "...",
                      tui_color::INFO);
    if (service_.reconnect_device(reconnect_device_id_)) {
        wm_.append_status("*** Reconnected!", tui_color::INFO);
        reconnect_device_id_.clear();
        reconnect_attempt_ = 0;
    }
    need_redraw_ = true;
}

int TuiApp::run() {
    init_ncurses();

    while (!quit_) {
        if (need_redraw_) {
            render();
            need_redraw_ = false;
        }

        // Poll stdin + eventfd.
        struct pollfd pfds[2];
        pfds[0].fd = STDIN_FILENO;
        pfds[0].events = POLLIN;
        pfds[1].fd = wake_.fd();
        pfds[1].events = POLLIN;
        int pr = ::poll(pfds, 2, 1000);   // 1s timeout for clock refresh
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
            while (ch != ERR) {
                // Handle Alt+key (ESC prefix).  In nodelay mode the second
                // byte of an ESC-sequence may not be available immediately,
                // so temporarily switch to a short blocking timeout.
                if (ch == 27) {
                    timeout(50);               // 50ms window for the next byte
                    int ch2 = getch();
                    nodelay(stdscr, TRUE);     // restore non-blocking
                    if (ch2 != ERR) {
                        // Alt+1..0 window switch.
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
                        } else if (ch2 == 'p') {
                            wm_.select_relative(-1);
                            need_redraw_ = true;
                        }
                        ch = getch();
                        continue;
                    }
                }
                if (ch == KEY_RESIZE) {
                    need_redraw_ = true;
                } else if (ch == 12) {  // Ctrl-L
                    need_redraw_ = true;
                } else if (ch == 3) {   // Ctrl-C
                    quit_ = true;
                    break;
                } else if (ch == KEY_PPAGE) {
                    if (auto* w = wm_.current_window()) {
                        w->scroll_by(1);
                        need_redraw_ = true;
                    }
                } else if (ch == KEY_NPAGE) {
                    if (auto* w = wm_.current_window()) {
                        w->scroll_by(-1);
                        need_redraw_ = true;
                    }
                } else {
                    std::string submitted;
                    if (input_.handle_key(ch, submitted)) {
                        need_redraw_ = true;
                        CommandDispatcher disp(service_, wm_,
                            [this](const std::string& s, int c) { wm_.append_status(s, c); });
                        auto res = disp.execute(submitted);
                        if (res.quit) quit_ = true;
                    } else {
                        need_redraw_ = true;
                    }
                }
                ch = getch();
            }
        }
        // Auto-reconnect check (runs on every ~1s poll cycle).
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
        } else if constexpr (std::is_same_v<T, EvDisconnected>) {
            wm_.append_status("*** Disconnected: " + e.reason, tui_color::ERROR);
            // Start auto-reconnect for this device.
            if (reconnect_device_id_.empty()) {
                reconnect_device_id_ = e.device;
                reconnect_attempt_ = 0;
                wm_.append_status("*** Auto-reconnect in " +
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
            wm_.append_text(e.device, e.node.node_num, kBroadcastNodeNum, 0,
                            true, "*** Node updated: " + e.node.long_name +
                            " (" + e.node.node_id + ")",
                            static_cast<uint32_t>(std::time(nullptr)), db);
        } else if constexpr (std::is_same_v<T, EvChannelUpdated>) {
            std::string name = e.channel.name;
            int idx = wm_.ensure_channel(e.device, e.channel.index, name);
            (void)idx;
            wm_.append_status("*** Channel " + std::to_string(e.channel.index) +
                              ": " + (name.empty() ? "(unnamed)" : name) +
                              " [" + e.channel.role + "]", tui_color::INFO);
        } else if constexpr (std::is_same_v<T, EvTextReceived>) {
            const NodeDb* db = service_.db_for(e.device);
            wm_.append_text(e.device, e.from_node, e.to_node, e.channel_idx,
                            e.broadcast, e.text, e.rx_time, db);
        } else if constexpr (std::is_same_v<T, EvAckReceived>) {
            std::string ack_text = "*** ACK " + std::to_string(e.packet_id) +
                (e.success ? " OK" : " FAIL: " + e.error_reason);
            int ack_color = e.success ? tui_color::INFO : tui_color::ERROR;
            // Route to the window where the message was sent.
            auto m = service_.database().find_by_packet_id(e.packet_id);
            if (m && !m->window_kind.empty()) {
                wm_.append_meta(m->device, m->window_kind, m->window_target,
                                ack_text, ack_color);
            }
            // Also show in status.
            wm_.append_status(ack_text, ack_color);
        } else if constexpr (std::is_same_v<T, EvLogLine>) {
            // Route device log lines to status as low-priority meta.
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
            ::localtime_r(&secs, &tm);
            std::snprintf(tsbuf, sizeof(tsbuf), "%02d:%02d:%02d",
                          tm.tm_hour, tm.tm_min, tm.tm_sec);
            // Header line.
            Line header;
            header.text = "[" + std::string(tsbuf) + "] " + e.summary +
                          "  " + std::to_string(e.hex.size()) + " bytes";
            header.is_meta = true;
            header.color_pair = tui_color::CHANNEL;
            w.append_line(header);
            // Hex lines.
            std::istringstream iss(e.hex);
            std::string hline;
            while (std::getline(iss, hline)) {
                Line hexline;
                hexline.text = "  " + hline;
                hexline.color_pair = tui_color::META;
                w.append_line(hexline);
            }
            if (idx != wm_.current_index()) w.bump_activity(1);
            else w.mark_read();
        }
    }, ev);
}

void TuiApp::render() {
    int rows = LINES;
    int cols = COLS;
    erase();

    Window* w = wm_.current_window();
    if (!w) {
        mvprintw(0, 0, "(no window)");
        refresh();
        return;
    }

    // Layout:
    //   line 0      : title bar
    //   lines 1..H  : scrollback (H = rows-3)
    //   line rows-2 : input
    //   line rows-1 : status bar
    int scrollback_top = 1;
    int scrollback_h = std::max(1, rows - 3);

    // Title bar.
    attron(A_REVERSE);
    mvhline(0, 0, ' ', cols);
    std::string title = "[" + std::to_string(wm_.current_index()) + ":" + w->title() + "]";
    mvprintw(0, 0, "%s", title.c_str());
    std::string conn = connection_info();
    mvprintw(0, std::max(0, cols - static_cast<int>(conn.size())), "%s", conn.c_str());
    attroff(A_REVERSE);

    render_scrollback(*w, scrollback_top, scrollback_h, cols);
    render_input(rows - 2, cols);
    status_bar_.render(wm_, cols, conn);

    // Place the cursor at the input line, after the prompt.
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
    move(rows - 2, static_cast<int>(cur_prompt.size() + input_.cursor()));
    refresh();
}

void TuiApp::render_scrollback(const Window& w, int top, int height, int width) {
    const auto& lines = w.lines();
    if (lines.empty()) {
        mvprintw(top, 0, "(empty)");
        return;
    }
    // Determine the window of lines to show, accounting for scroll offset.
    int total = static_cast<int>(lines.size());
    int end_idx = total - w.scroll_offset();          // exclusive
    int start_idx = std::max(0, end_idx - height);     // inclusive
    int row = top + (height - (end_idx - start_idx));  // bottom-align
    for (int i = start_idx; i < end_idx; ++i, ++row) {
        const Line& ln = lines[i];
        if (ln.color_pair) attron(COLOR_PAIR(ln.color_pair));
        if (ln.is_meta) attron(A_DIM);
        // Truncate to width.
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

    // Build a context prompt that shows where text will go:
    //   status window : ">" greyed, with a hint
    //   channel       : "#name>"
    //   dm            : "nick>"
    // If the input starts with '/', show "/" as the prompt to make it
    // obvious a command is being typed.
    Window* w = wm_.current_window();
    std::string prompt;
    int prompt_color = 0;
    const std::string& buf = input_.buf();

    if (!buf.empty() && buf[0] == '/') {
        // Command mode — always show "cmd" regardless of window.
        prompt = "cmd> ";
        prompt_color = tui_color::META;     // cyan
    } else if (w && w->target().kind == "channel") {
        prompt = w->title() + "> ";    // e.g. "#EdgeFastLow> "
        prompt_color = tui_color::CHANNEL;
    } else if (w && w->target().kind == "dm") {
        prompt = w->title() + "> ";    // e.g. "Bob> "
        prompt_color = tui_color::DM;
    } else {
        // Status window: plain text can't be sent.
        prompt = "status> ";
        prompt_color = tui_color::META;
    }

    if (prompt_color) attron(COLOR_PAIR(prompt_color) | A_BOLD);
    mvprintw(row, 0, "%s", prompt.c_str());
    if (prompt_color) attroff(COLOR_PAIR(prompt_color) | A_BOLD);

    int prompt_w = static_cast<int>(prompt.size());
    std::string s = buf;
    if (static_cast<int>(s.size()) > width - prompt_w) {
        // Show the tail so the cursor stays visible.
        size_t start = s.size() - (width - prompt_w);
        s = s.substr(start);
    }
    mvprintw(row, prompt_w, "%s", s.c_str());
}

} // namespace meshcli
