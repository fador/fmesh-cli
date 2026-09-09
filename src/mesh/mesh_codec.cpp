#include "mesh_codec.h"

#include "util/log.h"

#include <meshtastic/channel.pb.h>
#include <meshtastic/config.pb.h>
#include <meshtastic/mesh.pb.h>
#include <meshtastic/module_config.pb.h>
#include <meshtastic/portnums.pb.h>
#include <meshtastic/telemetry.pb.h>

#include <meshtastic/admin.pb.h>

#include <google/protobuf/util/json_util.h>
#include <google/protobuf/message.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/reflection.h>

#include <mutex>
#include <random>

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

bool MeshCodec::set_config_value(
    const std::string& config_bytes,
    const std::string& module_config_bytes,
    const std::string& key,
    const std::string& value,
    bool& out_is_module,
    std::string& out_modified_bytes)
{
    // key is like "lora.tx_power" or "display.screen_on_secs"
    auto dot = key.find('.');
    if (dot == std::string::npos) return false;
    
    std::string module_name = key.substr(0, dot);
    std::string field_name = key.substr(dot + 1);

    // Support common aliases
    if (module_name == "ext_notif") module_name = "external_notification";
    if (module_name == "canned_messages") module_name = "canned_message";
    
    meshtastic::Config config;
    meshtastic::ModuleConfig module_config;
    
    google::protobuf::Message* msg = nullptr;
    
    if (!config_bytes.empty()) config.ParseFromString(config_bytes);
    if (!module_config_bytes.empty()) module_config.ParseFromString(module_config_bytes);
    
    // Check if it's in Config
    const google::protobuf::Descriptor* config_desc = config.GetDescriptor();
    const google::protobuf::Reflection* config_refl = config.GetReflection();
    const google::protobuf::FieldDescriptor* config_field = config_desc->FindFieldByName(module_name);
    
    if (config_field && config_field->type() == google::protobuf::FieldDescriptor::TYPE_MESSAGE) {
        msg = config_refl->MutableMessage(&config, config_field);
        out_is_module = false;
    } else {
        // Check if it's in ModuleConfig
        const google::protobuf::Descriptor* mod_desc = module_config.GetDescriptor();
        const google::protobuf::Reflection* mod_refl = module_config.GetReflection();
        const google::protobuf::FieldDescriptor* mod_field = mod_desc->FindFieldByName(module_name);
        if (mod_field && mod_field->type() == google::protobuf::FieldDescriptor::TYPE_MESSAGE) {
            msg = mod_refl->MutableMessage(&module_config, mod_field);
            out_is_module = true;
        } else {
            return false; // Unknown module
        }
    }
    
    const google::protobuf::Descriptor* desc = msg->GetDescriptor();
    const google::protobuf::Reflection* refl = msg->GetReflection();
    const google::protobuf::FieldDescriptor* field = desc->FindFieldByName(field_name);
    if (!field) return false;
    
    try {
        switch (field->cpp_type()) {
            case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
                refl->SetInt32(msg, field, std::stoi(value));
                break;
            case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
                refl->SetUInt32(msg, field, std::stoul(value));
                break;
            case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
                refl->SetInt64(msg, field, std::stoll(value));
                break;
            case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
                refl->SetUInt64(msg, field, std::stoull(value));
                break;
            case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
                refl->SetFloat(msg, field, std::stof(value));
                break;
            case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
                refl->SetDouble(msg, field, std::stod(value));
                break;
            case google::protobuf::FieldDescriptor::CPPTYPE_BOOL: {
                std::string v = value;
                std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c){ return std::tolower(c); });
                refl->SetBool(msg, field, (v == "true" || v == "1" || v == "on" || v == "yes"));
                break;
            }
            case google::protobuf::FieldDescriptor::CPPTYPE_STRING:
                refl->SetString(msg, field, value);
                break;
            case google::protobuf::FieldDescriptor::CPPTYPE_ENUM: {
                const google::protobuf::EnumDescriptor* enum_desc = field->enum_type();
                const google::protobuf::EnumValueDescriptor* enum_val = enum_desc->FindValueByName(value);
                if (!enum_val) {
                    std::string upper = value;
                    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c){ return std::toupper(c); });
                    enum_val = enum_desc->FindValueByName(upper);
                }
                if (!enum_val) {
                    for (int i = 0; i < enum_desc->value_count(); ++i) {
                        const auto* ev = enum_desc->value(i);
                        std::string ev_name = ev->name();
                        std::string upper = value;
                        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c){ return std::toupper(c); });
                        if (ev_name == upper || ev_name.find(upper) != std::string::npos) {
                            enum_val = ev;
                            break;
                        }
                    }
                }
                if (!enum_val) {
                    try {
                        int int_val = std::stoi(value);
                        enum_val = enum_desc->FindValueByNumber(int_val);
                    } catch (...) {}
                }
                if (!enum_val) return false;
                refl->SetEnum(msg, field, enum_val);
                break;
            }
            default:
                return false;
        }
    } catch (...) {
        return false;
    }
    
    if (out_is_module) {
        meshtastic::ModuleConfig single_mod;
        const auto* target_f = single_mod.GetDescriptor()->FindFieldByName(module_name);
        if (target_f) {
            single_mod.GetReflection()->MutableMessage(&single_mod, target_f)->CopyFrom(*msg);
            out_modified_bytes = single_mod.SerializeAsString();
        } else {
            out_modified_bytes = module_config.SerializeAsString();
        }
    } else {
        meshtastic::Config single_cfg;
        const auto* target_f = single_cfg.GetDescriptor()->FindFieldByName(module_name);
        if (target_f) {
            single_cfg.GetReflection()->MutableMessage(&single_cfg, target_f)->CopyFrom(*msg);
            out_modified_bytes = single_cfg.SerializeAsString();
        } else {
            out_modified_bytes = config.SerializeAsString();
        }
    }
    
    return true;
}

