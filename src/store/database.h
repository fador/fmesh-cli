#pragma once

#include "mesh/node_db.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace meshcli {

// Logical window identifier: kind + target node/channel + which device.
//   kind="status"   : the status window (target ignored)
//   kind="channel"  : a broadcast channel window; target = channel index
//   kind="dm"       : a direct-message window; target = peer node num
struct WindowKey {
    std::string device;     // DeviceId ("" = any/legacy)
    std::string kind;
    uint32_t target = 0;

    bool operator==(const WindowKey& o) const {
        return device == o.device && kind == o.kind && target == o.target;
    }
};

struct StoredMessage {
    int64_t rowid = 0;
    std::string device;
    std::string window_kind;
    uint32_t window_target = 0;
    std::string direction;   // "in" or "out"
    uint32_t from_node = 0;
    uint32_t to_node = 0;
    uint32_t channel_idx = 0;
    std::string text;
    uint64_t ts = 0;         // unix seconds
    uint32_t packet_id = 0;
    std::string ack_state;   // "pending" / "acked" / "naked" / ""
    float rx_snr = 0.0f;
    int32_t rx_rssi = 0;
    uint32_t hop_start = 0;
    uint32_t hop_limit = 0;
    uint32_t relay_node = 0;
};

// SQLite-backed persistence. One DB file holds nodes, channels, messages,
// and window metadata for all connected devices. Thread-safe via SQLite's
// own mutex (SQLITE_THREADSAFE=1, serialized mode by default on this system).
class Database {
public:
    Database();
    ~Database();

    // Open (or create) the database at `path`. Returns false on failure.
    bool open(const std::string& path);
    void close();

    // --- nodes / channels (per device) -----------------------------------
    void upsert_node(const std::string& device, const Node& n);
    void upsert_channel(const std::string& device, const Channel& c);
    void load_nodes(const std::string& device, NodeDb& db);
    void load_channels(const std::string& device, NodeDb& db);

    // --- location history ------------------------------------------------
    bool insert_location(const std::string& device, uint32_t node_num, double lat, double lon, int altitude, uint64_t ts);
    uint64_t max_location_ts();

    struct LocationRow {
        std::string device;
        uint32_t node_num;
        double latitude;
        double longitude;
        int altitude;
        uint64_t ts;
    };
    std::vector<LocationRow> get_locations_after(uint64_t ts, int limit = 100);
    std::vector<LocationRow> get_node_locations(uint32_t node_num, uint64_t since_ts = 0, int limit = 500);
    std::vector<LocationRow> get_recent_node_locations(uint64_t since_ts = 0, int limit = 1000);

    // --- telemetry history ------------------------------------------------
    struct TelemetryRow {
        int64_t rowid = 0;
        std::string device;
        uint32_t node_num = 0;
        uint64_t ts = 0;
        std::optional<uint8_t> battery_level;
        std::optional<float> voltage;
        std::optional<float> channel_util;
        std::optional<float> air_util_tx;
        std::optional<uint32_t> uptime_seconds;
        std::optional<float> temperature;
        std::optional<float> relative_humidity;
        std::optional<float> barometric_pressure;
        std::optional<float> gas_resistance;
        std::optional<uint32_t> iaq;
        std::optional<uint32_t> pm25;
        std::optional<uint32_t> co2;
        std::optional<float> current;
        std::optional<float> snr;
        std::optional<uint32_t> hops_away;
    };
    bool insert_telemetry(const TelemetryRow& row);
    uint64_t max_telemetry_ts();
    std::vector<TelemetryRow> get_telemetry_after_ts(uint64_t ts, int limit = 100);
    std::vector<TelemetryRow> get_node_telemetry(uint32_t node_num, uint64_t since_ts = 0, uint64_t before_ts = 0, int limit = 500);
    std::vector<TelemetryRow> get_recent_telemetry(uint64_t since_ts = 0, int limit = 500);

    // --- offline history loading -----------------------------------------
    std::vector<std::string> get_all_devices();
    std::vector<WindowKey> get_all_windows(const std::string& device);
    [[nodiscard]] std::optional<Node> get_node_any_device(uint32_t node_num);

    // --- device configuration persistence --------------------------------
    struct DeviceConfigItem {
        std::string section;
        std::string key;
        std::string value;
    };
    void upsert_device_config(const std::string& device, const std::string& section, const std::string& key, const std::string& value);
    std::vector<DeviceConfigItem> load_device_config(const std::string& device);

    // --- messages ---------------------------------------------------------
    int64_t insert_message(const StoredMessage& m);
    void update_ack_state(int64_t rowid, const std::string& ack_state);
    std::vector<StoredMessage> recent_messages(const WindowKey& w, int limit = 200);
    std::vector<StoredMessage> get_messages_paginated(const WindowKey& w, int limit, int offset = 0);
    int64_t max_message_rowid();
    uint64_t max_message_ts();
    std::vector<StoredMessage> get_messages_after(int64_t rowid, int limit = 100);
    std::vector<StoredMessage> get_messages_after_ts(uint64_t ts, int limit = 100);

    // Find a stored message by its ToRadio packet_id (for ACK routing).
    [[nodiscard]] std::optional<StoredMessage> find_by_packet_id(uint32_t packet_id);

    // --- packet log & statistics ------------------------------------------
    struct PacketLogRow {
        int64_t rowid = 0;
        std::string device;
        uint32_t from_node = 0;
        uint32_t to_node = 0;
        std::string port_name;
        uint32_t channel_idx = 0;
        float rx_snr = 0.0f;
        int32_t rx_rssi = 0;
        uint32_t hop_limit = 0;
        uint32_t hop_start = 0;
        bool broadcast = false;
        std::string summary;
        uint64_t ts = 0;
    };
    bool insert_packet_log(const PacketLogRow& row);

    struct GlobalStats {
        uint64_t total_packets = 0;
        uint64_t active_nodes = 0;
        uint64_t msg_count = 0;
        uint64_t telemetry_count = 0;
        uint64_t position_count = 0;
        uint64_t ack_count = 0;
        uint64_t traceroute_count = 0;
        uint64_t nodeinfo_count = 0;
        uint64_t other_count = 0;
        uint64_t broadcast_count = 0;
        uint64_t unicast_count = 0;
        float avg_snr = 0.0f;

        std::map<std::string, uint64_t> port_distribution;
        std::map<uint32_t, uint64_t> channel_distribution;

        struct TimelineBucket {
            uint64_t ts = 0;
            uint64_t count = 0;
            uint64_t msg_count = 0;
            uint64_t telemetry_count = 0;
            uint64_t pos_count = 0;
            uint64_t other_count = 0;
        };
        std::vector<TimelineBucket> timeline;

        struct NodeStat {
            uint32_t node_num = 0;
            uint64_t packet_count = 0;
            uint64_t msg_count = 0;
            uint64_t telemetry_count = 0;
            uint64_t pos_count = 0;
            uint64_t ack_count = 0;
            float avg_snr = 0.0f;
            uint64_t last_seen = 0;
        };
        std::vector<NodeStat> per_node_stats;
    };

    GlobalStats get_stats(uint64_t since_ts = 0, uint32_t for_node = 0);

    // --- misc -------------------------------------------------------------
    [[nodiscard]] bool ok() const { return db_ != nullptr; }
    // Periodic WAL checkpoint (keep the WAL file size bounded).
    void checkpoint();

private:
    sqlite3* db_ = nullptr;
    bool exec(const std::string& sql);
    // Insert counter for triggering periodic checkpoints.
    mutable int write_count_ = 0;
    void maybe_checkpoint();
};

} // namespace meshcli
