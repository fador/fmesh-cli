#include "command.h"

#include "app/config.h"
#include "colors.h"
#include "mesh/mesh_service.h"
#include "window_manager.h"
#include "util/log.h"

#include <algorithm>
#include <ctime>
#include <map>
#include <sstream>

namespace meshcli {

namespace {

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string tok;
    while (iss >> tok) out.push_back(tok);
    return out;
}

} // namespace

CommandDispatcher::CommandDispatcher(MeshService& service, WindowManager& wm,
                                     StatusSink status,
                                     std::string& active_device,
                                     std::function<void()> on_scan)
    : service_(service), wm_(wm), status_(std::move(status)),
      active_device_(active_device), on_scan_(std::move(on_scan)) {}

CommandResult CommandDispatcher::execute(const std::string& line) {
    CommandResult res;
    if (line.empty()) return res;

    if (line[0] != '/') {
        // Plain text: send to the current window's target.
        const auto* tgt = wm_.current_target();
        if (!tgt) {
            status_("Cannot send text from the status window. Use /query <node> or /channel <n>.", tui_color::ERROR);
            return res;
        }
        if (tgt->kind == "channel") {
            service_.send_text(tgt->device, kBroadcastNodeNum, tgt->target, line, false);
        } else if (tgt->kind == "dm") {
            service_.send_text(tgt->device, tgt->target, 0, line, true);
        }
        // Echo the message immediately in the window (before mesh echo).
        const NodeDb* db = service_.db_for(tgt->device);
        wm_.append_outgoing(tgt->device, tgt->kind, tgt->target, line, db);
        return res;
    }

    auto tokens = split(line.substr(1));
    if (tokens.empty()) return res;
    const std::string cmd = tokens[0];
    tokens.erase(tokens.begin());

    if (cmd == "help" || cmd == "h")          cmd_help();
    else if (cmd == "list" || cmd == "windows") cmd_list();
    else if (cmd == "nodes" || cmd == "who")   cmd_nodes();
    else if (cmd == "query" || cmd == "q")     cmd_query(tokens);
    else if (cmd == "msg" || cmd == "m")       cmd_msg(tokens);
    else if (cmd == "close" || cmd == "c")     cmd_close();
    else if (cmd == "window" || cmd == "w")    cmd_window(tokens);
    else if (cmd == "channel" || cmd == "ch")  cmd_channel(tokens);
    else if (cmd == "clear")                   cmd_clear();
    else if (cmd == "info" || cmd == "i")      cmd_info();
    else if (cmd == "quit" || cmd == "exit")   cmd_quit(res);
    else if (cmd == "reconnect")               cmd_reconnect(tokens);
    else if (cmd == "device" || cmd == "dev")  cmd_device(tokens);
    else if (cmd == "me")                      cmd_me(tokens);
    else if (cmd == "config" || cmd == "cfg")  cmd_config(tokens);
    else if (cmd == "whois" || cmd == "wi")    cmd_whois(tokens);
    else if (cmd == "raw")                     cmd_raw(tokens);
    else if (cmd == "stats" || cmd == "st")    cmd_stats();
    else if (cmd == "topic" || cmd == "t")     cmd_topic();
    else if (cmd == "lastlog" || cmd == "l")   cmd_lastlog(tokens);
    else if (cmd == "connect")                cmd_connect(tokens);
    else if (cmd == "disconnect" || cmd == "dc") cmd_disconnect(tokens);
    else if (cmd == "scan" || cmd == "s")      cmd_scan();
    else {
        status_("Unknown command: /" + cmd + " (try /help)", tui_color::ERROR);
    }
    return res;
}

