#pragma once

#include "mesh/event.h"
#include "util/event_loop.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <mutex>

namespace meshcli {

#ifdef ENABLE_MESH_NET

class StreamServer {
public:
    using EventSink = std::function<void(MeshEvent)>;

    StreamServer(int port, std::string user, std::string password, EventSink sink);
    ~StreamServer();

    StreamServer(const StreamServer&) = delete;
    StreamServer& operator=(const StreamServer&) = delete;

    // Start the server loop. Returns true on success.
    bool start();

    // Stop the server loop and close all connections.
    void stop();

    // Broadcast a ToRadio or FromRadio message to all connected and authenticated clients.
    // Thread-safe.
    void broadcast(const std::string& bytes);

private:
    void accept_loop();
    void emit(MeshEvent ev);
    void emit_error(std::string msg);

    int port_;
    std::string user_;
    std::string password_;
    EventSink sink_;
    
    intptr_t server_fd_{-1};
    std::thread accept_thread_;
    std::atomic<bool> running_{false};

    struct ClientConn;
    std::vector<std::shared_ptr<ClientConn>> clients_;
    std::mutex clients_mu_;

    void remove_client(std::shared_ptr<ClientConn> client);
};

#else

// Dummy implementation when mesh net is disabled
class StreamServer {
public:
    using EventSink = std::function<void(MeshEvent)>;
    StreamServer(int, std::string, std::string, EventSink) {}
    ~StreamServer() {}
    bool start() { return false; }
    void stop() {}
    void broadcast(const std::string&) {}
};

#endif

} // namespace meshcli
