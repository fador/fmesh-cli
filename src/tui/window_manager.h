#pragma once

#include "mesh/node_db.h"
#include "window.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace meshcli {

class MeshService;

// Manages the ordered list of windows and routes messages to the right one.
// Windows are numbered 1..N for Alt+N switching. Window 1 is always status.
class WindowManager {
public:
    explicit WindowManager(MeshService& service);

    // Reset everything (e.g. on full reconnect).
    void clear();

    // Ensure a window exists for the given target; returns its index (>=1).
    int ensure_status(const std::string& device);
    int ensure_channel(const std::string& device, uint32_t channel_idx,
                       const std::string& name);
    int ensure_dm(const std::string& device, uint32_t peer_node,
                   const std::string& nick);

    // Ensure a raw-packet capture window exists (used for live hex dump).
    int ensure_raw(const std::string& device);

    // Ensure an interactive node-list window exists for the given device.
    int ensure_nodelist(const std::string& device);

    // Append a received text message to the right window and bump activity.
    void append_text(const std::string& device, uint32_t from_node,
                     uint32_t to_node, uint32_t channel_idx, bool broadcast,
                     const std::string& text, uint32_t ts, const NodeDb* db,
                     float rx_snr = 0, uint32_t hop_start = 0, uint32_t hop_limit = 0);

    // Append a status/info/meta line to the status window.
    void append_status(const std::string& text, int color_pair = 0);

    // Append an outgoing message immediately (before mesh echo arrives).
    void append_outgoing(const std::string& device, const std::string& kind,
                         uint32_t target, const std::string& text,
                         const NodeDb* db);

    // Append a meta line to a channel or DM window (for ACK routing).
    void append_meta(const std::string& device, const std::string& kind,
                     uint32_t target, const std::string& text, int color_pair);

    // Window selection.
    void select(int index);
    void select_next_active();
    void select_relative(int delta);   // +1 / -1
    [[nodiscard]] int current_index() const { return current_; }
    [[nodiscard]] Window* current_window();
    [[nodiscard]] const std::vector<std::unique_ptr<Window>>& windows() const { return windows_; }

    // Close the window at index (1-based) if it is a channel/dm with no messages.
    // Returns true if the window was closed.
    bool close_if_empty(int index);

    // Look up the routing target of the current window (returns nullptr if
    // the status window is active — you can't send text from there).
    [[nodiscard]] const WindowTarget* current_target() const;

private:
    int add_window(std::unique_ptr<Window> w);
    void load_history(int window_idx);
    std::string channel_title(const std::string& device, uint32_t idx,
                              const std::string& name);
    std::string dm_title(const std::string& device, uint32_t node,
                         const std::string& nick);

    MeshService& service_;
    std::vector<std::unique_ptr<Window>> windows_;
    // Keyed lookups so we don't create duplicate windows.
    std::map<std::string, int> by_key_;   // "device|kind|target" -> index
    int current_ = 1;   // 1-based; status is always 1
};

} // namespace meshcli