void CommandDispatcher::cmd_help() {
    status_("Commands:", tui_color::INFO);
    status_("  /help                 this help", tui_color::INFO);
    status_("  /list                 list windows", tui_color::INFO);
    status_("  /nodes                list known nodes", tui_color::INFO);
    status_("  /query <node|nick>    open a DM window with a node", tui_color::INFO);
    status_("  /msg <node|nick> <text>  send a DM without switching windows", tui_color::INFO);
    status_("  /channel <n>          switch to / create channel window n", tui_color::INFO);
    status_("                          (uses active device; Ctrl+X to cycle)", tui_color::INFO);
    status_("  /window <N>           switch to window N", tui_color::INFO);
    status_("  /close                close the current (non-status) window", tui_color::INFO);
    status_("  /clear                clear the current window's scrollback", tui_color::INFO);
    status_("  /info                 show connection info (all devices)", tui_color::INFO);
    status_("  /me <text>            send an action (italic *nick text*)", tui_color::INFO);
    status_("  /reconnect [id]       reconnect active/all devices (or specify ID)", tui_color::INFO);
    status_("  /config [section]     show device config (all devices, optional filter)", tui_color::INFO);
    status_("  /whois <node|nick>    show detailed node information", tui_color::INFO);
    status_("  /raw [N]              show last N raw packets (all devices, default 5)", tui_color::INFO);
    status_("  /stats                show packet statistics (all devices)", tui_color::INFO);
    status_("  /topic                show current channel details", tui_color::INFO);
    status_("  /lastlog <pattern>    search scrollback for pattern", tui_color::INFO);
    status_("  /connect <spec>       connect a new device at runtime", tui_color::INFO);
    status_("                        spec: ble:<name>[:<pin>]", tui_color::INFO);
    status_("                              addr:<mac>[:<pin>]", tui_color::INFO);
    status_("                              tcp:<host>[:<port>]", tui_color::INFO);
    status_("                              serial:<path>[:<baud>]", tui_color::INFO);
    status_("  /disconnect [id]      disconnect a device (no arg: list IDs)", tui_color::INFO);
    status_("  /scan                 open the interactive connection wizard", tui_color::INFO);
    status_("  /device [id]          show or switch active device", tui_color::INFO);
    status_("  /quit                 exit mesh-cli", tui_color::INFO);
    status_("Keys: Alt+1..0 switch window, Alt+a next active, PgUp/PgDn scroll, Ctrl-L redraw, Ctrl-X cycle device", tui_color::INFO);
}

void CommandDispatcher::cmd_list() {
    const auto& wins = wm_.windows();
    // Collect (index, window*) pairs, sort by activity desc (status always first).
    std::vector<std::pair<int, const Window*>> sorted;
    for (size_t i = 0; i < wins.size(); ++i) {
        sorted.emplace_back(static_cast<int>(i + 1), wins[i].get());
    }
    std::stable_sort(sorted.begin(), sorted.end(),
        [](const auto& a, const auto& b) {
            if (a.second->target().kind == "status") return true;
            if (b.second->target().kind == "status") return false;
            return a.second->activity() > b.second->activity();
        });
    for (const auto& [idx, w] : sorted) {
        std::string mark;
        if (w->activity() >= 2) mark = " *";
        else if (w->activity() == 1) mark = " #";
        if (w->unread() > 0) mark += std::to_string(w->unread());
        int color = tui_color::INFO;
        if (w->target().kind == "channel") color = tui_color::CHANNEL;
        else if (w->target().kind == "dm") color = tui_color::DM;
        status_("  " + std::to_string(idx) + ": " + w->title() +
                "  [" + w->target().kind + "]" + mark, color);
    }
}

void CommandDispatcher::cmd_nodes() {
    auto devices = service_.device_ids();
    if (devices.empty()) { status_("(no devices connected)", tui_color::ERROR); return; }
    for (const auto& id : devices) {
        const NodeDb* db = service_.db_for(id);
        if (!db) continue;
        status_("Nodes on " + id + ":", tui_color::INFO);
        auto nodes = db->all();
        std::sort(nodes.begin(), nodes.end(),
                  [](const Node& a, const Node& b) {
                      return a.long_name < b.long_name;
                  });
        for (const auto& n : nodes) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "  %-16s %-6s %s  batt=%d%%",
                          n.long_name.c_str(), n.short_name.c_str(),
                          n.node_id.c_str(),
                          n.battery_level.value_or(0));
            status_(buf, tui_color::CHANNEL);
        }
    }
}

