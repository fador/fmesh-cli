#pragma once

#include "event.h"
#include "node_db.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace meshcli {

// Pure translation layer between the meshtastic protobuf wire format and the
// high-level MeshEvent / Node / Channel types used by the rest of the app.
// Stateless; safe to call from any thread.
class MeshCodec {
public:
    // --- encoders (client -> radio) --------------------------------------

    // Build a ToRadio{want_config_id=N}. Sent as the first thing on connect.
    static std::string encode_want_config(uint32_t config_id);

    // Build a ToRadio{disconnect=true}. Sent on graceful close.
    static std::string encode_disconnect();

    // Build a ToRadio{packet=MeshPacket{...}} carrying a text message.
    //   to_node     : kBroadcastNodeNum for a channel broadcast, else a DM
    //   channel_idx : channel to send on (matters for both broadcast & DM)
    //   pki_pubkey  : if non-empty, mark pki_encrypted and attach the key
    //                 (used for DMs to PKI-enabled nodes on recent firmware)
    static std::string encode_text_packet(
        uint32_t packet_id,
        uint32_t to_node,
        uint32_t channel_idx,
        const std::string& text,
        bool want_ack,
        uint32_t hop_limit,
        const std::vector<uint8_t>& pki_pubkey);

    // --- decoders (radio -> client) --------------------------------------

    // Parse a FromRadio message. Returns nullopt for records we don't surface
    // to the UI (e.g. queueStatus, xmodem). On config_complete_id the returned
    // event is EvConfigComplete and `config_id` is set to the id the device
    // echoed back so the caller can match it to its request.
    static std::optional<MeshEvent> decode_from_radio(
        const std::string& bytes,
        const DeviceId& device,
        uint32_t& out_config_id);
};

} // namespace meshcli
