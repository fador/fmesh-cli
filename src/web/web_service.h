#pragma once

#include "http_server.h"
#include "mesh/event.h"

#include <deque>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

namespace meshcli {

class MeshService;

struct PacketActivity {
    std::string device;
    uint32_t from_node = 0;
    std::string from_id;
    uint32_t to_node = 0;
    std::string to_id;
    uint32_t packet_id = 0;
    uint32_t channel_idx = 0;
    std::string port_name;
    std::string summary;
    float rx_snr = 0.0f;
    int32_t rx_rssi = 0;
    uint32_t hop_limit = 0;
    uint32_t hop_start = 0;
    bool broadcast = false;
    std::vector<uint32_t> route;
    std::vector<float> snr_towards;
    uint64_t ts = 0;
};

struct RfLink {
    std::string device;
    uint32_t from_node = 0;
    std::string from_id;
    uint32_t to_node = 0;
    std::string to_id;
    float snr = 0.0f;
    std::string source; // "direct" | "traceroute" | "neighbor_info" | "packet"
    uint64_t last_heard = 0;
};

class WebService {
public:
    explicit WebService(MeshService& mesh_service);
    ~WebService();

    WebService(const WebService&) = delete;
    WebService& operator=(const WebService&) = delete;

    // Configure and start web server
    bool start(const std::string& host, int port, const std::string& web_root);

    // Stop web server
    void stop();

    [[nodiscard]] bool is_running() const { return server_.is_running(); }
    [[nodiscard]] int bound_port() const { return server_.bound_port(); }

    std::vector<PacketActivity> recent_packets() const;
    void record_and_broadcast_activity(const PacketActivity& act);

    std::vector<RfLink> get_links(const std::string& device) const;
    void update_link(const std::string& device, uint32_t from_node, uint32_t to_node,
                     float snr, const std::string& source, uint64_t last_heard = 0);

private:
    void register_routes();
    void on_mesh_event(const MeshEvent& ev);

    MeshService& mesh_service_;
    HttpServer server_;
    std::string active_device_id_;

    mutable std::mutex packets_mu_;
    std::deque<PacketActivity> recent_packets_;
    static constexpr size_t kMaxRecentPackets = 100;

    mutable std::mutex links_mu_;
    std::map<std::pair<uint32_t, uint32_t>, RfLink> links_;
};

} // namespace meshcli