void CommandDispatcher::cmd_query(const std::vector<std::string>& args) {
    if (args.empty()) { status_("Usage: /query <node|nick>", tui_color::ERROR); return; }
    std::string q = args[0];
    auto devices = service_.device_ids();
    if (devices.empty()) { status_("(no devices connected)", tui_color::ERROR); return; }
    // Try active device first, then the rest.
    auto try_device = [&](const std::string& id) -> bool {
        const NodeDb* db = service_.db_for(id);
        if (!db) return false;
        auto n = db->find_fuzzy(q);
        if (n) {
            wm_.ensure_dm(id, n->node_num, n->short_name.empty() ? n->long_name : n->short_name);
            wm_.select(wm_.windows().size());
            status_("Now talking to " + n->long_name + " (" + n->node_id + ")", tui_color::INFO);
            return true;
        }
        return false;
    };
    if (!active_device_.empty() && try_device(active_device_)) return;
    for (const auto& id : devices) {
        if (id == active_device_) continue;
        if (try_device(id)) return;
    }
    status_("No node matched '" + q + "'", tui_color::ERROR);
}

void CommandDispatcher::cmd_msg(const std::vector<std::string>& args) {
    if (args.size() < 2) { status_("Usage: /msg <node|nick> <text>", tui_color::ERROR); return; }
    std::string q = args[0];
    std::string text;
    for (size_t i = 1; i < args.size(); ++i) {
        if (i > 1) text += ' ';
        text += args[i];
    }
    auto devices = service_.device_ids();
    auto try_device = [&](const std::string& id) -> bool {
        const NodeDb* db = service_.db_for(id);
        if (!db) return false;
        auto n = db->find_fuzzy(q);
        if (n) {
            service_.send_text(id, n->node_num, 0, text, true);
            wm_.append_outgoing(id, "dm", n->node_num, text, db);
            return true;
        }
        return false;
    };
    if (!active_device_.empty() && try_device(active_device_)) return;
    for (const auto& id : devices) {
        if (id == active_device_) continue;
        if (try_device(id)) return;
    }
    status_("No node matched '" + q + "'", tui_color::ERROR);
}

void CommandDispatcher::cmd_close() {
    const auto* tgt = wm_.current_target();
    if (!tgt) { status_("Cannot close the status window", tui_color::ERROR); return; }
    // Closing is implemented by selecting the previous window; the window
    // object itself stays (irssi keeps history). For v1 we just switch away.
    wm_.select_relative(-1);
    status_("(window kept; switched away. Use /clear to wipe history)", tui_color::INFO);
}

void CommandDispatcher::cmd_window(const std::vector<std::string>& args) {
    if (args.empty()) { status_("Usage: /window <N>", tui_color::ERROR); return; }
    try {
        int n = std::stoi(args[0]);
        wm_.select(n);
    } catch (...) { status_("Invalid window number", tui_color::ERROR); }
}

void CommandDispatcher::cmd_channel(const std::vector<std::string>& args) {
    if (args.empty()) { status_("Usage: /channel <n>", tui_color::ERROR); return; }
    try {
        uint32_t idx = static_cast<uint32_t>(std::stoul(args[0]));
        auto devices = service_.device_ids();
        if (devices.empty()) { status_("(no devices connected)", tui_color::ERROR); return; }
        // Use the active device (set via Ctrl+X or /device), falling back to
        // the first available device with a valid DB.
        std::string dev = active_device_;
        if (dev.empty() || !service_.db_for(dev)) {
            for (const auto& d : devices) {
                if (service_.db_for(d)) { dev = d; break; }
            }
            if (dev.empty()) dev = devices[0];
        }
        std::string name;
        std::string role;
        if (auto* db = service_.db_for(dev)) {
            if (auto ch = db->channel(idx)) {
                name = ch->name;
                role = ch->role;
            }
        }
        int w = wm_.ensure_channel(dev, idx, name);
        wm_.select(w);
        if (role == "DISABLED")
            status_("Note: channel " + std::to_string(idx) + " is DISABLED",
                    tui_color::ERROR);
        else if (name.empty() && role != "DISABLED")
            status_("Note: channel " + std::to_string(idx) + " has no name",
                    tui_color::INFO);
    } catch (...) { status_("Invalid channel index", tui_color::ERROR); }
}

