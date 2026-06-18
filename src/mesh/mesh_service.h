#pragma once

#include "ble/ble_client.h"
#include "mesh/event.h"
#include "mesh/node_db.h"
#include "store/database.h"
#include "stream/stream_client.h"
#include "stream/stream_server.h"
#include "util/event_loop.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace meshcli {

struct DeviceRuntime {
    BleDeviceSpec spec;             // stored for reconnection
    std::unique_ptr<BleClient> client;       // BLE transport
    std::unique_ptr<class StreamClient> stream; // TCP/serial transport
    std::unique_ptr<NodeDb> db;
    std::string display_name;
    uint32_t my_node_num = 0;
    std::string my_long_name;
    std::string my_short_name;
    std::string firmware_version;
    std::string hw_model;
    bool config_complete = false;
    std::vector<std::string> config_lines;   // from /config decoding
    std::vector<EvRawPacket> raw_packets;    // last N raw FromRadio packets
    static constexpr size_t kMaxRawPackets = 200;
    // pending outbound messages awaiting ack, keyed by packet_id -> db rowid
    std::map<uint32_t, int64_t> pending_acks;
};

// Owns the BLE connections to one or more Meshtastic devices, runs the
// protocol handshake on each, and surfaces high-level MeshEvents to the UI
// via a thread-safe queue + eventfd. All public methods are safe to call
// from the UI thread.
class MeshService {
public:
    MeshService();
    ~MeshService();

    // Wire up the event sink (the TUI sets this so it gets woken on events).
    void set_event_sink(ConcurrentQueue<MeshEvent>* q, EventFd* wake);

    // Open the persistence database. Must be called before connect().
    bool open_database(const std::string& path);

    // Connect to a device. If `pair` is true, register a pairing agent.
    // Returns the assigned DeviceId (empty on failure). Safe to call multiple
    // times for different devices.
    std::string connect_device(const BleDeviceSpec& spec, bool pair);

    // Disconnect a single device. Returns false if the device id is unknown.
    bool disconnect_device(const std::string& device_id);

    // Disconnect everything.
    void disconnect_all();

    // Start the mesh stream server for incoming mesh clients
    void start_stream_server(int port, const std::string& user, const std::string& password);

    // Stop the mesh stream server
    void stop_stream_server();

    // Reconnect a device (e.g. after BLE disconnect). Returns false if the
    // device id is unknown.
    bool reconnect_device(const std::string& device_id);

    // Send a text message. `to_node` == kBroadcastNodeNum for a channel
    // broadcast. Returns the packet_id used (0 on failure).
    uint32_t send_text(const std::string& device_id,
                       uint32_t to_node,
                       uint32_t channel_idx,
                       const std::string& text,
                       bool want_ack);

    // --- queries for the UI ---
    [[nodiscard]] std::vector<std::string> device_ids() const;
    [[nodiscard]] const NodeDb* db_for(const std::string& device_id) const;
    [[nodiscard]] std::string firmware_for(const std::string& device_id) const;
    [[nodiscard]] std::string hw_model_for(const std::string& device_id) const;
    [[nodiscard]] std::string display_name_for(const std::string& device_id) const;
    [[nodiscard]] std::vector<std::string> config_lines_for(const std::string& device_id) const;
    [[nodiscard]] std::vector<EvRawPacket> raw_packets_for(const std::string& device_id) const;
    [[nodiscard]] Database& database() { return db_; }
    [[nodiscard]] bool has_devices() const { return !devices_.empty(); }

    // --- test support (tests can inject fake DeviceRuntimes) -----------
    [[nodiscard]] std::map<std::string, std::shared_ptr<DeviceRuntime>>& devices_for_test() { return devices_; }
    [[nodiscard]] std::mutex& devices_mu_for_test() { return devices_mu_; }

private:
    void handle_event(const std::shared_ptr<DeviceRuntime>& rt, const MeshEvent& ev);
    void dispatch_to_ui(MeshEvent ev);
    uint32_t next_packet_id();

    // packet id generator (mirrors the meshtastic python algorithm)
    std::mutex pid_mu_;
    uint32_t packet_id_ = 0;
    std::mt19937 rng_{std::random_device{}()};

    ConcurrentQueue<MeshEvent>* ui_queue_ = nullptr;
    EventFd* ui_wake_ = nullptr;

    Database db_;
    mutable std::mutex devices_mu_;
    std::map<std::string, std::shared_ptr<DeviceRuntime>> devices_;

#ifdef ENABLE_MESH_NET
    std::unique_ptr<StreamServer> stream_server_;
#endif

    // Dedup: track recently seen {from_node, packet_id} pairs across
    // all devices so the same mesh message only appears once in the UI.
    static constexpr size_t kDedupMax = 500;
    std::deque<uint64_t> seen_messages_;  // (from_node << 32) | packet_id
    bool is_duplicate(uint32_t from_node, uint32_t packet_id);
};

} // namespace meshcli
