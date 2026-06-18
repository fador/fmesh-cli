#pragma once

#include "mesh/event.h"
#include <functional>
#include <optional>
#include <string>

namespace meshcli {

struct BleDeviceSpec {
    std::string name;        // e.g. "Fad3_0330" (matched case-insensitively)
    std::string address;     // optional explicit MAC "AA:BB:CC:DD:EE:FF"
    std::string pin = "123456";
    // Stream transport (TCP/serial) — if set, BLE is skipped.
    std::string tcp_host;    // "host:port"
    std::string serial_port; // "/dev/ttyUSB0"
    int serial_baud = 115200;
};

// Abstract BLE client interface. Platform-specific implementations (e.g. BlueZ, WinRT)
// inherit from this and are instantiated by MeshService.
class BleClient {
public:
    using EventSink = std::function<void(MeshEvent)>;

    virtual ~BleClient() = default;

    // Start the background connection loop. Returns the assigned DeviceId.
    // If `pair` is true, the device is paired if necessary.
    // Returns empty string on failure.
    virtual std::string start(bool pair) = 0;

    // Stop the connection and join any background threads.
    virtual void stop() = 0;

    // Send raw ToRadio bytes to the device. Safe to call from any thread.
    // Returns false if not connected.
    virtual bool send_to_radio(const std::string& bytes) = 0;

    [[nodiscard]] virtual std::string device_id() const = 0;
    [[nodiscard]] virtual bool is_connected() const = 0;

    // Platform-specific static method for scanning BLE devices.
    // Starts an asynchronous scan that invokes `on_device` for each found device.
    // The callback may be invoked from a background thread.
    // To stop the scan, call stop_scan().
    static void scan_async(int timeout_s, std::function<void(const std::string& name, const std::string& address)> on_device, std::function<void()> on_complete);
    static void stop_scan();
};

} // namespace meshcli