void CommandDispatcher::cmd_clear() {
    if (auto* w = wm_.current_window()) w->clear();
}

void CommandDispatcher::cmd_info() {
    auto devices = service_.device_ids();
    if (devices.empty()) { status_("(no devices)", tui_color::ERROR); return; }
    for (const auto& id : devices) {
        const NodeDb* db = service_.db_for(id);
        std::string name = service_.display_name_for(id);
        std::string fw   = service_.firmware_for(id);
        std::string hw   = service_.hw_model_for(id);

        status_("Device: " + name + " (" + id + ")", tui_color::INFO);
        if (!fw.empty() || !hw.empty())
            status_("  Firmware: " + fw + "  HW: " + hw, tui_color::INFO);
        if (db) {
            auto me = db->get(db->my_node_num());
            if (me) {
                std::string batt;
                if (me->battery_level)
                    batt = " batt=" + std::to_string(*me->battery_level) + "%";
                if (me->voltage)
                    batt += " " + std::to_string(*me->voltage) + "V";
                status_("  My node: " + me->long_name + " (" + me->short_name
                        + ") " + me->node_id + batt, tui_color::CHANNEL);
            } else {
                status_("  My node: " + node_num_to_id(db->my_node_num()),
                        tui_color::INFO);
            }
            status_("  Nodes known: " + std::to_string(db->all().size()),
                    tui_color::INFO);
            status_("  Channels: " + std::to_string(db->channels().size()),
                    tui_color::INFO);
        }
    }
}

void CommandDispatcher::cmd_raw(const std::vector<std::string>& args) {
    int count = 5;
    if (!args.empty()) {
        try { count = std::stoi(args[0]); }
        catch (...) { status_("Invalid number", tui_color::ERROR); return; }
        if (count < 1) count = 1;
        if (count > 50) count = 50;
    }
    auto devices = service_.device_ids();
    if (devices.empty()) { status_("(no devices connected)", tui_color::ERROR); return; }
    for (const auto& id : devices) {
        auto pkts = service_.raw_packets_for(id);
        if (pkts.empty()) {
            status_("Raw packets for " + service_.display_name_for(id)
                    + ": (none yet)", tui_color::INFO);
            continue;
        }
        // Show last `count` packets from the end.
        size_t start = (pkts.size() > static_cast<size_t>(count))
                           ? pkts.size() - count : 0;
        status_("Raw packets for " + service_.display_name_for(id)
                + " (" + std::to_string(pkts.size() - start) + " of "
                + std::to_string(pkts.size()) + "):", tui_color::INFO);
        for (size_t i = start; i < pkts.size(); ++i) {
            const auto& p = pkts[i];
            // Show timestamp + summary header
            char tsbuf[16];
            std::time_t secs = static_cast<std::time_t>(p.ts / 1000);
            std::tm tm{};
            ::localtime_r(&secs, &tm);
            std::snprintf(tsbuf, sizeof(tsbuf), "%02d:%02d:%02d",
                          tm.tm_hour, tm.tm_min, tm.tm_sec);
            status_(std::string(tsbuf) + " [" + std::to_string(i) + "] "
                    + p.summary + "  " + std::to_string(p.hex.size()) + " bytes",
                    tui_color::CHANNEL);
            // Show hex dump (each line as a separate status line)
            std::istringstream iss(p.hex);
            std::string line;
            while (std::getline(iss, line))
                status_("    " + line, tui_color::META);
        }
    }
}

void CommandDispatcher::cmd_stats() {
    auto devices = service_.device_ids();
    if (devices.empty()) { status_("(no devices connected)", tui_color::ERROR); return; }
    for (const auto& id : devices) {
        auto pkts = service_.raw_packets_for(id);
        status_("Stats for " + service_.display_name_for(id)
                + " (" + std::to_string(pkts.size()) + " packets):",
                tui_color::INFO);

        // Count by extracting the first word of each summary.
        std::map<std::string, int> counts;
        for (const auto& p : pkts) {
            std::string type = p.summary;
            auto sp = type.find(' ');
            if (sp != std::string::npos) type = type.substr(0, sp);
            counts[type]++;
        }

        // Sort by count descending.
        std::vector<std::pair<std::string, int>> sorted(counts.begin(), counts.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });

        for (const auto& [type, count] : sorted) {
            char buf[80];
            std::snprintf(buf, sizeof(buf), "  %-20s %d", type.c_str(), count);
            status_(buf, tui_color::CHANNEL);
        }
    }
}

