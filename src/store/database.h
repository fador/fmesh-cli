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
    void insert_location(const std::string& device, uint32_t node_num, double lat, double lon, int altitude, uint64_t ts);
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

    // --- offline history loading -----------------------------------------
    std::vector<std::string> get_all_devices();
    std::vector<WindowKey> get_all_windows(const std::string& device);

    // --- messages ---------------------------------------------------------
    int64_t insert_message(const StoredMessage& m);
    void update_ack_state(int64_t rowid, const std::string& ack_state);
    std::vector<StoredMessage> recent_messages(const WindowKey& w, int limit = 200);
    int64_t max_message_rowid();
    uint64_t max_message_ts();
    std::vector<StoredMessage> get_messages_after(int64_t rowid, int limit = 100);
    std::vector<StoredMessage> get_messages_after_ts(uint64_t ts, int limit = 100);

    // Find a stored message by its ToRadio packet_id (for ACK routing).
    [[nodiscard]] std::optional<StoredMessage> find_by_packet_id(uint32_t packet_id);

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
