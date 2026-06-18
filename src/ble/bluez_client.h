#pragma once

#include "mesh/event.h"
#include "util/event_loop.h"

#ifndef _WIN32
#include <sdbus-c++/sdbus-c++.h>
#endif

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "ble_client.h"

namespace meshcli {

#ifndef _WIN32
// A single BLE connection to one Meshtastic device, built on BlueZ over
// sdbus-c++. Owns its own D-Bus connection + event-loop thread. Decoded
// FromRadio bytes are turned into MeshEvents and pushed to the supplied queue
// (with the eventfd notified so the UI can wake).
//
// GATT characteristics used (from the Meshtastic BLE spec):
//   Service  : 6ba1b218-15a8-461f-9fa8-5dcae273eafd
//   TORADIO  : f75c76d2-129e-4dad-a1dd-7866124401e7   (write)
//   FROMRADIO: 2c55e69e-4993-11ed-b878-0242ac120002   (read/poll)
//   FROMNUM  : ed9da18c-a800-4f66-a670-aa7547e34453   (notify)
//   LOGRADIO : 5a3d6e49-06e6-4423-9944-e9de8cdf9547   (notify, optional)
class BluezClient : public BleClient {
public:
    using EventSink = std::function<void(MeshEvent)>;

    BluezClient(BleDeviceSpec spec, EventSink sink);
    ~BluezClient() override;

    BluezClient(const BluezClient&) = delete;
    BluezClient& operator=(const BluezClient&) = delete;

    // Start the background D-Bus loop. Returns the assigned DeviceId (the
    // BlueZ object path of the device). If `pair` is true and the device is
    // not yet paired, the agent will be used to pair it.
    // Returns empty string on failure (an EvError is also pushed).
    std::string start(bool pair) override;

    // Stop the connection and join the background thread.
    void stop() override;

    // Send raw ToRadio bytes by writing the TORADIO characteristic. Safe to
    // call from any thread. Returns false if not connected.
    bool send_to_radio(const std::string& bytes) override;

    [[nodiscard]] std::string device_id() const override { return device_id_; }
    [[nodiscard]] bool is_connected() const override { return connected_; }

private:
    BleDeviceSpec spec_;
    EventSink sink_;
    std::unique_ptr<sdbus::IConnection> conn_;
    std::thread loop_thread_;
    std::thread init_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};

    std::string adapter_path_;     // /org/bluez/hci0
    std::string device_path_;      // /org/bluez/hci0/dev_XX...
    std::string device_id_;        // same as device_path_, the DeviceId
    std::string toradio_path_;
    std::string fromradio_path_;
    std::string fromnum_path_;
    std::string logradio_path_;

    // Protects proxy/connection access across the event-loop thread and
    // the main thread (send_to_radio is called from the UI thread).
    mutable std::mutex proxy_mu_;

    // Proxies kept alive for the lifetime of the connection (signal handlers
    // are registered on the notify proxies; the others are cached for speed).
    std::unique_ptr<sdbus::IProxy> toradio_proxy_;
    std::unique_ptr<sdbus::IProxy> fromradio_proxy_;
    std::unique_ptr<sdbus::IProxy> fromnum_proxy_;
    std::unique_ptr<sdbus::IProxy> logradio_proxy_;

    // Pairing agent (only used during the pair flow).
    std::unique_ptr<class BluezAgent> agent_;

    // --- internal helpers ---
    void run_connect_flow();  // synchronous scan/pair/connect/GATT setup
    void initial_handshake();  // send want_config + drain FROMRADIO (event loop active)
    void emit(MeshEvent ev);
    void emit_error(std::string msg);
    // Emit a raw-packet hex dump alongside the decoded event.
    void emit_raw(const std::string& fromradio_bytes);

    bool find_adapter();
    // Scan for up to `timeout_s` seconds; returns device object path.
    std::optional<std::string> scan_for_device(int timeout_s);
    bool ensure_paired_and_connected(bool do_pair);
    bool discover_gatt();
    // Subscribe to FROMNUM (and LOGRADIO if present). On FROMNUM, drain FROMRADIO.
    void subscribe_notifications();
    void drain_from_radio();
    // Read a GATT char value (bytes). Thread-safe via proxy_mu_.
    std::string read_char(const std::string& path);
    // Internal helpers: caller must hold proxy_mu_.
    std::string read_char_locked(const std::string& path);
    void write_char_locked(const std::string& path, const std::string& bytes);
    void write_char(const std::string& path, const std::string& bytes);
};
#endif

} // namespace meshcli