void CommandDispatcher::cmd_topic() {
    const auto* tgt = wm_.current_target();
    if (!tgt) {
        status_("No channel selected. Use /channel <n> to switch.",
                tui_color::ERROR);
        return;
    }
    if (tgt->kind == "dm") {
        // Show DM peer info
        const NodeDb* db = service_.db_for(tgt->device);
        if (db) {
            auto n = db->get(tgt->target);
            if (n) {
                status_("DM with " + n->long_name + " (" + n->node_id + ")",
                        tui_color::CHANNEL);
                if (!n->hw_model.empty())
                    status_("  HW: " + n->hw_model, tui_color::INFO);
                if (n->battery_level)
                    status_("  Battery: " + std::to_string(*n->battery_level) + "%",
                            tui_color::INFO);
                if (n->snr)
                    status_("  SNR: " + std::to_string(*n->snr) + " dB",
                            tui_color::INFO);
                return;
            }
        }
        status_("DM with node " + node_num_to_id(tgt->target), tui_color::INFO);
        return;
    }
    if (tgt->kind == "channel") {
        const NodeDb* db = service_.db_for(tgt->device);
        if (db) {
            auto ch = db->channel(tgt->target);
            if (ch) {
                status_("Channel " + std::to_string(ch->index) + ": " + ch->name,
                        tui_color::CHANNEL);
                status_("  Role: " + ch->role, tui_color::INFO);
                status_("  PSK: " + std::string(ch->has_psk ? "set" : "none"),
                        tui_color::INFO);
                return;
            }
        }
        status_("Channel " + std::to_string(tgt->target), tui_color::INFO);
        return;
    }
    status_("Not a channel or DM window.", tui_color::ERROR);
}

void CommandDispatcher::cmd_lastlog(const std::vector<std::string>& args) {
    if (args.empty()) {
        status_("Usage: /lastlog <pattern>", tui_color::ERROR);
        return;
    }
    std::string pattern;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) pattern += ' ';
        pattern += args[i];
    }
    Window* w = wm_.current_window();
    if (!w) return;

    // Case-insensitive search.
    std::string lower_pat = pattern;
    std::transform(lower_pat.begin(), lower_pat.end(), lower_pat.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    int matches = 0;
    const auto& lines = w->lines();
    for (const auto& line : lines) {
        std::string lower = line.text;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lower.find(lower_pat) != std::string::npos) {
            status_(line.text, line.color_pair);
            ++matches;
        }
    }
    status_("-- " + std::to_string(matches) + " matches for '" + pattern + "' --",
            tui_color::INFO);
}

void CommandDispatcher::cmd_quit(CommandResult& res) {
    res.quit = true;
    status_("Bye.", tui_color::INFO);
}

void CommandDispatcher::cmd_config(const std::vector<std::string>& args) {
    auto devices = service_.device_ids();
    if (devices.empty()) {
        status_("(no devices connected)", tui_color::ERROR);
        return;
    }
    std::string filter;
    if (!args.empty()) filter = args[0];

    for (const auto& id : devices) {
        status_("Configuration for " + service_.display_name_for(id)
                + (filter.empty() ? ":" : " (filter: " + filter + "):"),
                tui_color::INFO);
        auto lines = service_.config_lines_for(id);
        if (lines.empty()) {
            status_("  (no config data received yet)", tui_color::ERROR);
        } else {
            bool in_section = filter.empty();
            for (const auto& l : lines) {
                if (l.size() > 4 && l[0] == '-' && l[1] == '-' && l[2] == '-') {
                    std::string sec = l.substr(4);
                    auto end = sec.find(" ---");
                    if (end != std::string::npos) sec = sec.substr(0, end);
                    in_section = filter.empty();
                    if (!filter.empty()) {
                        std::string lower = sec;
                        std::transform(lower.begin(), lower.end(), lower.begin(),
                                       [](unsigned char c){ return std::tolower(c); });
                        if (lower.find(filter) != std::string::npos)
                            in_section = true;
                    }
                    if (in_section)
                        status_("  " + l, tui_color::INFO);
                } else if (in_section) {
                    status_("  " + l, tui_color::CHANNEL);
                }
            }
        }
    }
}