std::string MeshCodec::encode_admin_packet(
    uint32_t from_node, uint32_t to_node, const std::string& modified_bytes, bool is_module, uint32_t packet_id)
{
    meshtastic::AdminMessage admin;
    if (is_module) {
        admin.mutable_set_module_config()->ParseFromString(modified_bytes);
    } else {
        admin.mutable_set_config()->ParseFromString(modified_bytes);
    }
    
    meshtastic::MeshPacket pkt;
    if (packet_id == 0) {
        static std::mt19937 g_rng{std::random_device{}()};
        static std::mutex g_rng_mu;
        std::lock_guard<std::mutex> lk(g_rng_mu);
        packet_id = g_rng();
        if (packet_id == 0) packet_id = 1;
    }
    pkt.set_id(packet_id);
    pkt.set_from(from_node);
    pkt.set_to(to_node);
    pkt.set_want_ack(true);
    pkt.mutable_decoded()->set_payload(admin.SerializeAsString());
    pkt.mutable_decoded()->set_portnum(meshtastic::PortNum::ADMIN_APP);
    
    meshtastic::ToRadio tr;
    *tr.mutable_packet() = pkt;
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

std::string MeshCodec::encode_traceroute_packet(
    uint32_t packet_id,
    uint32_t from_node,
    uint32_t to_node,
    uint32_t channel_idx,
    uint32_t hop_limit) {

    meshtastic::RouteDiscovery rd;
    ToRadio tr;
    auto* pkt = tr.mutable_packet();
    pkt->set_id(packet_id);
    pkt->set_from(from_node);
    pkt->set_to(to_node);
    pkt->set_channel(channel_idx);
    pkt->set_want_ack(true);
    if (hop_limit > 0) pkt->set_hop_limit(hop_limit);
    pkt->set_priority(meshtastic::MeshPacket_Priority_RELIABLE);

    auto* data = pkt->mutable_decoded();
    data->set_portnum(PortNum::TRACEROUTE_APP);
    data->set_want_response(true);
    data->set_payload(rd.SerializeAsString());

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

    if (d.portnum() == PortNum::TEXT_MESSAGE_APP ||
        d.portnum() == PortNum::TEXT_MESSAGE_COMPRESSED_APP ||
        d.portnum() == PortNum::ALERT_APP ||
        d.portnum() == PortNum::DETECTION_SENSOR_APP) {
        EvTextReceived ev;
        ev.device = device;
        ev.from_node = pkt.from();
        ev.to_node = pkt.to();
        ev.channel_idx = pkt.channel();
        ev.packet_id = pkt.id();
        ev.rx_time = pkt.rx_time();
        ev.rx_snr = pkt.rx_snr();
        ev.rx_rssi = pkt.rx_rssi();
        ev.hop_start = pkt.hop_start();
        ev.hop_limit = pkt.hop_limit();
        ev.relay_node = pkt.relay_node();
        ev.broadcast = (pkt.to() == kBroadcastNodeNum || pkt.to() == 0);
        ev.want_ack = pkt.want_ack();
        if (d.portnum() == PortNum::ALERT_APP) {
            ev.text = "[ALERT] " + d.payload();
        } else if (d.portnum() == PortNum::DETECTION_SENSOR_APP) {
            ev.text = "[SENSOR] " + d.payload();
        } else {
            ev.text = d.payload();
        }
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
    if (d.portnum() == PortNum::POSITION_APP) {
        meshtastic::Position p;
        if (!p.ParseFromString(d.payload())) return std::nullopt;
        EvPositionReceived ev;
        ev.device = device;
        ev.from_node = pkt.from();
        // The firmware reports lat/lon as scaled integers (* 1e7)
        if (p.latitude_i() != 0) ev.latitude = p.latitude_i() / 1e7;
        if (p.longitude_i() != 0) ev.longitude = p.longitude_i() / 1e7;
        ev.altitude = p.altitude();
        ev.rx_time = pkt.rx_time();
        return ev;
    }
    if (d.portnum() == PortNum::TRACEROUTE_APP) {
        meshtastic::RouteDiscovery rd;
        if (!rd.ParseFromString(d.payload())) return std::nullopt;
        EvTracerouteReceived ev;
        ev.device = device;
        ev.from_node = pkt.from();
        ev.to_node = pkt.to();
        for (auto n : rd.route()) ev.route.push_back(n);
        for (auto s : rd.snr_towards()) ev.snr_towards.push_back(static_cast<float>(s) / 4.0f);
        for (auto n : rd.route_back()) ev.route_back.push_back(n);
        for (auto s : rd.snr_back()) ev.snr_back.push_back(static_cast<float>(s) / 4.0f);
        return ev;
    }
    if (d.portnum() == PortNum::TELEMETRY_APP) {
        meshtastic::Telemetry t;
        if (!t.ParseFromString(d.payload())) return std::nullopt;
        EvNodeUpdated ev;
        ev.device = device;
        ev.node.node_num = pkt.from();
        ev.node.node_id = node_num_to_id(pkt.from());
        if (pkt.rx_time() != 0) ev.node.last_heard = pkt.rx_time();
        else if (t.time() != 0) ev.node.last_heard = t.time();
        if (pkt.rx_snr() != 0.0f) ev.node.snr = pkt.rx_snr();
        if (pkt.hop_start() > 0 && pkt.hop_start() >= pkt.hop_limit()) {
            ev.node.hops_away = pkt.hop_start() - pkt.hop_limit();
        }
        if (t.has_device_metrics()) {
            const auto& m = t.device_metrics();
            if (m.has_battery_level()) ev.node.battery_level = static_cast<uint8_t>(m.battery_level());
            else if (m.battery_level() != 0) ev.node.battery_level = static_cast<uint8_t>(m.battery_level());
            if (m.has_voltage()) ev.node.voltage = m.voltage();
            else if (m.voltage() != 0.0f) ev.node.voltage = m.voltage();
            if (m.has_channel_utilization()) ev.node.channel_util = m.channel_utilization();
            else if (m.channel_utilization() != 0.0f) ev.node.channel_util = m.channel_utilization();
            if (m.has_air_util_tx()) ev.node.air_util_tx = m.air_util_tx();
            else if (m.air_util_tx() != 0.0f) ev.node.air_util_tx = m.air_util_tx();
            if (m.has_uptime_seconds()) ev.node.uptime_seconds = m.uptime_seconds();
            else if (m.uptime_seconds() != 0) ev.node.uptime_seconds = m.uptime_seconds();
        }
        if (t.has_environment_metrics()) {
            const auto& em = t.environment_metrics();
            if (em.has_temperature()) ev.node.temperature = em.temperature();
            else if (em.temperature() != 0.0f) ev.node.temperature = em.temperature();
            if (em.has_relative_humidity()) ev.node.relative_humidity = em.relative_humidity();
            else if (em.relative_humidity() != 0.0f) ev.node.relative_humidity = em.relative_humidity();
            if (em.has_barometric_pressure()) ev.node.barometric_pressure = em.barometric_pressure();
            else if (em.barometric_pressure() != 0.0f) ev.node.barometric_pressure = em.barometric_pressure();
            if (em.has_gas_resistance()) ev.node.gas_resistance = em.gas_resistance();
            else if (em.gas_resistance() != 0.0f) ev.node.gas_resistance = em.gas_resistance();
            if (em.has_iaq()) ev.node.iaq = em.iaq();
            else if (em.iaq() != 0) ev.node.iaq = em.iaq();
            if (em.has_current()) ev.node.current = em.current();
            else if (em.current() != 0.0f) ev.node.current = em.current();
            if (em.has_voltage() && !ev.node.voltage.has_value()) ev.node.voltage = em.voltage();
            else if (em.voltage() != 0.0f && !ev.node.voltage.has_value()) ev.node.voltage = em.voltage();
        }
        if (t.has_air_quality_metrics()) {
            const auto& aq = t.air_quality_metrics();
            if (aq.pm25_standard() != 0) ev.node.pm25 = aq.pm25_standard();
            else if (aq.pm25_environmental() != 0) ev.node.pm25 = aq.pm25_environmental();
            if (aq.co2() != 0) ev.node.co2 = aq.co2();
        }
        if (t.has_power_metrics()) {
            const auto& pm = t.power_metrics();
            if (pm.ch1_current() != 0.0f) ev.node.current = pm.ch1_current();
            if (pm.ch1_voltage() != 0.0f && !ev.node.voltage.has_value()) ev.node.voltage = pm.ch1_voltage();
        }
        if (t.has_local_stats()) {
            const auto& ls = t.local_stats();
            if (ls.uptime_seconds() != 0 && !ev.node.uptime_seconds.has_value()) ev.node.uptime_seconds = ls.uptime_seconds();
            if (ls.channel_utilization() != 0.0f && !ev.node.channel_util.has_value()) ev.node.channel_util = ls.channel_utilization();
            if (ls.air_util_tx() != 0.0f && !ev.node.air_util_tx.has_value()) ev.node.air_util_tx = ls.air_util_tx();
        }
        return ev;
    }
    if (d.portnum() == PortNum::NEIGHBORINFO_APP) {
        meshtastic::NeighborInfo ni;
        if (!ni.ParseFromString(d.payload())) return std::nullopt;
        EvNeighborInfoReceived ev;
        ev.device = device;
        ev.node_num = ni.node_id() != 0 ? ni.node_id() : pkt.from();
        for (const auto& nb : ni.neighbors()) {
            NeighborInfoEntry entry;
            entry.node_id = nb.node_id();
            entry.rx_snr = nb.snr();
            entry.rx_time = nb.last_rx_time();
            ev.neighbors.push_back(entry);
        }
        return ev;
    }
    if (d.portnum() == PortNum::NODEINFO_APP) {
        meshtastic::User u;
        if (!u.ParseFromString(d.payload())) return std::nullopt;
        EvNodeUpdated ev;
        ev.device = device;
        ev.node.node_num = pkt.from();
        ev.node.node_id = u.id().empty() ? node_num_to_id(pkt.from()) : u.id();
        ev.node.long_name = u.long_name();
        ev.node.short_name = u.short_name();
        ev.node.hw_model = meshtastic::HardwareModel_Name(u.hw_model());
        ev.node.role = meshtastic::Config_DeviceConfig_Role_Name(u.role());
        ev.node.has_public_key = !u.public_key().empty();
        if (ev.node.has_public_key) {
            ev.node.public_key.assign(u.public_key().begin(), u.public_key().end());
        }
        if (pkt.rx_time() != 0) ev.node.last_heard = pkt.rx_time();
        if (pkt.rx_snr() != 0.0f) ev.node.snr = pkt.rx_snr();
        if (pkt.hop_start() > 0 && pkt.hop_start() >= pkt.hop_limit()) {
            ev.node.hops_away = pkt.hop_start() - pkt.hop_limit();
        }
        return ev;
    }
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
        case FromRadio::kConfig: {
            EvConfigBytes ev;
            ev.device = device;
            ev.is_module = false;
            ev.bytes = fr.config().SerializeAsString();
            return ev;
        }
        case FromRadio::kModuleConfig: {
            EvConfigBytes ev;
            ev.device = device;
            ev.is_module = true;
            ev.bytes = fr.moduleconfig().SerializeAsString();
            return ev;
        }
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

// ---- config decoder --------------------------------------------------

std::vector<std::string> MeshCodec::decode_config_lines(
    const std::string& bytes, bool is_module) {
    std::vector<std::string> out;
    auto add = [&](const std::string& s) { if (!s.empty()) out.push_back(s); };
    auto add_int  = [&](const char* key, int64_t v) { add(std::string(key) + " = " + std::to_string(v)); };
    auto add_bool = [&](const char* key, bool v) { add(std::string(key) + " = " + (v ? "ON" : "OFF")); };
    auto add_str  = [&](const char* key, const std::string& v) {
        if (!v.empty()) add(std::string(key) + " = " + v); };

    if (is_module) {
        meshtastic::ModuleConfig mc;
        if (!mc.ParseFromString(bytes)) return out;

        if (mc.has_mqtt()) {
            const auto& m = mc.mqtt();
            add("--- Module: MQTT ---");
            add_bool("mqtt.enabled", m.enabled());
            add_str("mqtt.address", m.address());
            add_str("mqtt.username", m.username());
            add_bool("mqtt.encryption_enabled", m.encryption_enabled());
            add_bool("mqtt.json_enabled", m.json_enabled());
            add_bool("mqtt.tls_enabled", m.tls_enabled());
            add_str("mqtt.root", m.root());
            add_bool("mqtt.proxy_to_client_enabled", m.proxy_to_client_enabled());
            add_bool("mqtt.map_reporting_enabled", m.map_reporting_enabled());
        }
        if (mc.has_serial()) {
            const auto& s = mc.serial();
            add("--- Module: Serial ---");
            add_bool("serial.enabled", s.enabled());
            add_bool("serial.echo", s.echo());
            add_int("serial.baud", static_cast<int64_t>(s.baud()));
            add_str("serial.mode", meshtastic::ModuleConfig_SerialConfig_Serial_Mode_Name(s.mode()));
        }
        if (mc.has_external_notification()) {
            const auto& n = mc.external_notification();
            add("--- Module: ExtNotification ---");
            add_bool("ext_notif.enabled", n.enabled());
            add_bool("ext_notif.alert_message", n.alert_message());
            add_bool("ext_notif.alert_message_buzzer", n.alert_message_buzzer());
        }
        if (mc.has_store_forward()) {
            add_bool("store_forward.enabled", mc.store_forward().enabled());
        }
        if (mc.has_range_test()) {
            add_bool("range_test.enabled", mc.range_test().enabled());
        }
        if (mc.has_telemetry()) {
            add("--- Module: Telemetry ---");
            add_int("telemetry.device_update_interval",
                    mc.telemetry().device_update_interval());
            add_int("telemetry.environment_update_interval",
                    mc.telemetry().environment_update_interval());
        }
        if (mc.has_canned_message()) {
            add_bool("canned_messages.enabled", mc.canned_message().enabled());
        }
    } else {
        meshtastic::Config cfg;
        if (!cfg.ParseFromString(bytes)) return out;

        if (cfg.has_device()) {
            const auto& d = cfg.device();
            add("--- Device ---");
            add_str("device.role", meshtastic::Config_DeviceConfig_Role_Name(d.role()));
            add_bool("device.serial_enabled", d.serial_enabled());
            add_int("device.node_info_broadcast_secs", d.node_info_broadcast_secs());
            add_bool("device.double_tap_as_button_press", d.double_tap_as_button_press());
            add_bool("device.is_managed", d.is_managed());
            add_str("device.rebroadcast_mode",
                    meshtastic::Config_DeviceConfig_RebroadcastMode_Name(d.rebroadcast_mode()));
        }
        if (cfg.has_position()) {
            const auto& p = cfg.position();
            add("--- Position ---");
            add_int("position.broadcast_secs", p.position_broadcast_secs());
            add_str("position.gps_mode",
                    meshtastic::Config_PositionConfig_GpsMode_Name(p.gps_mode()));
            add_bool("position.fixed_position", p.fixed_position());
            add_int("position.broadcast_smart_min_interval_secs",
                    p.broadcast_smart_minimum_interval_secs());
        }
        if (cfg.has_power()) {
            const auto& pw = cfg.power();
            add("--- Power ---");
            add_bool("power.is_power_saving", pw.is_power_saving());
            add_int("power.on_battery_shutdown_after_secs",
                    pw.on_battery_shutdown_after_secs());
            add_int("power.wait_bluetooth_secs", pw.wait_bluetooth_secs());
        }
        if (cfg.has_network()) {
            const auto& n = cfg.network();
            add("--- Network ---");
            add_bool("network.wifi_enabled", n.wifi_enabled());
            add_str("network.address_mode",
                    meshtastic::Config_NetworkConfig_AddressMode_Name(n.address_mode()));
            add_str("network.ntp_server", n.ntp_server());
        }
        if (cfg.has_display()) {
            const auto& d = cfg.display();
            add("--- Display ---");
            add_int("display.screen_on_secs", d.screen_on_secs());
            add_str("display.display_mode",
                    meshtastic::Config_DisplayConfig_DisplayMode_Name(d.displaymode()));
        }
        if (cfg.has_lora()) {
            const auto& l = cfg.lora();
            add("--- LoRa ---");
            add_bool("lora.use_preset", l.use_preset());
            add_str("lora.modem_preset", meshtastic::Config_LoRaConfig_ModemPreset_Name(l.modem_preset()));
            add_str("lora.region", meshtastic::Config_LoRaConfig_RegionCode_Name(l.region()));
            add_int("lora.tx_power", l.tx_power());
            add_int("lora.channel_num", l.channel_num());
            add_int("lora.frequency_offset", l.frequency_offset());
            add_int("lora.bandwidth", l.bandwidth());
            add_int("lora.spread_factor", l.spread_factor());
            add_int("lora.coding_rate", l.coding_rate());
            add_int("lora.hop_limit", l.hop_limit());
            add_bool("lora.tx_enabled", l.tx_enabled());
            add_bool("lora.override_duty_cycle", l.override_duty_cycle());
            add_bool("lora.sx126x_rx_boosted_gain", l.sx126x_rx_boosted_gain());
        }
        if (cfg.has_bluetooth()) {
            const auto& bt = cfg.bluetooth();
            add("--- Bluetooth ---");
            add_bool("bluetooth.enabled", bt.enabled());
            add_int("bluetooth.fixed_pin", static_cast<int64_t>(bt.fixed_pin()));
            add_str("bluetooth.mode",
                    meshtastic::Config_BluetoothConfig_PairingMode_Name(bt.mode()));
        }
        if (cfg.has_security()) {
            const auto& s = cfg.security();
            add("--- Security ---");
            add_bool("security.is_managed", s.is_managed());
            add_bool("security.serial_enabled", s.serial_enabled());
            add_bool("security.debug_log_api_enabled", s.debug_log_api_enabled());
            add_bool("security.admin_channel_enabled", s.admin_channel_enabled());
        }
    }
    return out;
}

std::string MeshCodec::hex_dump(const std::string& bytes) {
    if (bytes.empty()) return "(empty)";
    std::string out;
    out.reserve(bytes.size() * 3);
    for (size_t i = 0; i < bytes.size(); i += 16) {
        if (i) out += '\n';
        // Write hex bytes
        for (size_t j = 0; j < 16; ++j) {
            if (j == 8) out += ' ';
            if (i + j < bytes.size()) {
                char buf[4];
                std::snprintf(buf, sizeof(buf), "%02x ",
                              static_cast<unsigned char>(bytes[i + j]));
                out += buf;
            } else {
                out += "   ";
            }
        }
        out += " |";
        // Write ASCII
        for (size_t j = 0; j < 16 && i + j < bytes.size(); ++j) {
            unsigned char c = static_cast<unsigned char>(bytes[i + j]);
            out += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.';
        }
    }
    return out;
}

std::string MeshCodec::from_radio_summary(const std::string& bytes) {
    meshtastic::FromRadio fr;
    if (!fr.ParseFromString(bytes)) return "parse failed";
    switch (fr.payload_variant_case()) {
        case meshtastic::FromRadio::kMyInfo:
            return "MyInfo node=" + node_num_to_id(fr.my_info().my_node_num());
        case meshtastic::FromRadio::kNodeInfo:
            return "NodeInfo " + node_num_to_id(fr.node_info().num());
        case meshtastic::FromRadio::kChannel: {
            auto& ch = fr.channel();
            return "Channel idx=" + std::to_string(ch.index())
                 + " \"" + ch.settings().name() + "\"";
        }
        case meshtastic::FromRadio::kConfig:
            return "Config";
        case meshtastic::FromRadio::kModuleConfig:
            return "ModuleConfig";
        case meshtastic::FromRadio::kPacket: {
            auto& pkt = fr.packet();
            if (pkt.has_decoded()) {
                auto pn = pkt.decoded().portnum();
                return "Packet port=" + meshtastic::PortNum_Name(pn)
                     + " from=" + node_num_to_id(pkt.from());
            }
            return "Packet (encrypted)";
        }
        case meshtastic::FromRadio::kMetadata:
            return "Metadata fw=" + fr.metadata().firmware_version();
        case meshtastic::FromRadio::kConfigCompleteId:
            return "ConfigComplete id=" + std::to_string(fr.config_complete_id());
        case meshtastic::FromRadio::kLogRecord:
            return "LogRecord \"" + fr.log_record().message() + "\"";
        default:
            return "type=" + std::to_string(fr.payload_variant_case());
    }
}

} // namespace meshcli
