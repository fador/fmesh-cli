#include "db_sync.h"
#include "util/log.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace meshcli {

DbSyncManager::DbSyncManager(Database& db, MeshService& mesh)
    : db_(db), mesh_(mesh) {
}

void DbSyncManager::handle_sync_payload(const std::string& device, const std::string& payload) {
    try {
        auto j = json::parse(payload);
        std::string type = j.value("type", "");
        
        LOG_DEBUG() << "DbSyncManager: received " << type << " from " << device;

        if (type == "inventory") {
            handle_inventory(device, payload);
        } else if (type == "request") {
            handle_request(device, payload);
        } else if (type == "data") {
            handle_data(device, payload);
        } else if (type == "devices") {
            handle_devices(device, payload);
        } else if (type == "send_raw") {
            handle_send_raw(payload);
        } else {
            LOG_DEBUG() << "DbSyncManager: unknown type " << type;
        }
    } catch (const std::exception& e) {
        LOG_DEBUG() << "DbSyncManager: json parse error: " << e.what();
    }
}

void DbSyncManager::initiate_sync() {
    json j;
    j["type"] = "inventory";
    j["max_message_ts"] = db_.max_message_ts();
    j["max_location_ts"] = db_.max_location_ts();

    mesh_.send_db_sync(j.dump());

    // Broadcast our local devices too
    std::vector<std::pair<std::string, uint32_t>> local_devs;
    for (const auto& id : mesh_.device_ids()) {
        // Skip virtual devices and offline dbs
        if (id.find("virtual:") == 0) continue;
        const auto* ndb = mesh_.db_for(id);
        if (ndb && ndb->my_node_num() != 0) {
            local_devs.push_back({id, ndb->my_node_num()});
        }
    }
    push_devices(local_devs);
}

void DbSyncManager::push_message(const StoredMessage& m) {
    json msg;
    msg["rowid"] = m.rowid;
    msg["device"] = m.device;
    msg["window_kind"] = m.window_kind;
    msg["window_target"] = m.window_target;
    msg["direction"] = m.direction;
    msg["from_node"] = m.from_node;
    msg["to_node"] = m.to_node;
    msg["channel_idx"] = m.channel_idx;
    msg["text"] = m.text;
    msg["ts"] = m.ts;
    msg["packet_id"] = m.packet_id;
    msg["ack_state"] = m.ack_state;

    json j;
    j["type"] = "data";
    j["messages"] = json::array({msg});

    mesh_.send_db_sync(j.dump());
}

void DbSyncManager::push_location(const Database::LocationRow& loc) {
    json row;
    row["device"] = loc.device;
    row["node_num"] = loc.node_num;
    row["latitude"] = loc.latitude;
    row["longitude"] = loc.longitude;
    row["altitude"] = loc.altitude;
    row["ts"] = loc.ts;

    json j;
    j["type"] = "data";
    j["locations"] = json::array({row});

    mesh_.send_db_sync(j.dump());
}

void DbSyncManager::handle_inventory(const std::string& device, const std::string& json_str) {
    auto j = json::parse(json_str);
    uint64_t remote_msg = j.value("max_message_ts", (uint64_t)0);
    uint64_t remote_loc = j.value("max_location_ts", (uint64_t)0);

    uint64_t local_msg = db_.max_message_ts();
    uint64_t local_loc = db_.max_location_ts();

    // If they have more than us, request the difference.
    if (remote_msg > local_msg || remote_loc > local_loc) {
        json req;
        req["type"] = "request";
        req["after_message_ts"] = local_msg;
        req["after_location_ts"] = local_loc;
        
        mesh_.send_db_sync(req.dump());
    }
    
    // If we have more than them, proactively send our data to them
    // by pretending they requested it from their max points.
    if (local_msg > remote_msg || local_loc > remote_loc) {
        json req;
        req["after_message_ts"] = remote_msg;
        req["after_location_ts"] = remote_loc;
        handle_request(device, req.dump());
    }
}

void DbSyncManager::handle_request(const std::string& device, const std::string& json_str) {
    auto j = json::parse(json_str);
    uint64_t after_msg = j.value("after_message_ts", (uint64_t)0);
    uint64_t after_loc = j.value("after_location_ts", (uint64_t)0);

    auto msgs = db_.get_messages_after_ts(after_msg, 21); // fetch 21 to check if has_more
    auto locs = db_.get_locations_after(after_loc, 21);

    if (msgs.empty() && locs.empty()) return;

    bool has_more = false;
    if (msgs.size() > 20) {
        has_more = true;
        msgs.pop_back(); // remove the 21st
    }
    if (locs.size() > 20) {
        has_more = true;
        locs.pop_back(); // remove the 21st
    }

    json resp;
    resp["type"] = "data";
    resp["has_more"] = has_more;
    
    json m_arr = json::array();
    for (const auto& m : msgs) {
        json msg;
        msg["rowid"] = m.rowid; // Optional: we might not trust remote rowids directly, but we pass it.
        msg["device"] = m.device;
        msg["window_kind"] = m.window_kind;
        msg["window_target"] = m.window_target;
        msg["direction"] = m.direction;
        msg["from_node"] = m.from_node;
        msg["to_node"] = m.to_node;
        msg["channel_idx"] = m.channel_idx;
        msg["text"] = m.text;
        msg["ts"] = m.ts;
        msg["packet_id"] = m.packet_id;
        msg["ack_state"] = m.ack_state;
        m_arr.push_back(msg);
    }
    resp["messages"] = m_arr;

    json l_arr = json::array();
    for (const auto& loc : locs) {
        json row;
        row["device"] = loc.device;
        row["node_num"] = loc.node_num;
        row["latitude"] = loc.latitude;
        row["longitude"] = loc.longitude;
        row["altitude"] = loc.altitude;
        row["ts"] = loc.ts;
        l_arr.push_back(row);
    }
    resp["locations"] = l_arr;

    mesh_.send_db_sync(resp.dump());
}