void CommandDispatcher::cmd_whois(const std::vector<std::string>& args) {
    if (args.empty()) { status_("Usage: /whois <node|nick>", tui_color::ERROR); return; }
    std::string q = args[0];
    auto devices = service_.device_ids();
    if (devices.empty()) { status_("(no devices connected)", tui_color::ERROR); return; }
    auto show = [&](const std::string& id, const Node& n) {
        status_("Node: " + n.long_name + " (" + n.short_name + ")"
                + "  [device: " + service_.display_name_for(id) + "]",
                tui_color::CHANNEL);
        status_("  ID:       " + n.node_id, tui_color::INFO);
        if (!n.hw_model.empty())
            status_("  HW:       " + n.hw_model, tui_color::INFO);
        if (!n.role.empty())
            status_("  Role:     " + n.role, tui_color::INFO);
        if (n.battery_level)
            status_("  Battery:  " + std::to_string(*n.battery_level) + "%", tui_color::INFO);
        if (n.voltage) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.2f", *n.voltage);
            status_("  Voltage:  " + std::string(buf) + " V", tui_color::INFO);
        }
        if (n.snr) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.1f", *n.snr);
            status_("  SNR:      " + std::string(buf) + " dB", tui_color::INFO);
        }
        if (n.channel_util) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.1f", *n.channel_util * 100.0f);
            status_("  Ch util:  " + std::string(buf) + "%", tui_color::INFO);
        }
        if (n.air_util_tx) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.1f", *n.air_util_tx * 100.0f);
            status_("  Air util: " + std::string(buf) + "%", tui_color::INFO);
        }
        if (n.hops_away)
            status_("  Hops:     " + std::to_string(*n.hops_away), tui_color::INFO);
        if (n.last_heard) {
            std::time_t t = static_cast<std::time_t>(*n.last_heard);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", std::localtime(&t));
            status_("  Heard:    " + std::string(buf), tui_color::INFO);
        }
        if (n.latitude && n.longitude) {
            char buf[80];
            std::snprintf(buf, sizeof(buf), "  Pos:      %.6f, %.6f  alt=%d m",
                          *n.latitude, *n.longitude, n.altitude.value_or(0));
            status_(buf, tui_color::INFO);
        }
        status_("  Flags:    " +
                std::string(n.is_favorite ? "fav " : "") +
                std::string(n.is_muted ? "muted " : "") +
                std::string(n.is_key_verified ? "verified " : "") +
                std::string(n.has_public_key ? "PKI" : ""),
                tui_color::INFO);
    };
    auto try_device = [&](const std::string& id) -> bool {
        const NodeDb* db = service_.db_for(id);
        if (!db) return false;
        auto n = db->find_fuzzy(q);
        if (n) { show(id, *n); return true; }
        return false;
    };
    if (!active_device_.empty() && try_device(active_device_)) return;
    for (const auto& id : devices) {
        if (id == active_device_) continue;
        if (try_device(id)) return;
    }
    status_("No node matched '" + q + "'", tui_color::ERROR);
}

void CommandDispatcher::cmd_reconnect(const std::vector<std::string>& args) {
    auto devices = service_.device_ids();
    if (devices.empty()) {
        status_("(no devices connected)", tui_color::ERROR);
        return;
    }
    if (!args.empty()) {
        // Per-device reconnect: partial match on device ID.
        std::string q = args[0];
        for (const auto& id : devices) {
            if (id.find(q) != std::string::npos || q == id) {
                if (service_.reconnect_device(id)) {
                    status_("Reconnecting to " + id + "...", tui_color::INFO);
                } else {
                    status_("Reconnect failed for " + id, tui_color::ERROR);
                }
                return;
            }
        }
        status_("No device matched: " + q, tui_color::ERROR);
        return;
    }
    // Reconnect all.
    for (const auto& id : devices) {
        if (service_.reconnect_device(id)) {
            status_("Reconnecting to " + id + "...", tui_color::INFO);
        } else {
            status_("Reconnect failed for " + id, tui_color::ERROR);
        }
    }
}

