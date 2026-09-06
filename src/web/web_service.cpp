#include "web_service.h"
#include "mesh/mesh_service.h"
#include "util/log.h"

#include <chrono>

namespace meshcli {

namespace {

nlohmann::json node_to_json(const Node& n) {
    nlohmann::json j;
    j["node_num"] = n.node_num;
    j["node_id"] = n.node_id.empty() ? node_num_to_id(n.node_num) : n.node_id;
    j["long_name"] = n.long_name;
    j["short_name"] = n.short_name;
    j["hw_model"] = n.hw_model;
    j["role"] = n.role;
    j["is_favorite"] = n.is_favorite;
    j["is_ignored"] = n.is_ignored;
    j["is_muted"] = n.is_muted;
    j["is_key_verified"] = n.is_key_verified;

    if (n.battery_level) j["battery_level"] = *n.battery_level;
    else j["battery_level"] = nullptr;

    if (n.voltage) j["voltage"] = *n.voltage;
    else j["voltage"] = nullptr;

    if (n.snr) j["snr"] = *n.snr;
    else j["snr"] = nullptr;

    if (n.hops_away) j["hops_away"] = *n.hops_away;
    else j["hops_away"] = nullptr;

    if (n.last_heard) j["last_heard"] = *n.last_heard;
    else j["last_heard"] = nullptr;

    if (n.latitude) j["latitude"] = *n.latitude;
    else j["latitude"] = nullptr;

    if (n.longitude) j["longitude"] = *n.longitude;
    else j["longitude"] = nullptr;

    if (n.altitude) j["altitude"] = *n.altitude;
    else j["altitude"] = nullptr;

    // Environmental & channel telemetry
    if (n.temperature) j["temperature"] = *n.temperature;
    else j["temperature"] = nullptr;

    if (n.relative_humidity) j["relative_humidity"] = *n.relative_humidity;
    else j["relative_humidity"] = nullptr;

    if (n.barometric_pressure) j["barometric_pressure"] = *n.barometric_pressure;
    else j["barometric_pressure"] = nullptr;

    if (n.channel_util) j["channel_util"] = *n.channel_util;
    else j["channel_util"] = nullptr;

    if (n.air_util_tx) j["air_util_tx"] = *n.air_util_tx;
    else j["air_util_tx"] = nullptr;

    if (n.uptime_seconds) j["uptime_seconds"] = *n.uptime_seconds;
    else j["uptime_seconds"] = nullptr;

    return j;
}

nlohmann::json message_to_json(const StoredMessage& m) {
    return {
        {"rowid", m.rowid},
        {"device", m.device},
        {"window_kind", m.window_kind},
        {"window_target", m.window_target},
        {"direction", m.direction},
        {"from_node", m.from_node},
        {"to_node", m.to_node},
        {"channel_idx", m.channel_idx},
        {"text", m.text},
        {"ts", m.ts},
        {"packet_id", m.packet_id},
        {"ack_state", m.ack_state}
    };
}

nlohmann::json location_to_json(const Database::LocationRow& r) {
    return {
        {"device", r.device},
        {"node_num", r.node_num},
        {"latitude", r.latitude},
        {"longitude", r.longitude},
        {"altitude", r.altitude},
        {"ts", r.ts}
    };
}

nlohmann::json packet_activity_to_json(const PacketActivity& p) {
    nlohmann::json route_arr = nlohmann::json::array();
    for (uint32_t hop : p.route) route_arr.push_back(node_num_to_id(hop));
    return {
        {"device", p.device},
        {"from_node", p.from_node},
        {"from_id", p.from_id.empty() ? node_num_to_id(p.from_node) : p.from_id},
        {"to_node", p.to_node},
        {"to_id", p.to_id.empty() ? (p.broadcast ? "!ffffffff" : node_num_to_id(p.to_node)) : p.to_id},
        {"packet_id", p.packet_id},
        {"channel_idx", p.channel_idx},
        {"port_name", p.port_name},
        {"summary", p.summary},
        {"rx_snr", p.rx_snr},
        {"rx_rssi", p.rx_rssi},
        {"hop_limit", p.hop_limit},
        {"hop_start", p.hop_start},
        {"broadcast", p.broadcast},
        {"route", p.route},
        {"route_ids", route_arr},
        {"snr_towards", p.snr_towards},
        {"ts", p.ts}
    };
}

} // namespace

std::vector<PacketActivity> WebService::recent_packets() const {
    std::lock_guard<std::mutex> lock(packets_mu_);
    return {recent_packets_.begin(), recent_packets_.end()};
}

void WebService::record_and_broadcast_activity(const PacketActivity& act) {
    {
        std::lock_guard<std::mutex> lock(packets_mu_);
        recent_packets_.push_back(act);
        if (recent_packets_.size() > kMaxRecentPackets) {
            recent_packets_.pop_front();
        }
    }
    server_.broadcast_sse("packet_activity", packet_activity_to_json(act).dump());
}


WebService::WebService(MeshService& mesh_service)
    : mesh_service_(mesh_service) {
    mesh_service_.add_event_listener([this](const MeshEvent& ev) {
        on_mesh_event(ev);
    });
}

WebService::~WebService() {
    stop();
}

bool WebService::start(const std::string& host, int port, const std::string& web_root) {
    server_.set_static_dir(web_root);
    server_.set_sse_endpoint("/api/events");
    register_routes();
    return server_.start(host, port);
}

void WebService::stop() {
    server_.stop();
}

void WebService::register_routes() {
    // GET /api/status
    server_.get("/api/status", [this](const HttpRequest&) {
        auto dev_ids = mesh_service_.device_ids();
        nlohmann::json devices = nlohmann::json::array();
        for (const auto& id : dev_ids) {
            devices.push_back({
                {"id", id},
                {"display_name", mesh_service_.display_name_for(id)},
                {"hw_model", mesh_service_.hw_model_for(id)},
                {"firmware", mesh_service_.firmware_for(id)}
            });
        }

        nlohmann::json status = {
            {"status", "online"},
            {"version", "fmesh-cli 1.0"},
            {"active_device", active_device_id_.empty() && !dev_ids.empty() ? dev_ids.front() : active_device_id_},
            {"devices", devices},
            {"web_port", server_.bound_port()}
        };
        return HttpResponse::json(200, status);
    });

    // GET /api/devices
    server_.get("/api/devices", [this](const HttpRequest&) {
        auto dev_ids = mesh_service_.device_ids();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& id : dev_ids) {
            const NodeDb* db = mesh_service_.db_for(id);
            uint32_t my_node = db ? db->my_node_num() : 0;
            arr.push_back({
                {"id", id},
                {"display_name", mesh_service_.display_name_for(id)},
                {"my_node_num", my_node},
                {"my_node_id", node_num_to_id(my_node)},
                {"hw_model", mesh_service_.hw_model_for(id)},
                {"firmware", mesh_service_.firmware_for(id)},
                {"virtual_stream", mesh_service_.virtual_stream_for(id)},
                {"virtual_original", mesh_service_.virtual_original_for(id)}
            });
        }
        return HttpResponse::json(200, arr);
    });

