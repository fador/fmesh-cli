#pragma once

#include "http_server.h"
#include "mesh/event.h"

#include <memory>
#include <string>

namespace meshcli {

class MeshService;

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

private:
    void register_routes();
    void on_mesh_event(const MeshEvent& ev);

    MeshService& mesh_service_;
    HttpServer server_;
    std::string active_device_id_;
};

} // namespace meshcli
