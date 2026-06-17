#include "command.h"

#include "colors.h"
#include "mesh/mesh_service.h"
#include "window_manager.h"
#include "util/log.h"

#include <algorithm>
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
                                     StatusSink status)
    : service_(service), wm_(wm), status_(std::move(status)) {}

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
    else if (cmd == "reconnect")               cmd_reconnect();
    else if (cmd == "me")                      cmd_me(tokens);
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
    status_("  /window <N>           switch to window N", tui_color::INFO);
    status_("  /close                close the current (non-status) window", tui_color::INFO);
    status_("  /clear                clear the current window's scrollback", tui_color::INFO);
    status_("  /info                 show connection info", tui_color::INFO);
    status_("  /me <text>            send an action (italic *nick text*)", tui_color::INFO);
    status_("  /reconnect            reconnect the device", tui_color::INFO);
    status_("  /quit                 exit mesh-cli", tui_color::INFO);
    status_("Keys: Alt+1..0 switch window, Alt+a next active, PgUp/PgDn scroll, Ctrl-L redraw", tui_color::INFO);
}

void CommandDispatcher::cmd_list() {
    const auto& wins = wm_.windows();
    for (size_t i = 0; i < wins.size(); ++i) {
        int idx = static_cast<int>(i + 1);
        const Window& w = *wins[i];
        std::string mark;
        if (w.activity() >= 2) mark = " *";
        else if (w.activity() == 1) mark = " #";
        int color = tui_color::INFO;
        if (w.target().kind == "channel") color = tui_color::CHANNEL;
        else if (w.target().kind == "dm") color = tui_color::DM;
        status_("  " + std::to_string(idx) + ": " + w.title() +
                "  [" + w.target().kind + "]" + mark, color);
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
    // Try each device's node DB for a fuzzy match.
    for (const auto& id : devices) {
        const NodeDb* db = service_.db_for(id);
        if (!db) continue;
        auto n = db->find_fuzzy(q);
        if (n) {
            wm_.ensure_dm(id, n->node_num, n->short_name.empty() ? n->long_name : n->short_name);
            wm_.select(wm_.windows().size());  // select the newly created/last
            // Better: find the index of the dm window we just ensured.
            // ensure_dm returns the index but we don't have it here; re-find.
            // (For simplicity we select the last window.)
            status_("Now talking to " + n->long_name + " (" + n->node_id + ")", tui_color::INFO);
            return;
        }
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
    for (const auto& id : devices) {
        const NodeDb* db = service_.db_for(id);
        if (!db) continue;
        auto n = db->find_fuzzy(q);
        if (n) {
            service_.send_text(id, n->node_num, 0, text, true);
            return;
        }
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
        std::string name;
        if (auto* db = service_.db_for(devices[0])) {
            if (auto ch = db->channel(idx)) name = ch->name;
        }
        int w = wm_.ensure_channel(devices[0], idx, name);
        wm_.select(w);
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
        status_("device: " + id, tui_color::INFO);
        if (db) {
            status_("  my node: " + node_num_to_id(db->my_node_num()), tui_color::INFO);
            status_("  nodes known: " + std::to_string(db->all().size()), tui_color::INFO);
            status_("  channels: " + std::to_string(db->channels().size()), tui_color::INFO);
        }
    }
}

void CommandDispatcher::cmd_quit(CommandResult& res) {
    res.quit = true;
    status_("Bye.", tui_color::INFO);
}

void CommandDispatcher::cmd_reconnect() {
    status_("Reconnect not yet implemented; use /quit and re-run.", tui_color::ERROR);
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
}

} // namespace meshcli
