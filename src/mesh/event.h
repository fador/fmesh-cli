#pragma once

#include "node_db.h"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace meshcli {

// High-level events delivered from the mesh/ble layer up to the UI.
// All events carry the DeviceId of the radio they originated from so the
// UI can route them when multiple devices are connected.

struct EvConnected {
    DeviceId device;
    std::string display_name;   // BLE name or host
};

struct EvDisconnected {
    DeviceId device;
    std::string reason;
};

// my_node_num from FromRadio.my_info; arrives early in the handshake.
struct EvMyInfo {
    DeviceId device;
    uint32_t my_node_num = 0;
};

// firmware/hw from FromRadio.metadata.
struct EvMetadata {
    DeviceId device;
    std::string firmware_version;
    std::string hw_model;
};

struct EvConfigComplete {
    DeviceId device;
    bool rebooted = false;
};

struct EvNodeUpdated {
    DeviceId device;
    Node node;
    std::string old_short_name;
    std::string old_long_name;
    bool is_new = false;
};

struct EvChannelUpdated {
    DeviceId device;
    Channel channel;
};

struct EvTextReceived {
    DeviceId device;
    uint32_t from_node = 0;
    uint32_t to_node = 0;
    uint32_t channel_idx = 0;
    uint32_t packet_id = 0;
    uint32_t rx_time = 0;
    float rx_snr = 0.0f;
    int32_t rx_rssi = 0;
    uint32_t hop_start = 0;
    uint32_t hop_limit = 0;
    bool broadcast = false;
    bool want_ack = false;
    bool duplicate = false;
    std::string text;
};

struct EvAckReceived {
    DeviceId device;
    uint32_t packet_id = 0;
    uint32_t from_node = 0;
    bool success = false;
    std::string error_reason;
};

struct EvLogLine {
    DeviceId device;
    std::string source;
    std::string message;
};

struct EvError {
    DeviceId device;
    std::string message;
};

// A single config line: "section.key = value"
struct EvConfigLine {
    DeviceId device;
    std::string line;
};

// A raw FromRadio packet (hex dump + decoded summary)
struct EvRawPacket {
    DeviceId device;
    std::string hex;       // hex dump of the raw bytes
    std::string summary;   // short description (e.g. "TEXT_MESSAGE_APP")
    uint64_t ts = 0;       // unix millis
};

// A raw byte stream chunk received from the radio (before framing/decoding)
struct EvRawRxBytes {
    DeviceId device;
    std::string bytes;
};

// Request to send raw bytes to the local physical radio (from StreamServer clients)
struct EvSendRawToRadio {
    std::string bytes;
};

// A BLE device found during scan (for the interactive connection wizard).
struct EvBleDeviceFound {
    DeviceId device;       // BlueZ object path
    std::string name;
    std::string address;
    int16_t rssi = 0;
    bool scan_complete = false;  // true on the final sentinel event
};

// A DB sync protocol payload received over TCP (0xD0 framing)
struct EvDbSyncPayload {
    DeviceId device;
    std::string payload; // JSON payload
};

using MeshEvent = std::variant<
    EvConnected,
    EvDisconnected,
    EvMyInfo,
    EvMetadata,
    EvConfigComplete,
    EvNodeUpdated,
    EvChannelUpdated,
    EvTextReceived,
    EvAckReceived,
    EvLogLine,
    EvError,
    EvConfigLine,
    EvRawPacket,
    EvRawRxBytes,
    EvSendRawToRadio,
    EvBleDeviceFound,
    EvDbSyncPayload
>;

} // namespace meshcli