    // POST /api/devices/select
    server_.post("/api/devices/select", [this](const HttpRequest& req) {
        try {
            auto j = nlohmann::json::parse(req.body);
            std::string id = j.value("device", "");
            if (!id.empty()) {
                active_device_id_ = id;
                return HttpResponse::json(200, {{"success", true}, {"active_device", active_device_id_}});
            }
        } catch (...) {}
        return HttpResponse::bad_request("Missing device id");
    });

    // GET /api/nodes
    server_.get("/api/nodes", [this](const HttpRequest& req) {
        std::string filter_device = req.get_query("device");
        auto dev_ids = mesh_service_.device_ids();

        std::map<uint32_t, Node> unique_nodes;
        for (const auto& dev_id : dev_ids) {
            if (!filter_device.empty() && filter_device != dev_id) continue;

            const NodeDb* db = mesh_service_.db_for(dev_id);
            if (!db) continue;

            for (const auto& node : db->all()) {
                auto it = unique_nodes.find(node.node_num);
                if (it == unique_nodes.end() || (node.last_heard.value_or(0) > it->second.last_heard.value_or(0))) {
                    unique_nodes[node.node_num] = node;
                }
            }
        }

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& [_, node] : unique_nodes) {
            arr.push_back(node_to_json(node));
        }
        return HttpResponse::json(200, arr);
    });

    // GET /api/channels
    server_.get("/api/channels", [this](const HttpRequest& req) {
        std::string dev_id = req.get_query("device");
        if (dev_id.empty()) {
            auto dev_ids = mesh_service_.device_ids();
            if (!dev_ids.empty()) dev_id = dev_ids.front();
        }

        nlohmann::json arr = nlohmann::json::array();
        const NodeDb* db = mesh_service_.db_for(dev_id);
        if (db) {
            for (const auto& ch : db->channels()) {
                arr.push_back({
                    {"index", ch.index},
                    {"name", ch.name},
                    {"role", ch.role},
                    {"has_psk", ch.has_psk}
                });
            }
        }
        return HttpResponse::json(200, arr);
    });

    // GET /api/locations
    server_.get("/api/locations", [this](const HttpRequest& req) {
        std::string node_str = req.get_query("node_num");
        std::string since_str = req.get_query("since", "0");
        std::string limit_str = req.get_query("limit", "500");

        uint64_t since_ts = 0;
        int limit = 500;
        try { since_ts = std::stoull(since_str); } catch (...) {}
        try { limit = std::stoi(limit_str); } catch (...) {}

        std::vector<Database::LocationRow> rows;
        if (!node_str.empty()) {
            uint32_t node_num = 0;
            if (parse_node_id(node_str, node_num)) {
                rows = mesh_service_.database().get_node_locations(node_num, since_ts, limit);
            }
        } else {
            rows = mesh_service_.database().get_recent_node_locations(since_ts, limit);
        }

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& r : rows) {
            arr.push_back(location_to_json(r));
        }
        return HttpResponse::json(200, arr);
    });

    // GET /api/messages
    server_.get("/api/messages", [this](const HttpRequest& req) {
        WindowKey w;
        w.device = req.get_query("device");
        w.kind = req.get_query("kind", "channel"); // "channel", "dm", "status"
        std::string target_str = req.get_query("target", "0");
        try { w.target = static_cast<uint32_t>(std::stoul(target_str)); } catch (...) {}

        int limit = 100;
        int offset = 0;
        try { limit = std::stoi(req.get_query("limit", "100")); } catch (...) {}
        try { offset = std::stoi(req.get_query("offset", "0")); } catch (...) {}

        auto msgs = mesh_service_.database().get_messages_paginated(w, limit, offset);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& m : msgs) {
            arr.push_back(message_to_json(m));
        }
        return HttpResponse::json(200, arr);
    });

    // GET /api/conversations
    server_.get("/api/conversations", [this](const HttpRequest& req) {
        std::string dev = req.get_query("device");
        if (dev.empty()) dev = active_device_id_;
        if (dev.empty()) {
            auto ids = mesh_service_.device_ids();
            if (!ids.empty()) dev = ids.front();
        }
        auto windows = mesh_service_.database().get_all_windows(dev);
        const NodeDb* db = mesh_service_.db_for(dev);
        nlohmann::json ch_arr = nlohmann::json::array();
        nlohmann::json dm_arr = nlohmann::json::array();
        for (const auto& w : windows) {
            if (w.kind == "channel") {
                std::string name = "";
                if (db) {
                    auto ch = db->channel(w.target);
                    if (ch) name = ch->name;
                }
                ch_arr.push_back({
                    {"index", w.target},
                    {"name", name}
                });
            } else if (w.kind == "dm") {
                std::string nick = "";
                if (db) {
                    auto n = db->get(w.target);
                    if (n) nick = n->short_name.empty() ? n->long_name : n->short_name;
                }
                if (nick.empty()) nick = node_num_to_id(w.target);
                dm_arr.push_back({
                    {"node_num", w.target},
                    {"nick", nick}
                });
            }
        }
        nlohmann::json res = {
            {"channels", ch_arr},
            {"dms", dm_arr}
        };
        return HttpResponse::json(200, res);
    });

    // POST /api/messages
    server_.post("/api/messages", [this](const HttpRequest& req) {
        try {
            auto j = nlohmann::json::parse(req.body);
            std::string device = j.value("device", "");
            if (device.empty()) {
                device = active_device_id_;
                if (device.empty()) {
                    auto ids = mesh_service_.device_ids();
                    if (!ids.empty()) device = ids.front();
                }
            }

            uint32_t to_node = j.value("to_node", kBroadcastNodeNum);
            uint32_t channel_idx = j.value("channel_idx", 0);
            std::string text = j.value("text", "");
            bool want_ack = j.value("want_ack", true);

            if (text.empty()) {
                return HttpResponse::bad_request("Empty message text");
            }

            uint32_t packet_id = mesh_service_.send_text(device, to_node, channel_idx, text, want_ack);
            if (packet_id == 0) {
                return HttpResponse::error("Failed to transmit message");
            }

            const NodeDb* db = mesh_service_.db_for(device);
            uint32_t my_node = db ? db->my_node_num() : 0;
            PacketActivity act;
            act.device = device;
            act.from_node = my_node;
            act.from_id = node_num_to_id(my_node);
            act.to_node = to_node;
            act.to_id = (to_node == kBroadcastNodeNum) ? "!ffffffff" : node_num_to_id(to_node);
            act.packet_id = packet_id;
            act.channel_idx = channel_idx;
            act.port_name = "TEXT_MESSAGE_APP";
            act.summary = text;
            act.broadcast = (to_node == kBroadcastNodeNum);
            act.ts = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            record_and_broadcast_activity(act);

            return HttpResponse::json(200, {
                {"success", true},
                {"packet_id", packet_id},
                {"device", device},
                {"to_node", to_node},
                {"channel_idx", channel_idx}
            });
        } catch (const std::exception& e) {
            return HttpResponse::bad_request(e.what());
        }
    });

    // POST /api/traceroute
    server_.post("/api/traceroute", [this](const HttpRequest& req) {
        try {
            auto j = nlohmann::json::parse(req.body);
            std::string device = j.value("device", "");
            if (device.empty()) {
                device = active_device_id_;
                if (device.empty()) {
                    auto ids = mesh_service_.device_ids();
                    if (!ids.empty()) device = ids.front();
                }
            }

            uint32_t to_node = j.value("to_node", 0);
            uint32_t channel_idx = j.value("channel_idx", 0);
            uint32_t hop_limit = j.value("hop_limit", 0);

            if (to_node == 0) {
                return HttpResponse::bad_request("Invalid target node");
            }

            uint32_t packet_id = mesh_service_.send_traceroute(device, to_node, channel_idx, hop_limit);
            if (packet_id == 0) {
                return HttpResponse::error("Failed to dispatch traceroute");
            }

            const NodeDb* db = mesh_service_.db_for(device);
            uint32_t my_node = db ? db->my_node_num() : 0;
            PacketActivity act;
            act.device = device;
            act.from_node = my_node;
            act.from_id = node_num_to_id(my_node);
            act.to_node = to_node;
            act.to_id = node_num_to_id(to_node);
            act.packet_id = packet_id;
            act.channel_idx = channel_idx;
            act.hop_limit = hop_limit;
            act.port_name = "TRACEROUTE_APP";
            act.summary = "Traceroute request to " + node_num_to_id(to_node);
            act.broadcast = false;
            act.ts = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            record_and_broadcast_activity(act);

            return HttpResponse::json(200, {
                {"success", true},
                {"packet_id", packet_id},
                {"device", device},
                {"to_node", to_node}
            });
        } catch (const std::exception& e) {
            return HttpResponse::bad_request(e.what());
        }
    });

    // GET /api/config
    server_.get("/api/config", [this](const HttpRequest& req) {
        std::string device = req.get_query("device");
        if (device.empty()) {
            auto ids = mesh_service_.device_ids();
            if (!ids.empty()) device = ids.front();
        }

        auto lines = mesh_service_.config_lines_for(device);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& line : lines) {
            size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string k = line.substr(0, eq);
                std::string v = line.substr(eq + 1);
                while (!k.empty() && k.back() == ' ') k.pop_back();
                while (!v.empty() && v.front() == ' ') v.erase(v.begin());
                arr.push_back({{"key", k}, {"value", v}});
            }
        }
        return HttpResponse::json(200, {{"device", device}, {"config", arr}});
    });

    // POST /api/config
    server_.post("/api/config", [this](const HttpRequest& req) {
        try {
            auto j = nlohmann::json::parse(req.body);
            std::string device = j.value("device", "");
            if (device.empty()) {
                auto ids = mesh_service_.device_ids();
                if (!ids.empty()) device = ids.front();
            }

            std::string key = j.value("key", "");
            std::string val = j.value("value", "");

            if (key.empty()) return HttpResponse::bad_request("Missing config key");

            bool ok = mesh_service_.set_config(device, key, val);
            return HttpResponse::json(200, {{"success", ok}, {"device", device}, {"key", key}, {"value", val}});
        } catch (const std::exception& e) {
            return HttpResponse::bad_request(e.what());
        }
    });

    // GET /api/raw
    server_.get("/api/raw", [this](const HttpRequest& req) {
        std::string device = req.get_query("device");
        if (device.empty()) {
            auto ids = mesh_service_.device_ids();
            if (!ids.empty()) device = ids.front();
        }

        auto raw = mesh_service_.raw_packets_for(device);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& pkt : raw) {
            arr.push_back({
                {"device", pkt.device},
                {"hex", pkt.hex},
                {"summary", pkt.summary},
                {"ts", pkt.ts}
            });
        }
        return HttpResponse::json(200, arr);
    });

    // GET /api/packets
    server_.get("/api/packets", [this](const HttpRequest& req) {
        int limit = 50;
        try { limit = std::stoi(req.get_query("limit", "50")); } catch (...) {}
        if (limit <= 0) limit = 50;

        auto all = recent_packets();
        nlohmann::json arr = nlohmann::json::array();
        size_t start_idx = (all.size() > static_cast<size_t>(limit)) ? (all.size() - static_cast<size_t>(limit)) : 0;
        for (size_t i = start_idx; i < all.size(); ++i) {
            arr.push_back(packet_activity_to_json(all[i]));
        }
        return HttpResponse::json(200, arr);
    });
}

