#include "config.h"

#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

namespace meshcli {

namespace {
std::string home_dir() {
    const char* h = std::getenv("HOME");
    return h ? std::string(h) : "/tmp";
}
std::string default_data_dir() { return home_dir() + "/.local/share/mesh-cli"; }
} // namespace

bool parse_args(int argc, char** argv, AppConfig& out) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char*& v) -> bool {
            if (i + 1 >= argc) return false;
            v = argv[++i];
            return true;
        };
        const char* v = nullptr;
        if (a == "--help" || a == "-h") {
            std::printf(
                "mesh-cli - irssi-style Meshtastic terminal client\n\n"
                "Usage: mesh-cli [options] [pair]\n\n"
                "Options:\n"
                "  --name <name>     BLE device name to connect to (default: %s)\n"
                "  --addr <mac>      explicit BLE MAC address (skips scan)\n"
                "  --pin <pin>       pairing PIN (default: %s)\n"
                "  --tcp <host:port>  connect via TCP (e.g. 192.168.1.50:4403)\n"
                "  --serial <path>    connect via serial port (e.g. /dev/ttyUSB0)\n"
                "  --serial-baud <N>  serial baud rate (default: 115200)\n"
                "  --db <path>       SQLite database path\n"
                "  --log <path>      log file path\n"
                "  --debug           verbose logging\n"
                "  --scan            scan for BLE devices and exit\n"
                "  --headless        connect + dump events to log, no TUI (for testing)\n"
                "  pair              shorthand for --pair: pair then connect\n"
                "  -h, --help        show this help\n",
                out.device_name.c_str(), out.pin.c_str());
            return false;
        } else if (a == "--name" && need(v)) {
            out.device_addr.clear();  // name takes precedence
            out.device_name = v;
        } else if (a == "--addr" && need(v)) {
            out.device_addr = v;
        } else if (a == "--pin" && need(v)) {
            out.pin = v;
        } else if (a == "--tcp" && need(v)) {
            out.tcp_host = v;
        } else if (a == "--serial" && need(v)) {
            out.serial_port = v;
        } else if (a == "--serial-baud" && need(v)) {
            out.serial_baud = std::atoi(v);
        } else if (a == "--db" && need(v)) {
            out.db_path = v;
        } else if (a == "--log" && need(v)) {
            out.log_path = v;
        } else if (a == "--debug") {
            out.log_debug = true;
        } else if (a == "--scan") {
            out.list_only = true;
        } else if (a == "--headless") {
            out.headless = true;
        } else if (a == "pair") {
            out.pair = true;
        } else {
            std::fprintf(stderr, "unknown argument: %s (try --help)\n", a.c_str());
            return false;
        }
    }
    return true;
}

void finalize_paths(AppConfig& c) {
    std::string dir = default_data_dir();
    ::mkdir(dir.c_str(), 0755);
    if (c.db_path.empty())  c.db_path  = dir + "/mesh.db";
    if (c.log_path.empty()) c.log_path = dir + "/mesh-cli.log";
}

} // namespace meshcli
