#include "window_manager.h"

#include "mesh/mesh_service.h"
#include "store/database.h"
#include "util/log.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace meshcli {

namespace {

std::string fmt_time(uint32_t ts) {
    if (!ts) return "??:??";
    std::time_t t = static_cast<std::time_t>(ts);
    std::tm tm{};
    ::localtime_r(&t, &tm);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    return buf;
}

std::string short_nick(const NodeDb* db, uint32_t node) {
    auto n = db ? db->get(node) : std::nullopt;
    if (n && !n->short_name.empty()) return n->short_name;
    if (n && !n->long_name.empty()) {
        std::string s = n->long_name;
        return s.substr(0, std::min<size_t>(s.size(), 12));
    }
    return node_num_to_id(node).substr(0, 10);
}

} // namespace

WindowManager::WindowManager(MeshService& service) : service_(service) {
    // Window 1 is always the status window (device-agnostic until a device
    // connects).
    auto w = std::make_unique<Window>(WindowTarget{"", "status", 0}, "status");
    by_key_["|status|0"] = 1;
    windows_.push_back(std::move(w));
}

void WindowManager::clear() {
    windows_.clear();
    by_key_.clear();
    current_ = 1;
    auto w = std::make_unique<Window>(WindowTarget{"", "status", 0}, "status");
    by_key_["|status|0"] = 1;
    windows_.push_back(std::move(w));
}

int WindowManager::add_window(std::unique_ptr<Window> w) {
    int idx = static_cast<int>(windows_.size()) + 1;
    const auto& t = w->target();
    std::string key = t.device + "|" + t.kind + "|" + std::to_string(t.target);
    by_key_[key] = idx;
    windows_.push_back(std::move(w));
    return idx;
}

int WindowManager::ensure_status(const std::string& device) {
    (void)device;
    return 1;
}

std::string WindowManager::channel_title(const std::string& device,
                                         uint32_t idx,
                                         const std::string& name) {
    (void)device;
    if (!name.empty()) return "#" + name;
    return "#ch" + std::to_string(idx);
}

std::string WindowManager::dm_title(const std::string& device, uint32_t node,
                                    const std::string& nick) {
    (void)device;
    if (!nick.empty()) return nick;
    return node_num_to_id(node);
}

int WindowManager::ensure_channel(const std::string& device, uint32_t idx,
                                  const std::string& name) {
    std::string key = device + "|channel|" + std::to_string(idx);
    auto it = by_key_.find(key);
    if (it != by_key_.end()) {
        // Update title if a real name arrived (created earlier with "").
        if (!name.empty())
            windows_[it->second - 1]->set_title(channel_title(device, idx, name));
        return it->second;
    }
    auto w = std::make_unique<Window>(
        WindowTarget{device, "channel", idx},
        channel_title(device, idx, name));
    int win_idx = add_window(std::move(w));
    load_history(win_idx);
    return win_idx;
}

int WindowManager::ensure_dm(const std::string& device, uint32_t peer_node,
                             const std::string& nick) {
    std::string key = device + "|dm|" + std::to_string(peer_node);
    auto it = by_key_.find(key);
    if (it != by_key_.end()) return it->second;
    auto w = std::make_unique<Window>(
        WindowTarget{device, "dm", peer_node},
        dm_title(device, peer_node, nick));
    int win_idx = add_window(std::move(w));
    load_history(win_idx);
    return win_idx;
}

void WindowManager::append_text(const std::string& device, uint32_t from_node,
                                uint32_t to_node, uint32_t channel_idx,
                                bool broadcast, const std::string& text,
                                uint32_t ts, const NodeDb* db) {
    int idx;
    std::string nick = short_nick(db, from_node);
    if (broadcast) {
        idx = ensure_channel(device, channel_idx, "");
    } else {
        idx = ensure_dm(device, from_node, nick);
    }
    Window& w = *windows_[idx - 1];

    // Simple mention detection: current node's short/long name in the text.
    bool mention = false;
    if (db) {
        auto me = db->get(db->my_node_num());
        if (me) {
            if (!me->short_name.empty() && text.find(me->short_name) != std::string::npos)
                mention = true;
            else if (!me->long_name.empty() && text.find(me->long_name) != std::string::npos)
                mention = true;
        }
    }

    Line line;
    line.text = "[" + fmt_time(ts) + "] <" + nick + "> " + text;
    line.color_pair = mention ? 4 : (broadcast ? 2 : 3);   // see tui.cpp color table
    w.append_line(line);

    if (idx != current_) {
        w.bump_activity(mention ? 2 : 1);
    } else {
        w.mark_read();
    }
}

void WindowManager::append_status(const std::string& text, int color_pair) {
    Window& w = *windows_[0];
    Line line;
    line.text = "[" + fmt_time(static_cast<uint32_t>(std::time(nullptr))) + "] " + text;
    line.color_pair = color_pair;
    line.is_meta = true;
    w.append_line(line);
    if (current_ != 1) w.bump_activity(1);
}

void WindowManager::select(int index) {
    if (index < 1 || index > static_cast<int>(windows_.size())) return;
    current_ = index;
    if (auto* w = current_window()) {
        w->scroll_to_bottom();
        w->mark_read();
    }
}

void WindowManager::select_next_active() {
    // Find the next window with activity, wrapping around.
    int n = static_cast<int>(windows_.size());
    for (int i = 1; i <= n; ++i) {
        int idx = ((current_ - 1 + i) % n) + 1;
        if (windows_[idx - 1]->activity() > 0) {
            select(idx);
            return;
        }
    }
}

void WindowManager::select_relative(int delta) {
    int n = static_cast<int>(windows_.size());
    if (n == 0) return;
    int idx = ((current_ - 1 + delta) % n + n) % n + 1;
    select(idx);
}

Window* WindowManager::current_window() {
    if (current_ < 1 || current_ > static_cast<int>(windows_.size())) return nullptr;
    return windows_[current_ - 1].get();
}

const WindowTarget* WindowManager::current_target() const {
    if (current_ < 1 || current_ > static_cast<int>(windows_.size())) return nullptr;
    const auto& t = windows_[current_ - 1]->target();
    if (t.kind == "status") return nullptr;
    return &t;
}

void WindowManager::load_history(int window_idx) {
    if (window_idx < 1 || window_idx > static_cast<int>(windows_.size())) return;
    Window& w = *windows_[window_idx - 1];
    const auto& t = w.target();
    if (t.kind == "status") return;

    WindowKey wk{t.device, t.kind, t.target};
    auto msgs = service_.database().recent_messages(wk, 200);
    if (msgs.empty()) return;

    const NodeDb* db = service_.db_for(t.device);
    for (const auto& m : msgs) {
        // Skip empty messages.
        if (m.text.empty()) continue;

        std::string nick;
        if (m.direction == "out") {
            // Message we sent. Use our own nick.
            uint32_t me = db ? db->my_node_num() : 0;
            nick = short_nick(db, me);
        } else {
            nick = short_nick(db, m.from_node);
        }

        Line line;
        line.text = "[" + fmt_time(static_cast<uint32_t>(m.ts)) + "] <" + nick + "> " + m.text;
        bool is_dm = (t.kind == "dm");
        bool mention = false;
        if (db) {
            auto me = db->get(db->my_node_num());
            if (me) {
                if (!me->short_name.empty() && m.text.find(me->short_name) != std::string::npos)
                    mention = true;
                else if (!me->long_name.empty() && m.text.find(me->long_name) != std::string::npos)
                    mention = true;
            }
        }
        line.color_pair = mention ? 4 : (is_dm ? 3 : 2);
        w.append_line(line);
    }
}

} // namespace meshcli
