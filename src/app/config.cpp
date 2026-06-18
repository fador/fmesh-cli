#include "config.h"

#include <cstdlib>
#include <cstring>
#include <sstream>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

namespace meshcli {

namespace {
std::string home_dir() {
    const char* h = std::getenv("HOME");
    return h ? std::string(h) : "/tmp";
}
std::string default_data_dir() { return home_dir() + "/.local/share/mesh-cli"; }
} // namespace

bool parse_device_spec(const std::string& spec, BleDeviceSpec& out) {
    // Format: <type>:<arg1>[:<arg2>[:<arg3>]]
    //   ble:<name>[:<pin>]
    //   addr:<mac>[:<pin>]
    //   tcp:<host>[:<port>]
    //   serial:<path>[:<baud>]
    auto colon1 = spec.find(':');
    if (colon1 == std::string::npos) return false;
    std::string type = spec.substr(0, colon1);
    std::string rest = spec.substr(colon1 + 1);

    if (type == "ble") {
        auto colon2 = rest.find(':');
        if (colon2 != std::string::npos) {
            out.name = rest.substr(0, colon2);
            out.pin = rest.substr(colon2 + 1);
        } else {
            out.name = rest;
        }
        return !out.name.empty();
    } else if (type == "addr") {
        auto colon2 = rest.find(':');
        if (colon2 != std::string::npos) {
            out.address = rest.substr(0, colon2);
            out.pin = rest.substr(colon2 + 1);
        } else {
            out.address = rest;
        }
        return !out.address.empty();
    } else if (type == "tcp") {
        out.tcp_host = rest;
        return !out.tcp_host.empty();
    } else if (type == "serial") {
        auto colon2 = rest.find(':');
        if (colon2 != std::string::npos) {
            out.serial_port = rest.substr(0, colon2);
            out.serial_baud = std::atoi(rest.substr(colon2 + 1).c_str());
            if (out.serial_baud <= 0) out.serial_baud = 115200;
        } else {
            out.serial_port = rest;
        }
        return !out.serial_port.empty();
    }
    return false;
}

bool parse_args(int argc, char** argv, AppConfig& out) {
    bool used_legacy = false;
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
                "Connection options:\n"
                "  --name <name>     BLE device name (default: %s)\n"
                "  --addr <mac>      explicit BLE MAC address\n"
                "  --tcp <host:port> TCP connection (port 4403 default)\n"
                "  --serial <path>   serial port (e.g. /dev/ttyUSB0)\n"
                "  --serial-baud <N> serial baud rate (default: 115200)\n"
                "  --device <spec>   connect to a device (can be used multiple times)\n"
                "                      spec formats:\n"
                "                        ble:<name>[:<pin>]\n"
                "                        addr:<mac>[:<pin>]\n"
                "                        tcp:<host>[:<port>]\n"
                "                        serial:<path>[:<baud>]\n"
                "  pair              pair with BLE device before connecting\n"
                "\n"
                "Other options:\n"
                "  --pin <pin>       BLE pairing PIN (default: %s)\n"
                "  --db <path>       SQLite database path\n"
                "  --log <path>      log file path\n"
                "  --debug           verbose logging\n"
                "  --scan            scan for BLE devices and exit\n"
                "  --headless        connect + dump events to log, no TUI\n"
                "  -h, --help        show this help\n\n"
                "TUI commands: /help /list /nodes /query /msg /channel /window\n"
                "              /close /clear /info /me /reconnect /config /whois\n"
                "              /raw /stats /scan /connect /disconnect /device /quit\n",
                out.device_name.c_str(), out.pin.c_str());
            return false;
        } else if (a == "--device" && need(v)) {
            BleDeviceSpec spec;
            spec.pin = out.pin;  // inherit global pin default
            if (!parse_device_spec(v, spec)) {
                std::fprintf(stderr, "invalid --device spec: \"%s\"\n", v);
                return false;
            }
            out.devices.push_back(std::move(spec));
        } else if (a == "--name" && need(v)) {
            used_legacy = true;
            out.device_addr.clear();
            out.device_name = v;
        } else if (a == "--addr" && need(v)) {
            used_legacy = true;
            out.device_addr = v;
        } else if (a == "--pin" && need(v)) {
            out.pin = v;
        } else if (a == "--tcp" && need(v)) {
            used_legacy = true;
            out.tcp_host = v;
        } else if (a == "--serial" && need(v)) {
            used_legacy = true;
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

    // If legacy flags were used and no --device flags, build a single spec.
    if (out.devices.empty() && used_legacy) {
        BleDeviceSpec spec;
        spec.name = out.device_name;
        spec.address = out.device_addr;
        spec.pin = out.pin;
        spec.tcp_host = out.tcp_host;
        spec.serial_port = out.serial_port;
        spec.serial_baud = out.serial_baud;
        out.devices.push_back(std::move(spec));
    }

    return true;
}

void finalize_paths(AppConfig& c) {
    std::string dir = default_data_dir();
    #ifdef _WIN32
    ::_mkdir(dir.c_str());
#else
    ::mkdir(dir.c_str(), 0755);
#endif
    if (c.db_path.empty())  c.db_path  = dir + "/mesh.db";
    if (c.log_path.empty()) c.log_path = dir + "/mesh-cli.log";
    if (c.history_path.empty()) c.history_path = dir + "/history";
}

} // namespace meshcli