void DbSyncManager::handle_data(const std::string& device, const std::string& json_str) {
    auto j = json::parse(json_str);

    if (j.contains("messages")) {
        for (const auto& msg : j["messages"]) {
            // We ignore rowid, letting SQLite assign a new local rowid.
            // Check if we already have this packet_id (from same sender).
            uint32_t pkt = msg.value("packet_id", (uint32_t)0);
            if (pkt > 0) {
                if (db_.find_by_packet_id(pkt)) continue; // Already have it
            }

            StoredMessage m;
            m.device = msg.value("device", "");
            m.window_kind = msg.value("window_kind", "");
            m.window_target = msg.value("window_target", (uint32_t)0);
            m.direction = msg.value("direction", "");
            m.from_node = msg.value("from_node", (uint32_t)0);
            m.to_node = msg.value("to_node", (uint32_t)0);
            m.channel_idx = msg.value("channel_idx", (uint32_t)0);
            m.text = msg.value("text", "");
            m.ts = msg.value("ts", (uint64_t)0);
            m.packet_id = pkt;
            m.ack_state = msg.value("ack_state", "");

            db_.insert_message(m);
            push_message(m); // Forward to other connected mesh peers

            EvTextReceived ev;
            ev.device = m.device;
            ev.from_node = m.from_node;
            ev.to_node = m.to_node;
            ev.channel_idx = m.channel_idx;
            ev.text = m.text;
            ev.rx_time = m.ts;
            ev.packet_id = m.packet_id;
            ev.broadcast = (m.window_kind == "channel");
            mesh_.dispatch_to_ui(ev);
        }
    }

    if (j.contains("locations")) {
        for (const auto& loc : j["locations"]) {
            std::string d = loc.value("device", "");
            uint32_t node_num = loc.value("node_num", (uint32_t)0);
            double lat = loc.value("latitude", 0.0);
            double lon = loc.value("longitude", 0.0);
            int alt = loc.value("altitude", 0);
            uint64_t ts = loc.value("ts", (uint64_t)0);

            db_.insert_location(d, node_num, lat, lon, alt, ts);
            
            Database::LocationRow lr;
            lr.device = d;
            lr.node_num = node_num;
            lr.latitude = lat;
            lr.longitude = lon;
            lr.altitude = alt;
            lr.ts = ts;
            push_location(lr); // Forward to other connected mesh peers
            // We could emit EvNodeUpdated to UI, but UI probably re-queries DB on /nodes.
        }
    }

    if (j.value("has_more", false)) {
        initiate_sync();
    }
}

// Hex encoding/decoding helpers
static std::string bytes_to_hex(const std::string& bytes) {
    static const char hex_chars[] = "0123456789ABCDEF";
    std::string ret;
    ret.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        ret.push_back(hex_chars[b >> 4]);
        ret.push_back(hex_chars[b & 0x0F]);
    }
    return ret;
}

static std::string hex_to_bytes(const std::string& hex) {
    std::string bytes;
    if (hex.length() % 2 != 0) return bytes;
    bytes.reserve(hex.length() / 2);
    for (size_t i = 0; i < hex.length(); i += 2) {
        uint8_t hi = hex[i] <= '9' ? hex[i] - '0' : (hex[i] & ~0x20) - 'A' + 10;
        uint8_t lo = hex[i+1] <= '9' ? hex[i+1] - '0' : (hex[i+1] & ~0x20) - 'A' + 10;
        bytes.push_back((char)((hi << 4) | lo));
    }
    return bytes;
}

void DbSyncManager::push_devices(const std::vector<std::pair<std::string, uint32_t>>& local_devices) {
    if (local_devices.empty()) return;
    json j;
    j["type"] = "devices";
    json arr = json::array();
    for (const auto& dev : local_devices) {
        json item;
        item["id"] = dev.first;
        item["node_num"] = dev.second;
        arr.push_back(item);
    }
    j["devices"] = arr;
    mesh_.send_db_sync(j.dump());
}

void DbSyncManager::send_raw_to_device(const std::string& target_original_id, const std::string& bytes) {
    json j;
    j["type"] = "send_raw";
    j["target"] = target_original_id;
    j["payload"] = bytes_to_hex(bytes);
    mesh_.send_db_sync(j.dump());
}

void DbSyncManager::handle_devices(const std::string& device, const std::string& json_str) {
    auto j = json::parse(json_str);
    if (!j.contains("devices")) return;
    
    std::vector<VirtualDevice> vdevs;
    for (const auto& item : j["devices"]) {
        std::string orig_id = item.value("id", "");
        uint32_t node_num = item.value("node_num", (uint32_t)0);
        if (orig_id.empty()) continue;
        
        VirtualDevice vd;
        vd.original_id = orig_id;
        vd.stream_id = device;
        vd.id = "virtual:" + device + ":" + orig_id;
        vd.node_num = node_num;
        vdevs.push_back(vd);
    }
    mesh_.update_virtual_devices(device, vdevs);
}

void DbSyncManager::handle_send_raw(const std::string& json_str) {
    auto j = json::parse(json_str);
    std::string target = j.value("target", "");
    std::string hex_payload = j.value("payload", "");
    
    if (target.empty() || hex_payload.empty()) return;
    
    auto bytes = hex_to_bytes(hex_payload);
    mesh_.send_raw_to_physical(target, bytes);
}

} // namespace meshcli
