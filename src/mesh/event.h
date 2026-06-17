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
    bool broadcast = false;
    bool want_ack = false;
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

struct EvNodeJoined {
    DeviceId device;
    Node node;   // a brand-new node we hadn't seen before
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
    EvNodeJoined
>;

} // namespace meshcli
