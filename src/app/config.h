#pragma once

#include "ble/bluez_client.h"

#include <string>
#include <vector>

namespace meshcli {

struct AppConfig {
    std::string device_name = "Fad3_0330";  // single-device backward compat
    std::string device_addr;                // single-device backward compat
    std::string pin = "123456";
    std::string tcp_host;              // single-device backward compat
    std::string serial_port;           // single-device backward compat
    int serial_baud = 115200;
    std::string db_path;
    std::string log_path;
    std::string history_path;
    bool pair = false;
    bool list_only = false;
    bool headless = false;
    bool log_debug = false;
    // Multi-device: explicit device specs from --device flags.
    std::vector<BleDeviceSpec> devices;
};

// Parse argv into an AppConfig. Returns false on parse error / --help.
bool parse_args(int argc, char** argv, AppConfig& out);

// Parse a --device spec string: "ble:name[:pin]" / "addr:mac[:pin]" /
// "tcp:host[:port]" / "serial:path[:baud]". Returns true on success.
bool parse_device_spec(const std::string& spec, BleDeviceSpec& out);

// Expand ~ in paths and fill defaults for empty fields.
void finalize_paths(AppConfig& c);

} // namespace meshcli
