#pragma once

#include <string>

namespace meshcli {

struct AppConfig {
    std::string device_name = "Fad3_0330";
    std::string device_addr;           // optional explicit MAC
    std::string pin = "123456";
    std::string tcp_host;              // "host:port" for TCP transport
    std::string serial_port;           // "/dev/ttyUSB0" for serial transport
    int serial_baud = 115200;
    std::string db_path;               // default filled in main
    std::string log_path;              // default filled in main
    bool pair = false;                 // run pairing agent
    bool list_only = false;            // scan + list, then exit
    bool headless = false;             // connect + dump events, no TUI
    bool log_debug = false;
};

// Parse argv into an AppConfig. Returns false on parse error / --help.
bool parse_args(int argc, char** argv, AppConfig& out);

// Expand ~ in paths and fill defaults for empty fields.
void finalize_paths(AppConfig& c);

} // namespace meshcli
