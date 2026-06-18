// Stream transport for Meshtastic devices over TCP sockets or serial ports.
// Handles the 0x94 0xC3 <len_hi> <len_lo> framing used by the firmware.
// Provides the same event-sink interface as BluezClient so MeshService can
// use either transport uniformly.
#pragma once

#include "mesh/event.h"
#include "util/event_loop.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#ifdef ENABLE_MESH_NET
extern "C" {
    struct ssl_st;
    struct ssl_ctx_st;
}
#endif

namespace meshcli {

// Opens a TCP connection to host:port. Returns the connected fd, or -1.
intptr_t tcp_connect(const std::string& host, uint16_t port);

// Opens a serial port at `device` with the given baud rate.
// Returns the fd, or -1.
intptr_t serial_open(const std::string& device, int baud);

class StreamClient {
public:
    using EventSink = std::function<void(MeshEvent)>;

    // `fd` must be a connected, non-blocking file descriptor (socket or
    // serial port). `display_name` is shown in the UI (e.g. "tcp:host:port").
    // The client takes ownership of `fd` and will close it on stop().
    StreamClient(intptr_t fd, std::string display_name, EventSink sink);
    ~StreamClient();

    StreamClient(const StreamClient&) = delete;
    StreamClient& operator=(const StreamClient&) = delete;

    // Start the background read loop. Returns a device_id string for
    // the UI to reference. Never fails (the fd is already open).
    std::string start();

    // Stop the read loop, close the fd, join the thread.
    void stop();

    // Write a ToRadio message (with framing). Thread-safe.
    bool send_to_radio(const std::string& bytes, unsigned char marker = 0xC3);

    [[nodiscard]] std::string device_id() const { return device_id_; }
    [[nodiscard]] bool is_connected() const { return connected_; }

    // Frame a protobuf message with the 0x94 <marker> <len16> header.
    static std::string frame(const std::string& payload, unsigned char marker = 0xC3);

#ifdef ENABLE_MESH_NET
    // Enable TLS client mode with authentication
    void enable_tls(const std::string& user, const std::string& password);
#endif

private:
    void read_loop();        // runs on background thread
    void emit(MeshEvent ev);
    void emit_error(std::string msg);

    intptr_t fd_{-1};
    std::string display_name_;
    std::string device_id_;
    EventSink sink_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};

#ifdef ENABLE_MESH_NET
    bool use_tls_{false};
    std::string tls_user_;
    std::string tls_password_;
    ::ssl_st* ssl_{nullptr};
    ::ssl_ctx_st* ssl_ctx_{nullptr};
#endif
};

} // namespace meshcli
