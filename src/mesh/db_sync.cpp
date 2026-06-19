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

    EvSendDbSyncToTcp ev;
    ev.payload = j.dump();
    // In MeshService we handle EvSendDbSyncToTcp by broadcasting it.
    mesh_.dispatch_to_ui(ev); // wait, UI shouldn't broadcast it, mesh_service does.
    // Let's actually provide a method on mesh_service, or we can just push EvSendDbSyncToTcp directly.
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

    EvSendDbSyncToTcp ev;
    ev.payload = j.dump();
    mesh_.dispatch_to_ui(ev); // The UI will loop it back to MeshService, or we need to add a method.
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

    EvSendDbSyncToTcp ev;
    ev.payload = j.dump();
    mesh_.dispatch_to_ui(ev);
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
        
        EvSendDbSyncToTcp ev;
        ev.payload = req.dump();
        mesh_.dispatch_to_ui(ev);
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

    auto msgs = db_.get_messages_after_ts(after_msg, 1001); // fetch 1001 to check if has_more
    auto locs = db_.get_locations_after(after_loc, 1001);

    if (msgs.empty() && locs.empty()) return;

    bool has_more = false;
    if (msgs.size() > 1000) {
        has_more = true;
        msgs.pop_back(); // remove the 1001st
    }
    if (locs.size() > 1000) {
        has_more = true;
        locs.pop_back(); // remove the 1001st
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

    EvSendDbSyncToTcp ev;
    ev.payload = resp.dump();
    mesh_.dispatch_to_ui(ev);
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
            // We could emit EvNodeUpdated to UI, but UI probably re-queries DB on /nodes.
        }
    }

    if (j.value("has_more", false)) {
        initiate_sync();
    }
}

} // namespace meshcli
