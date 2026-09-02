#include "node_db.h"

#include <algorithm>
#include <cctype>

namespace meshcli {

void NodeDb::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    nodes_.clear();
    channels_.clear();
    my_node_num_ = 0;
}

void NodeDb::upsert_node(Node n) {
    if (n.node_id.empty()) n.node_id = node_num_to_id(n.node_num);
    std::lock_guard<std::mutex> lock(mu_);
    auto it = nodes_.find(n.node_num);
    if (it != nodes_.end()) {
        if (!n.long_name.empty()) it->second.long_name = std::move(n.long_name);
        if (!n.short_name.empty()) it->second.short_name = std::move(n.short_name);
        if (!n.hw_model.empty()) it->second.hw_model = std::move(n.hw_model);
        if (!n.role.empty()) it->second.role = std::move(n.role);
        if (n.battery_level.has_value()) it->second.battery_level = n.battery_level;
        if (n.voltage.has_value()) it->second.voltage = n.voltage;
        if (n.channel_util.has_value()) it->second.channel_util = n.channel_util;
        if (n.air_util_tx.has_value()) it->second.air_util_tx = n.air_util_tx;
        if (n.uptime_seconds.has_value()) it->second.uptime_seconds = n.uptime_seconds;
        if (n.temperature.has_value()) it->second.temperature = n.temperature;
        if (n.relative_humidity.has_value()) it->second.relative_humidity = n.relative_humidity;
        if (n.barometric_pressure.has_value()) it->second.barometric_pressure = n.barometric_pressure;
        if (n.gas_resistance.has_value()) it->second.gas_resistance = n.gas_resistance;
        if (n.iaq.has_value()) it->second.iaq = n.iaq;
        if (n.pm25.has_value()) it->second.pm25 = n.pm25;
        if (n.co2.has_value()) it->second.co2 = n.co2;
        if (n.current.has_value()) it->second.current = n.current;
        if (n.snr.has_value()) it->second.snr = n.snr;
        if (n.hops_away.has_value()) it->second.hops_away = n.hops_away;
        if (n.last_heard.has_value()) it->second.last_heard = n.last_heard;
        if (n.latitude.has_value()) it->second.latitude = n.latitude;
        if (n.longitude.has_value()) it->second.longitude = n.longitude;
        if (n.altitude.has_value()) it->second.altitude = n.altitude;
        if (n.has_public_key) {
            it->second.has_public_key = true;
            it->second.public_key = std::move(n.public_key);
        }
        it->second.is_favorite = n.is_favorite || it->second.is_favorite;
        it->second.is_muted = n.is_muted || it->second.is_muted;
        it->second.is_key_verified = n.is_key_verified || it->second.is_key_verified;
    } else {
        nodes_[n.node_num] = std::move(n);
    }
}

void NodeDb::update_position(uint32_t node_num, double lat, double lon, int32_t alt) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = nodes_.find(node_num);
    if (it != nodes_.end()) {
        it->second.latitude = lat;
        it->second.longitude = lon;
        it->second.altitude = alt;
    } else {
        Node n;
        n.node_num = node_num;
        n.node_id = node_num_to_id(node_num);
        n.latitude = lat;
        n.longitude = lon;
        n.altitude = alt;
        nodes_[node_num] = std::move(n);
    }
}

void NodeDb::upsert_channel(Channel c) {
    std::lock_guard<std::mutex> lock(mu_);
    channels_[c.index] = std::move(c);
}

std::optional<Node> NodeDb::get(uint32_t node_num) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = nodes_.find(node_num);
    if (it == nodes_.end()) return std::nullopt;
    return it->second;
}

std::optional<Node> NodeDb::get_by_id(const std::string& id) const {
    uint32_t n;
    if (!parse_node_id(id, n)) return std::nullopt;
    return get(n);
}

std::vector<Node> NodeDb::all() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<Node> out;
    out.reserve(nodes_.size());
    for (const auto& [_, n] : nodes_) out.push_back(n);
    return out;
}

std::vector<Channel> NodeDb::channels() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<Channel> out;
    out.reserve(channels_.size());
    for (const auto& [_, c] : channels_) out.push_back(c);
    return out;
}

std::optional<Channel> NodeDb::channel(uint32_t idx) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = channels_.find(idx);
    if (it == channels_.end()) return std::nullopt;
    return it->second;
}

std::optional<Node> NodeDb::find_fuzzy(const std::string& query) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (nodes_.empty()) return std::nullopt;

    // Exact node id match first.
    uint32_t n;
    if (parse_node_id(query, n)) {
        auto it = nodes_.find(n);
        if (it != nodes_.end()) return it->second;
    }

    // Case-insensitive substring match against long/short name.
    std::string q = query;
    std::transform(q.begin(), q.end(), q.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    std::optional<Node> best;
    int best_score = 0;
    for (const auto& [_, node] : nodes_) {
        std::string ln = node.long_name, sn = node.short_name;
        std::transform(ln.begin(), ln.end(), ln.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        std::transform(sn.begin(), sn.end(), sn.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        int score = 0;
        if (ln == q || sn == q) score = 100;
        else if (ln.find(q) == 0) score = 80;
        else if (sn.find(q) == 0) score = 70;
        else if (ln.find(q) != std::string::npos) score = 50;
        else if (sn.find(q) != std::string::npos) score = 40;
        if (score > best_score) { best_score = score; best = node; }
    }
    return best;
}

} // namespace meshcli