void CommandDispatcher::cmd_me(const std::vector<std::string>& args) {
    std::string text;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) text += ' ';
        text += args[i];
    }
    const auto* tgt = wm_.current_target();
    if (!tgt) { status_("Cannot /me from status window", tui_color::ERROR); return; }
    if (tgt->kind == "channel") {
        service_.send_text(tgt->device, kBroadcastNodeNum, tgt->target, "* " + text, false);
    } else {
        service_.send_text(tgt->device, tgt->target, 0, "* " + text, true);
    }
    const NodeDb* db = service_.db_for(tgt->device);
    wm_.append_outgoing(tgt->device, tgt->kind, tgt->target, "* " + text, db);
}

void CommandDispatcher::cmd_connect(const std::vector<std::string>& args) {
    if (args.empty()) {
        status_("Usage: /connect <spec>", tui_color::ERROR);
        status_("  ble:<name>[:<pin>]  addr:<mac>[:<pin>]", tui_color::INFO);
        status_("  tcp:<host>[:<port>]  serial:<path>[:<baud>]", tui_color::INFO);
        return;
    }
    BleDeviceSpec spec;
    if (!parse_device_spec(args[0], spec)) {
        status_("Invalid spec: " + args[0], tui_color::ERROR);
        return;
    }
    std::string id = service_.connect_device(spec, false);
    if (id.empty()) {
        status_("Failed to connect to " + args[0], tui_color::ERROR);
    } else {
        status_("Connected to " + args[0] + " (" + id + ")", tui_color::INFO);
    }
}

void CommandDispatcher::cmd_disconnect(const std::vector<std::string>& args) {
    auto devices = service_.device_ids();
    if (devices.empty()) {
        status_("(no devices connected)", tui_color::ERROR);
        return;
    }
    if (args.empty()) {
        status_("Connected devices:", tui_color::INFO);
        for (const auto& id : devices)
            status_("  " + id + "  " + service_.display_name_for(id), tui_color::CHANNEL);
        status_("Usage: /disconnect <id>", tui_color::INFO);
        return;
    }
    std::string id = args[0];
    // Allow partial match on device ID.
    for (const auto& did : devices) {
        if (did.find(id) != std::string::npos || id == did) {
            if (service_.disconnect_device(did)) {
                status_("Disconnected " + did, tui_color::INFO);
            } else {
                status_("Failed to disconnect " + did, tui_color::ERROR);
            }
            return;
        }
    }
    status_("No device matched: " + id, tui_color::ERROR);
}

void CommandDispatcher::cmd_device(const std::vector<std::string>& args) {
    auto devices = service_.device_ids();
    if (devices.empty()) {
        status_("(no devices connected)", tui_color::ERROR);
        return;
    }
    if (args.empty()) {
        // List devices, marking the active one.
        status_("Connected devices:", tui_color::INFO);
        for (const auto& id : devices) {
            std::string marker = (id == active_device_) ? " *" : "  ";
            status_("  " + marker + " " + service_.display_name_for(id)
                    + "  (" + id + ")",
                    (id == active_device_) ? tui_color::CHANNEL : tui_color::INFO);
        }
        return;
    }
    // Switch active device by partial match on ID or display name.
    std::string q = args[0];
    for (const auto& id : devices) {
        std::string dname = service_.display_name_for(id);
        if (id.find(q) != std::string::npos
            || dname.find(q) != std::string::npos
            || q == id) {
            active_device_ = id;
            status_("Active device: " + dname + " (" + id + ")", tui_color::INFO);
            return;
        }
    }
    status_("No device matched: " + q, tui_color::ERROR);
}

void CommandDispatcher::cmd_scan() {
    if (on_scan_) {
        on_scan_();
    } else {
        status_("Interactive scan is only available from the TUI.", tui_color::ERROR);
    }
}

} // namespace meshcli
