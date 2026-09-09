#include "web_service.h"
#include "mesh/mesh_service.h"
#include "util/log.h"

#include <chrono>
#include <set>

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
    uint32_t hops = (m.hop_start > m.hop_limit) ? (m.hop_start - m.hop_limit) : 0;
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
        {"ack_state", m.ack_state},
        {"rx_snr", m.rx_snr},
        {"rx_rssi", m.rx_rssi},
        {"hop_start", m.hop_start},
        {"hop_limit", m.hop_limit},
        {"hops", hops},
        {"relay_node", m.relay_node},
        {"reply_id", m.reply_id},
        {"emoji", m.emoji}
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

nlohmann::json telemetry_to_json(const Database::TelemetryRow& r) {
    nlohmann::json j = {
        {"device", r.device},
        {"node_num", r.node_num},
        {"node_id", node_num_to_id(r.node_num)},
        {"ts", r.ts}
    };
    if (r.battery_level) j["battery_level"] = *r.battery_level; else j["battery_level"] = nullptr;
    if (r.voltage) j["voltage"] = *r.voltage; else j["voltage"] = nullptr;
    if (r.channel_util) j["channel_util"] = *r.channel_util; else j["channel_util"] = nullptr;
    if (r.air_util_tx) j["air_util_tx"] = *r.air_util_tx; else j["air_util_tx"] = nullptr;
    if (r.uptime_seconds) j["uptime_seconds"] = *r.uptime_seconds; else j["uptime_seconds"] = nullptr;
    if (r.temperature) j["temperature"] = *r.temperature; else j["temperature"] = nullptr;
    if (r.relative_humidity) j["relative_humidity"] = *r.relative_humidity; else j["relative_humidity"] = nullptr;
    if (r.barometric_pressure) j["barometric_pressure"] = *r.barometric_pressure; else j["barometric_pressure"] = nullptr;
    if (r.gas_resistance) j["gas_resistance"] = *r.gas_resistance; else j["gas_resistance"] = nullptr;
    if (r.iaq) j["iaq"] = *r.iaq; else j["iaq"] = nullptr;
    if (r.pm25) j["pm25"] = *r.pm25; else j["pm25"] = nullptr;
    if (r.co2) j["co2"] = *r.co2; else j["co2"] = nullptr;
    if (r.current) j["current"] = *r.current; else j["current"] = nullptr;
    if (r.snr) j["snr"] = *r.snr; else j["snr"] = nullptr;
    if (r.hops_away) j["hops_away"] = *r.hops_away; else j["hops_away"] = nullptr;
    return j;
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

nlohmann::json rf_link_to_json(const RfLink& l) {
    return {
        {"device", l.device},
        {"from_node", l.from_node},
        {"from_id", l.from_id},
        {"to_node", l.to_node},
        {"to_id", l.to_id},
        {"snr", l.snr},
        {"source", l.source},
        {"last_heard", l.last_heard}
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

    Database::PacketLogRow row;
    row.device = act.device;
    row.from_node = act.from_node;
    row.to_node = act.to_node;
    row.port_name = act.port_name;
    row.channel_idx = act.channel_idx;
    row.rx_snr = act.rx_snr;
    row.rx_rssi = act.rx_rssi;
    row.hop_limit = act.hop_limit;
    row.hop_start = act.hop_start;
    row.broadcast = act.broadcast;
    row.summary = act.summary;
    row.ts = act.ts != 0 ? act.ts : static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    mesh_service_.database().insert_packet_log(row);

    server_.broadcast_sse("packet_activity", packet_activity_to_json(act).dump());
}

void WebService::update_link(const std::string& device, uint32_t from_node, uint32_t to_node,
                             float snr, const std::string& source, uint64_t last_heard) {
    if (from_node == 0 || to_node == 0 || from_node == kBroadcastNodeNum ||
        to_node == kBroadcastNodeNum || from_node == to_node) {
        return;
    }

    uint32_t u1 = (std::min)(from_node, to_node);
    uint32_t u2 = (std::max)(from_node, to_node);
    auto key = std::make_pair(u1, u2);

    RfLink link;
    link.device = device;
    link.from_node = from_node;
    link.from_id = node_num_to_id(from_node);
    link.to_node = to_node;
    link.to_id = node_num_to_id(to_node);
    link.snr = snr;
    link.source = source;
    link.last_heard = last_heard != 0 ? last_heard : static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    {
        std::lock_guard<std::mutex> lock(links_mu_);
        links_[key] = link;
    }

    server_.broadcast_sse("link_updated", rf_link_to_json(link).dump());
}

std::vector<RfLink> WebService::get_links(const std::string& device) const {
    std::map<std::pair<uint32_t, uint32_t>, RfLink> result;
    {
        std::lock_guard<std::mutex> lock(links_mu_);
        for (const auto& [k, link] : links_) {
            if (device.empty() || link.device.empty() || link.device == device) {
                result[k] = link;
            }
        }
    }

    // Include direct neighbors from NodeDb (nodes with hops_away == 0)
    auto dev_ids = mesh_service_.device_ids();
    for (const auto& dev_id : dev_ids) {
        if (!device.empty() && device != dev_id) continue;
        const NodeDb* db = mesh_service_.db_for(dev_id);
        if (!db) continue;
        uint32_t my_node = db->my_node_num();
        if (my_node == 0) continue;

        for (const auto& node : db->all()) {
            if (node.node_num != my_node && node.hops_away.has_value() && *node.hops_away == 0) {
                uint32_t u1 = (std::min)(my_node, node.node_num);
                uint32_t u2 = (std::max)(my_node, node.node_num);
                auto key = std::make_pair(u1, u2);
                if (result.find(key) == result.end()) {
                    RfLink l;
                    l.device = dev_id;
                    l.from_node = my_node;
                    l.from_id = node_num_to_id(my_node);
                    l.to_node = node.node_num;
                    l.to_id = node_num_to_id(node.node_num);
                    l.snr = node.snr.value_or(0.0f);
                    l.source = "direct";
                    l.last_heard = node.last_heard.value_or(0);
                    result[key] = l;
                }
            }
        }
    }

    std::vector<RfLink> list;
    list.reserve(result.size());
    for (auto& [_, l] : result) {
        list.push_back(std::move(l));
    }
    return list;
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

        std::string active_id = active_device_id_.empty() && !dev_ids.empty() ? dev_ids.front() : active_device_id_;
        const NodeDb* act_db = mesh_service_.db_for(active_id);
        uint32_t my_node = act_db ? act_db->my_node_num() : 0;

        nlohmann::json status = {
            {"status", "online"},
            {"version", "fmesh-cli 1.0"},
            {"active_device", active_id},
            {"my_node_num", my_node},
            {"my_node_id", node_num_to_id(my_node)},
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
            auto rt = mesh_service_.find_device_runtime(id);
            bool connected = rt && ((rt->client && rt->client->is_connected()) || (rt->stream && rt->stream->is_connected()));
            std::string disp_name = mesh_service_.display_name_for(id);
            std::string name = rt ? (!rt->spec.name.empty() ? rt->spec.name : (!rt->my_long_name.empty() ? rt->my_long_name : disp_name)) : disp_name;
            std::string mac = rt ? (!rt->spec.address.empty() ? rt->spec.address : rt->id) : id;
            arr.push_back({
                {"id", id},
                {"display_name", disp_name},
                {"name", name},
                {"mac", mac},
                {"connected", connected},
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

    // GET /api/links
    server_.get("/api/links", [this](const HttpRequest& req) {
        std::string dev_id = req.get_query("device");
        auto links = get_links(dev_id);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& l : links) {
            arr.push_back(rf_link_to_json(l));
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

    // GET /api/telemetry
    server_.get("/api/telemetry", [this](const HttpRequest& req) {
        std::string node_str = req.get_query("node_num");
        std::string since_str = req.get_query("since", "0");
        std::string before_str = req.get_query("before", "0");
        std::string limit_str = req.get_query("limit", "500");

        uint64_t since_ts = 0;
        uint64_t before_ts = 0;
        int limit = 500;
        try { since_ts = std::stoull(since_str); } catch (...) {}
        try { before_ts = std::stoull(before_str); } catch (...) {}
        try { limit = std::stoi(limit_str); } catch (...) {}
        if (limit > 5000) limit = 5000;
        if (limit <= 0) limit = 500;

        std::vector<Database::TelemetryRow> rows;
        if (!node_str.empty()) {
            uint32_t node_num = 0;
            if (parse_node_id(node_str, node_num)) {
                rows = mesh_service_.database().get_node_telemetry(node_num, since_ts, before_ts, limit);
            }
        } else {
            rows = mesh_service_.database().get_recent_telemetry(since_ts, limit);
        }

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& r : rows) {
            arr.push_back(telemetry_to_json(r));
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
        if (msgs.empty() && !w.device.empty()) {
            WindowKey any_dev = w;
            any_dev.device = "";
            msgs = mesh_service_.database().get_messages_paginated(any_dev, limit, offset);
        }
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
        if (windows.empty() && !dev.empty()) {
            windows = mesh_service_.database().get_all_windows("");
        }
        const NodeDb* db = mesh_service_.db_for(dev);
        nlohmann::json ch_arr = nlohmann::json::array();
        nlohmann::json dm_arr = nlohmann::json::array();
        std::set<uint32_t> seen_channels;
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
                seen_channels.insert(w.target);
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
        if (db) {
            for (const auto& ch : db->channels()) {
                if (ch.role != "DISABLED" && seen_channels.find(ch.index) == seen_channels.end()) {
                    ch_arr.push_back({
                        {"index", ch.index},
                        {"name", ch.name}
                    });
                    seen_channels.insert(ch.index);
                }
            }
        }
        if (seen_channels.find(0) == seen_channels.end()) {
            ch_arr.push_back({
                {"index", 0},
                {"name", "Primary"}
            });
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
            uint32_t reply_id = j.value("reply_id", 0);
            uint32_t emoji = j.value("emoji", 0);

            if (text.empty() && emoji == 0) {
                return HttpResponse::bad_request("Empty message text");
            }
            if (text.empty() && emoji != 0) {
                if (emoji > 1) {
                    uint32_t cp = emoji;
                    if (cp <= 0x7F) {
                        text += static_cast<char>(cp);
                    } else if (cp <= 0x7FF) {
                        text += static_cast<char>(0xC0 | ((cp >> 6) & 0x1F));
                        text += static_cast<char>(0x80 | (cp & 0x3F));
                    } else if (cp <= 0xFFFF) {
                        text += static_cast<char>(0xE0 | ((cp >> 12) & 0x0F));
                        text += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        text += static_cast<char>(0x80 | (cp & 0x3F));
                    } else if (cp <= 0x10FFFF) {
                        text += static_cast<char>(0xF0 | ((cp >> 18) & 0x07));
                        text += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                        text += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        text += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                } else if (emoji == 1) {
                    text = "👍";
                }
            }

            uint32_t packet_id = mesh_service_.send_text(device, to_node, channel_idx, text, want_ack, reply_id, emoji);
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
            device = active_device_id_;
            if (device.empty()) {
                auto ids = mesh_service_.device_ids();
                if (!ids.empty()) device = ids.front();
            }
        }

        auto lines = mesh_service_.config_lines_for(device);
        nlohmann::json arr = nlohmann::json::array();
        nlohmann::json sections = nlohmann::json::object();
        std::string current_section = "general";

        for (const auto& line : lines) {
            if (line.rfind("--- ", 0) == 0) {
                // Parse section header e.g. "--- LoRa ---" or "--- Module: MQTT ---"
                std::string sec = line;
                while (sec.rfind("---", 0) == 0) sec = sec.substr(3);
                while (!sec.empty() && sec.back() == '-') sec.pop_back();
                while (!sec.empty() && sec.front() == ' ') sec.erase(sec.begin());
                while (!sec.empty() && sec.back() == ' ') sec.pop_back();
                if (sec.rfind("Module: ", 0) == 0) sec = sec.substr(8);
                std::string sec_lower = sec;
                std::transform(sec_lower.begin(), sec_lower.end(), sec_lower.begin(), [](unsigned char c){ return std::tolower(c); });
                current_section = sec_lower;
                if (!sections.contains(current_section)) {
                    sections[current_section] = nlohmann::json::array();
                }
                continue;
            }

            size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string k = line.substr(0, eq);
                std::string v = line.substr(eq + 1);
                while (!k.empty() && k.back() == ' ') k.pop_back();
                while (!v.empty() && v.front() == ' ') v.erase(v.begin());

                std::string item_sec = current_section;
                auto dot = k.find('.');
                if (dot != std::string::npos) {
                    item_sec = k.substr(0, dot);
                }

                std::string type = "string";
                if (v == "ON" || v == "OFF" || v == "true" || v == "false") {
                    type = "boolean";
                } else if (!v.empty() && std::all_of(v.begin(), v.end(), [](char c){ return std::isdigit(static_cast<unsigned char>(c)) || c == '-'; })) {
                    type = "number";
                }

                nlohmann::json item = {
                    {"key", k},
                    {"value", v},
                    {"section", item_sec},
                    {"type", type}
                };
                arr.push_back(item);
                if (!sections.contains(item_sec)) {
                    sections[item_sec] = nlohmann::json::array();
                }
                sections[item_sec].push_back(item);
            }
        }
        return HttpResponse::json(200, {
            {"device", device},
            {"config", arr},
            {"sections", sections}
        });
    });

    // POST /api/config
    server_.post("/api/config", [this](const HttpRequest& req) {
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

            // Check if batch update
            if (j.contains("settings")) {
                const auto& settings = j["settings"];
                nlohmann::json updated = nlohmann::json::array();
                bool all_ok = true;
                if (settings.is_object()) {
                    for (auto& [k, v] : settings.items()) {
                        std::string val_str;
                        if (v.is_boolean()) val_str = v.get<bool>() ? "true" : "false";
                        else if (v.is_number()) val_str = v.dump();
                        else if (v.is_string()) val_str = v.get<std::string>();
                        else val_str = v.dump();

                        bool ok = mesh_service_.set_config(device, k, val_str);
                        updated.push_back({{"key", k}, {"value", val_str}, {"success", ok}});
                        if (!ok) all_ok = false;
                    }
                } else if (settings.is_array()) {
                    for (const auto& item : settings) {
                        std::string k = item.value("key", "");
                        std::string v = item.value("value", "");
                        if (!k.empty()) {
                            bool ok = mesh_service_.set_config(device, k, v);
                            updated.push_back({{"key", k}, {"value", v}, {"success", ok}});
                            if (!ok) all_ok = false;
                        }
                    }
                }
                return HttpResponse::json(200, {{"success", all_ok}, {"device", device}, {"updated", updated}});
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

    // GET /api/stats
    server_.get("/api/stats", [this](const HttpRequest& req) {
        std::string range = req.get_query("range", "1d");
        uint64_t now_ts = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        uint64_t since_ts = 0;
        if (range == "1h") {
            since_ts = (now_ts > 3600) ? (now_ts - 3600) : 0;
        } else if (range == "6h") {
            since_ts = (now_ts > 21600) ? (now_ts - 21600) : 0;
        } else if (range == "1d") {
            since_ts = (now_ts > 86400) ? (now_ts - 86400) : 0;
        } else if (range == "7d") {
            since_ts = (now_ts > 604800) ? (now_ts - 604800) : 0;
        } else if (range == "all") {
            since_ts = 0;
        } else {
            range = "1d";
            since_ts = (now_ts > 86400) ? (now_ts - 86400) : 0;
        }

        uint32_t filter_node = 0;
        std::string node_param = req.get_query("node", req.get_query("node_num", ""));
        if (!node_param.empty()) {
            try {
                if (node_param.front() == '!') {
                    filter_node = static_cast<uint32_t>(std::stoul(node_param.substr(1), nullptr, 16));
                } else if (node_param.rfind("0x", 0) == 0 || node_param.rfind("0X", 0) == 0) {
                    filter_node = static_cast<uint32_t>(std::stoul(node_param, nullptr, 16));
                } else {
                    filter_node = static_cast<uint32_t>(std::stoul(node_param, nullptr, 10));
                }
            } catch (...) {}
        }

        auto stats = mesh_service_.database().get_stats(since_ts, filter_node);

        nlohmann::json j;
        j["range"] = range;
        j["since_ts"] = since_ts;
        j["node_filter"] = filter_node;
        j["metrics"] = {
            {"total_packets", stats.total_packets},
            {"active_nodes", stats.active_nodes},
            {"msg_count", stats.msg_count},
            {"telemetry_count", stats.telemetry_count},
            {"position_count", stats.position_count},
            {"ack_count", stats.ack_count},
            {"traceroute_count", stats.traceroute_count},
            {"nodeinfo_count", stats.nodeinfo_count},
            {"other_count", stats.other_count},
            {"broadcast_count", stats.broadcast_count},
            {"unicast_count", stats.unicast_count},
            {"avg_snr", stats.avg_snr}
        };

        nlohmann::json ports_j = nlohmann::json::object();
        for (const auto& [k, v] : stats.port_distribution) {
            ports_j[k] = v;
        }
        j["ports"] = ports_j;

        nlohmann::json chan_j = nlohmann::json::object();
        for (const auto& [k, v] : stats.channel_distribution) {
            chan_j[std::to_string(k)] = v;
        }
        j["channels"] = chan_j;

        nlohmann::json timeline_arr = nlohmann::json::array();
        for (const auto& b : stats.timeline) {
            timeline_arr.push_back({
                {"ts", b.ts},
                {"count", b.count},
                {"msg_count", b.msg_count},
                {"telemetry_count", b.telemetry_count},
                {"pos_count", b.pos_count},
                {"other_count", b.other_count}
            });
        }
        j["timeline"] = timeline_arr;

        nlohmann::json nodes_arr = nlohmann::json::array();
        for (const auto& ns : stats.per_node_stats) {
            auto opt_n = mesh_service_.find_node(ns.node_num);
            std::string nid = node_num_to_id(ns.node_num);
            std::string long_name = nid;
            std::string short_name = "";
            std::string hw_model = "";
            std::string role = "";
            if (opt_n) {
                if (!opt_n->node_id.empty()) nid = opt_n->node_id;
                if (!opt_n->long_name.empty()) long_name = opt_n->long_name;
                short_name = opt_n->short_name;
                hw_model = opt_n->hw_model;
                role = opt_n->role;
            }
            bool is_local = mesh_service_.is_my_node(ns.node_num);

            nodes_arr.push_back({
                {"node_num", ns.node_num},
                {"node_id", nid},
                {"long_name", long_name},
                {"short_name", short_name},
                {"hw_model", hw_model},
                {"role", role},
                {"is_local", is_local},
                {"packet_count", ns.packet_count},
                {"msg_count", ns.msg_count},
                {"telemetry_count", ns.telemetry_count},
                {"pos_count", ns.pos_count},
                {"ack_count", ns.ack_count},
                {"avg_snr", ns.avg_snr},
                {"last_seen", ns.last_seen}
            });
        }
        j["nodes"] = nodes_arr;

        return HttpResponse::json(200, j);
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

            const NodeDb* db = mesh_service_.db_for(e.device);
            uint32_t my_node = db ? db->my_node_num() : 0;
            bool is_local = (my_node != 0 && e.node.node_num == my_node) || mesh_service_.is_my_node(e.node.node_num);

            if (!is_local && (e.is_new || e.node.battery_level || e.node.voltage)) {
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

            if (e.node.hops_away.has_value() && *e.node.hops_away == 0) {
                if (my_node != 0 && e.node.node_num != my_node && !is_local) {
                    uint64_t ts = e.node.last_heard.value_or(static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count()));
                    update_link(e.device, my_node, e.node.node_num, e.node.snr.value_or(0.0f), "direct", ts);
                }
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

            const NodeDb* db = mesh_service_.db_for(e.device);
            uint32_t my_node = db ? db->my_node_num() : 0;
            bool is_local = (my_node != 0 && e.from_node == my_node) || mesh_service_.is_my_node(e.from_node);

            if (!is_local) {
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
        }
        else if constexpr (std::is_same_v<T, EvTextReceived>) {
            uint32_t hops = (e.hop_start > e.hop_limit) ? (e.hop_start - e.hop_limit) : 0;
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
                {"hop_start", e.hop_start},
                {"hop_limit", e.hop_limit},
                {"hops", hops},
                {"relay_node", e.relay_node},
                {"broadcast", e.broadcast}
            };
            server_.broadcast_sse("message_received", data.dump());

            const NodeDb* db = mesh_service_.db_for(e.device);
            uint32_t my_node = db ? db->my_node_num() : 0;
            bool is_local = (my_node != 0 && e.from_node == my_node) || mesh_service_.is_my_node(e.from_node);

            uint64_t rx_ts = e.rx_time ? e.rx_time : static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());

            if (!is_local) {
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
                act.ts = rx_ts;
                record_and_broadcast_activity(act);
            }

            if (e.hop_start > 0 && e.hop_start == e.hop_limit) {
                if (my_node != 0 && e.from_node != my_node && !is_local) {
                    update_link(e.device, my_node, e.from_node, e.rx_snr, "direct", rx_ts);
                }
            }
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

            // Update verified RF links along the traceroute path
            const NodeDb* db = mesh_service_.db_for(e.device);
            uint32_t my_node = db ? db->my_node_num() : 0;

            // Forward path: my_node -> route[0] -> ... -> from_node
            std::vector<uint32_t> fwd;
            if (my_node != 0) fwd.push_back(my_node);
            for (uint32_t hop : e.route) fwd.push_back(hop);
            if (e.from_node != 0 && (fwd.empty() || fwd.back() != e.from_node)) {
                fwd.push_back(e.from_node);
            }
            for (size_t i = 0; i + 1 < fwd.size(); ++i) {
                float snr = (i < e.snr_towards.size()) ? e.snr_towards[i] : 0.0f;
                update_link(e.device, fwd[i], fwd[i+1], snr, "traceroute", act.ts);
            }

            // Return path: from_node -> route_back[0] -> ... -> my_node
            if (!e.route_back.empty()) {
                std::vector<uint32_t> bck;
                if (e.from_node != 0) bck.push_back(e.from_node);
                for (uint32_t hop : e.route_back) bck.push_back(hop);
                if (my_node != 0 && (bck.empty() || bck.back() != my_node)) {
                    bck.push_back(my_node);
                }
                for (size_t i = 0; i + 1 < bck.size(); ++i) {
                    float snr = (i < e.snr_back.size()) ? e.snr_back[i] : 0.0f;
                    update_link(e.device, bck[i], bck[i+1], snr, "traceroute", act.ts);
                }
            }
        }
        else if constexpr (std::is_same_v<T, EvNeighborInfoReceived>) {
            uint64_t now_ts = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            for (const auto& nb : e.neighbors) {
                update_link(e.device, e.node_num, nb.node_id, nb.rx_snr, "neighbor_info",
                            nb.rx_time != 0 ? nb.rx_time : now_ts);
            }
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
