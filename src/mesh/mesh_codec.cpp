#include "mesh_codec.h"

#include "util/log.h"

#include <meshtastic/channel.pb.h>
#include <meshtastic/config.pb.h>
#include <meshtastic/mesh.pb.h>
#include <meshtastic/module_config.pb.h>
#include <meshtastic/portnums.pb.h>
#include <meshtastic/telemetry.pb.h>

#include <google/protobuf/util/json_util.h>

namespace meshcli {

using meshtastic::FromRadio;
using meshtastic::ToRadio;
using meshtastic::MeshPacket;
using meshtastic::Data;
using meshtastic::PortNum;

// ---------------------------------------------------------------------------
// encoders
// ---------------------------------------------------------------------------

std::string MeshCodec::encode_want_config(uint32_t config_id) {
    ToRadio tr;
    tr.set_want_config_id(config_id);
    return tr.SerializeAsString();
}

std::string MeshCodec::encode_disconnect() {
    ToRadio tr;
    tr.set_disconnect(true);
    return tr.SerializeAsString();
}

std::string MeshCodec::encode_text_packet(
    uint32_t packet_id,
    uint32_t to_node,
    uint32_t channel_idx,
    const std::string& text,
    bool want_ack,
    uint32_t hop_limit,
    const std::vector<uint8_t>& pki_pubkey) {

    ToRadio tr;
    auto* pkt = tr.mutable_packet();
    pkt->set_id(packet_id);
    pkt->set_to(to_node);
    pkt->set_channel(channel_idx);
    pkt->set_want_ack(want_ack);
    if (hop_limit > 0) pkt->set_hop_limit(hop_limit);

    auto* data = pkt->mutable_decoded();
    data->set_portnum(PortNum::TEXT_MESSAGE_APP);
    data->set_payload(text);

    if (!pki_pubkey.empty()) {
        pkt->set_pki_encrypted(true);
        pkt->set_public_key(std::string(pki_pubkey.begin(), pki_pubkey.end()));
    }

    pkt->set_priority(meshtastic::MeshPacket_Priority_RELIABLE);

    return tr.SerializeAsString();
}

// ---------------------------------------------------------------------------
// helpers for decoding
// ---------------------------------------------------------------------------

namespace {

Node node_from_node_info(const meshtastic::NodeInfo& ni) {
    Node n;
    n.node_num = ni.num();
    n.node_id = node_num_to_id(ni.num());
    if (ni.has_user()) {
        const auto& u = ni.user();
        n.long_name = u.long_name();
        n.short_name = u.short_name();
        n.hw_model = meshtastic::HardwareModel_Name(u.hw_model());
        n.role = meshtastic::Config_DeviceConfig_Role_Name(u.role());
        n.has_public_key = !u.public_key().empty();
        if (n.has_public_key) {
            n.public_key.assign(u.public_key().begin(), u.public_key().end());
        }
    }
    n.is_favorite = ni.is_favorite();
    n.is_ignored = ni.is_ignored();
    n.is_muted = ni.is_muted();
    n.is_key_verified = ni.is_key_manually_verified();
    if (ni.has_position()) {
        const auto& p = ni.position();
        if (p.latitude_i() != 0) n.latitude = p.latitude_i() * 1e-7;
        if (p.longitude_i() != 0) n.longitude = p.longitude_i() * 1e-7;
        if (p.altitude() != 0) n.altitude = p.altitude();
    }
    if (ni.has_device_metrics()) {
        const auto& m = ni.device_metrics();
        n.battery_level = static_cast<uint8_t>(m.battery_level());
        n.voltage = m.voltage();
        n.channel_util = m.channel_utilization();
        n.air_util_tx = m.air_util_tx();
    }
    if (ni.snr() != 0.0f) n.snr = ni.snr();
    if (ni.last_heard() != 0) n.last_heard = ni.last_heard();
    if (ni.has_hops_away()) n.hops_away = ni.hops_away();
    return n;
}

Channel channel_from_proto(const meshtastic::Channel& c) {
    Channel ch;
    ch.index = c.index();
    ch.name = c.settings().name();
    ch.has_psk = !c.settings().psk().empty();
    switch (c.role()) {
        case meshtastic::Channel_Role_PRIMARY:   ch.role = "PRIMARY"; break;
        case meshtastic::Channel_Role_SECONDARY: ch.role = "SECONDARY"; break;
        case meshtastic::Channel_Role_DISABLED:  ch.role = "DISABLED"; break;
        default: ch.role = "UNKNOWN";
    }
    return ch;
}

std::optional<MeshEvent> decode_packet(
    const DeviceId& device, const MeshPacket& pkt) {
    // We only surface decoded (i.e. decrypted-by-radio) packets to the UI.
    if (!pkt.has_decoded()) return std::nullopt;

    const auto& d = pkt.decoded();

    if (d.portnum() == PortNum::TEXT_MESSAGE_APP) {
        EvTextReceived ev;
        ev.device = device;
        ev.from_node = pkt.from();
        ev.to_node = pkt.to();
        ev.channel_idx = pkt.channel();
        ev.packet_id = pkt.id();
        ev.rx_time = pkt.rx_time();
        ev.rx_snr = pkt.rx_snr();
        ev.broadcast = (pkt.to() == kBroadcastNodeNum);
        ev.want_ack = pkt.want_ack();
        ev.text = d.payload();
        return ev;
    }
    if (d.portnum() == PortNum::ROUTING_APP) {
        // ACK / NAK for a previously sent packet.
        meshtastic::Routing r;
        if (!r.ParseFromString(d.payload())) return std::nullopt;
        EvAckReceived ev;
        ev.device = device;
        ev.packet_id = d.request_id() ? d.request_id() : pkt.id();
        ev.from_node = pkt.from();
        ev.success = (r.error_reason() == meshtastic::Routing_Error_NONE);
        ev.error_reason = meshtastic::Routing_Error_Name(r.error_reason());
        return ev;
    }
    // Other portnums (telemetry, position, nodeinfo) arriving as packets
    // would normally be processed into the node DB; the firmware typically
    // sends them as FromRadio.node_info rather than FromRadio.packet, so we
    // ignore stray packets here for v1.
    return std::nullopt;
}

} // namespace

std::optional<MeshEvent> MeshCodec::decode_from_radio(
    const std::string& bytes, const DeviceId& device, uint32_t& out_config_id) {
    FromRadio fr;
    if (!fr.ParseFromString(bytes)) {
        LOG_WARN() << "failed to parse FromRadio (" << bytes.size() << " bytes)";
        return std::nullopt;
    }

    switch (fr.payload_variant_case()) {
        case FromRadio::kMyInfo: {
            EvMyInfo ev;
            ev.device = device;
            ev.my_node_num = fr.my_info().my_node_num();
            return ev;
        }
        case FromRadio::kMetadata: {
            EvMetadata ev;
            ev.device = device;
            ev.firmware_version = fr.metadata().firmware_version();
            ev.hw_model = meshtastic::HardwareModel_Name(fr.metadata().hw_model());
            return ev;
        }
        case FromRadio::kNodeInfo: {
            EvNodeUpdated ev;
            ev.device = device;
            ev.node = node_from_node_info(fr.node_info());
            return ev;
        }
        case FromRadio::kChannel: {
            EvChannelUpdated ev;
            ev.device = device;
            ev.channel = channel_from_proto(fr.channel());
            return ev;
        }
        case FromRadio::kConfig:
        case FromRadio::kModuleConfig:
            // Local config / module config; not surfaced to UI for v1.
            return std::nullopt;
        case FromRadio::kPacket:
            return decode_packet(device, fr.packet());
        case FromRadio::kLogRecord: {
            EvLogLine ev;
            ev.device = device;
            const auto& lr = fr.log_record();
            if (!lr.source().empty()) ev.source = lr.source();
            ev.message = lr.message();
            return ev;
        }
        case FromRadio::kConfigCompleteId: {
            out_config_id = fr.config_complete_id();
            EvConfigComplete ev;
            ev.device = device;
            ev.rebooted = fr.rebooted();
            return ev;
        }
        case FromRadio::kQueueStatus:
        case FromRadio::kXmodemPacket:
        case FromRadio::kMqttClientProxyMessage:
        case FromRadio::kFileInfo:
        case FromRadio::kClientNotification:
        case FromRadio::kDeviceuiConfig:
        case FromRadio::kLockdownStatus:
            // Future work.
            return std::nullopt;
        case FromRadio::kRebooted:
            // Bare `rebooted` flag with no config_complete; treat as info.
            return std::nullopt;
        default:
            return std::nullopt;
    }
}

} // namespace meshcli
