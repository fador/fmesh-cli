#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace meshcli {

// Logical identifier for a connected Meshtastic device. For BLE this is the
// BlueZ object path of the Device1; it is used to namespace every event so
// the UI can support multiple radios simultaneously in the future.
using DeviceId = std::string;

constexpr uint32_t kBroadcastNodeNum = 0xFFFFFFFFu;

// High-level representation of a node in the mesh, distilled from the
// meshtastic::NodeInfo / User / Position / DeviceMetrics protos.
struct Node {
    uint32_t node_num = 0;            // !hexxxxxx
    std::string node_id;              // "!abcd1234"
    std::string long_name;
    std::string short_name;
    std::string hw_model;             // enum name
    std::string role;
    bool is_favorite = false;
    bool is_ignored = false;
    bool is_muted = false;
    bool is_key_verified = false;
    bool has_public_key = false;
    std::vector<uint8_t> public_key;
    std::optional<double> latitude;
    std::optional<double> longitude;
    std::optional<int32_t> altitude;
    std::optional<uint8_t> battery_level;   // 0..100, 101 = powered
    std::optional<float> voltage;
    std::optional<float> channel_util;
    std::optional<float> air_util_tx;
    std::optional<float> snr;
    std::optional<uint32_t> hops_away;
    std::optional<uint64_t> last_heard;      // unix seconds
};

// High-level representation of a configured channel on the local node.
struct Channel {
    uint32_t index = 0;
    std::string name;
    bool has_psk = false;
    std::string role; // PRIMARY / SECONDARY / DISABLED
};

// Convert a node number to the canonical "!hexxxxxx" string id.
inline std::string node_num_to_id(uint32_t num) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "!%08x", num);
    return buf;
}

// Parse a "!hexxxxxx" / "0xhex" / decimal string into a node number. Returns
// false on failure.
inline bool parse_node_id(const std::string& s, uint32_t& out) {
    if (s.empty()) return false;
    if (s[0] == '!') {
        if (s.size() < 9) return false;
        try { out = static_cast<uint32_t>(std::stoul(s.substr(1, 8), nullptr, 16)); }
        catch (...) { return false; }
        return true;
    }
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        try { out = static_cast<uint32_t>(std::stoul(s.substr(2), nullptr, 16)); }
        catch (...) { return false; }
        return true;
    }
    // Try decimal.
    try { out = static_cast<uint32_t>(std::stoul(s, nullptr, 10)); }
    catch (...) { return false; }
    return true;
}

// Thread-safe collection of nodes + channels seen on a single device.
class NodeDb {
public:
    void clear();
    void upsert_node(Node n);
    void upsert_channel(Channel c);
    [[nodiscard]] std::optional<Node> get(uint32_t node_num) const;
    [[nodiscard]] std::optional<Node> get_by_id(const std::string& id) const;
    [[nodiscard]] std::vector<Node> all() const;
    [[nodiscard]] std::vector<Channel> channels() const;
    [[nodiscard]] std::optional<Channel> channel(uint32_t idx) const;

    void set_my_node_num(uint32_t n) { my_node_num_ = n; }
    [[nodiscard]] uint32_t my_node_num() const { return my_node_num_; }

    // Find a node by a fuzzy name match (long name, short name, or node id).
    // Used by /query and /msg. Returns the best match or std::nullopt.
    [[nodiscard]] std::optional<Node> find_fuzzy(const std::string& query) const;

private:
    mutable std::mutex mu_;
    std::map<uint32_t, Node> nodes_;
    std::map<uint32_t, Channel> channels_;
    uint32_t my_node_num_ = 0;
};

} // namespace meshcli