void WebService::on_mesh_event(const MeshEvent& ev) {
    std::visit([this](const auto& e) {
        using T = std::decay_t<decltype(e)>;

        if constexpr (std::is_same_v<T, EvNodeUpdated>) {
            nlohmann::json data = {
                {"type", "node_updated"},
                {"device", e.device},
                {"node", node_to_json(e.node)},
                {"is_new", e.is_new}
            };
            server_.broadcast_sse("node_updated", data.dump());

            if (e.is_new || e.node.battery_level || e.node.voltage) {
                PacketActivity act;
                act.device = e.device;
                act.from_node = e.node.node_num;
                act.from_id = e.node.node_id;
                act.to_node = kBroadcastNodeNum;
                act.to_id = "!ffffffff";
                act.port_name = e.is_new ? "NODEINFO_APP" : "TELEMETRY_APP";
                act.summary = e.node.long_name.empty() ? e.node.node_id : e.node.long_name;
                if (e.node.snr) act.rx_snr = *e.node.snr;
                act.broadcast = true;
                act.ts = e.node.last_heard ? *e.node.last_heard : static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count());
                record_and_broadcast_activity(act);
            }
        }
        else if constexpr (std::is_same_v<T, EvPositionReceived>) {
            nlohmann::json data = {
                {"type", "position_received"},
                {"device", e.device},
                {"from_node", e.from_node},
                {"node_id", node_num_to_id(e.from_node)},
                {"latitude", e.latitude},
                {"longitude", e.longitude},
                {"altitude", e.altitude},
                {"rx_time", e.rx_time}
            };
            server_.broadcast_sse("position_received", data.dump());

            PacketActivity act;
            act.device = e.device;
            act.from_node = e.from_node;
            act.from_id = node_num_to_id(e.from_node);
            act.to_node = kBroadcastNodeNum;
            act.to_id = "!ffffffff";
            act.port_name = "POSITION_APP";
            act.summary = "GPS Position Update";
            act.broadcast = true;
            act.ts = e.rx_time ? e.rx_time : static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            record_and_broadcast_activity(act);
        }
        else if constexpr (std::is_same_v<T, EvTextReceived>) {
            nlohmann::json data = {
                {"type", "message_received"},
                {"device", e.device},
                {"from_node", e.from_node},
                {"from_id", node_num_to_id(e.from_node)},
                {"to_node", e.to_node},
                {"channel_idx", e.channel_idx},
                {"text", e.text},
                {"packet_id", e.packet_id},
                {"rx_snr", e.rx_snr},
                {"rx_rssi", e.rx_rssi},
                {"broadcast", e.broadcast}
            };
            server_.broadcast_sse("message_received", data.dump());

            PacketActivity act;
            act.device = e.device;
            act.from_node = e.from_node;
            act.from_id = node_num_to_id(e.from_node);
            act.to_node = e.to_node;
            act.to_id = (e.broadcast || e.to_node == kBroadcastNodeNum) ? "!ffffffff" : node_num_to_id(e.to_node);
            act.packet_id = e.packet_id;
            act.channel_idx = e.channel_idx;
            act.port_name = "TEXT_MESSAGE_APP";
            act.summary = e.text;
            act.rx_snr = e.rx_snr;
            act.rx_rssi = e.rx_rssi;
            act.hop_limit = e.hop_limit;
            act.hop_start = e.hop_start;
            act.broadcast = e.broadcast;
            act.ts = e.rx_time ? e.rx_time : static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            record_and_broadcast_activity(act);
        }
        else if constexpr (std::is_same_v<T, EvAckReceived>) {
            nlohmann::json data = {
                {"type", "ack_received"},
                {"device", e.device},
                {"packet_id", e.packet_id},
                {"from_node", e.from_node},
                {"success", e.success},
                {"error_reason", e.error_reason}
            };
            server_.broadcast_sse("ack_received", data.dump());

            const NodeDb* db = mesh_service_.db_for(e.device);
            uint32_t my_node = db ? db->my_node_num() : 0;
            PacketActivity act;
            act.device = e.device;
            act.from_node = e.from_node;
            act.from_id = node_num_to_id(e.from_node);
            act.to_node = my_node;
            act.to_id = node_num_to_id(my_node);
            act.packet_id = e.packet_id;
            act.port_name = "ROUTING_APP";
            act.summary = e.success ? "ACK (Acknowledged)" : ("NAK (" + e.error_reason + ")");
            act.broadcast = false;
            act.ts = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            record_and_broadcast_activity(act);
        }
        else if constexpr (std::is_same_v<T, EvTracerouteReceived>) {
            nlohmann::json route_arr = nlohmann::json::array();
            for (uint32_t hop : e.route) route_arr.push_back(node_num_to_id(hop));
            nlohmann::json back_arr = nlohmann::json::array();
            for (uint32_t hop : e.route_back) back_arr.push_back(node_num_to_id(hop));

            nlohmann::json data = {
                {"type", "traceroute_received"},
                {"device", e.device},
                {"from_node", e.from_node},
                {"to_node", e.to_node},
                {"from_id", node_num_to_id(e.from_node)},
                {"to_id", node_num_to_id(e.to_node)},
                {"route", e.route},
                {"route_ids", route_arr},
                {"snr_towards", e.snr_towards},
                {"route_back", e.route_back},
                {"route_back_ids", back_arr},
                {"snr_back", e.snr_back}
            };
            server_.broadcast_sse("traceroute_received", data.dump());

            PacketActivity act;
            act.device = e.device;
            act.from_node = e.from_node;
            act.from_id = node_num_to_id(e.from_node);
            act.to_node = e.to_node;
            act.to_id = node_num_to_id(e.to_node);
            act.port_name = "TRACEROUTE_APP";
            act.summary = "Traceroute response (" + std::to_string(e.route.size()) + " hops)";
            act.route = e.route;
            act.snr_towards = e.snr_towards;
            act.broadcast = false;
            act.ts = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            record_and_broadcast_activity(act);
        }

        else if constexpr (std::is_same_v<T, EvConnected>) {
            nlohmann::json data = {
                {"type", "device_connected"},
                {"device", e.device},
                {"display_name", e.display_name}
            };
            server_.broadcast_sse("device_connected", data.dump());
        }
        else if constexpr (std::is_same_v<T, EvDisconnected>) {
            nlohmann::json data = {
                {"type", "device_disconnected"},
                {"device", e.device},
                {"reason", e.reason}
            };
            server_.broadcast_sse("device_disconnected", data.dump());
        }
        else if constexpr (std::is_same_v<T, EvRawPacket>) {
            nlohmann::json data = {
                {"type", "raw_packet"},
                {"device", e.device},
                {"summary", e.summary},
                {"hex", e.hex},
                {"ts", e.ts}
            };
            server_.broadcast_sse("raw_packet", data.dump());
        }
    }, ev);
}

} // namespace meshcli
