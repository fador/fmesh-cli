#include "config.h"

#include <cstdlib>
#include <ctime>
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
    if (h) return std::string(h);
#ifdef _WIN32
    const char* up = std::getenv("USERPROFILE");
    if (up) return std::string(up);
#endif
    return "/tmp";
}
std::string default_data_dir() { return home_dir() + "/.local/share/fmesh-cli"; }
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
    } else if (type == "mesh") {
        // format: mesh:<host>:<port>[:<user>:<password>]
        auto colon2 = rest.find(':');
        if (colon2 != std::string::npos) {
            out.mesh_host = rest.substr(0, colon2) + ":" + rest.substr(colon2 + 1);
            // Wait, we just keep the whole host:port in mesh_host for now.
            // Let's assume mesh_host is "host:port" and user/password are empty for now if not provided,
            // or we parse them out if there's an '@' or multiple colons.
            // Let's just use the whole string as mesh_host. If they want user/pass, they can use the UI wizard.
            out.mesh_host = rest;
        } else {
            out.mesh_host = rest;
        }
        return !out.mesh_host.empty();
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
                "fmesh-cli - irssi-style Meshtastic terminal client\n\n"
                "Usage: fmesh-cli [options] [pair]\n\n"
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
    if (c.log_path.empty()) c.log_path = dir + "/fmesh-cli.log";
    if (c.history_path.empty()) c.history_path = dir + "/history";
    if (c.config_path.empty()) c.config_path = dir + "/config.txt";

    // Load configuration to populate server fields
    load_config(c);
}

void load_config(AppConfig& c) {
    if (c.config_path.empty()) return;
    FILE* f = fopen(c.config_path.c_str(), "r");
    if (!f) {
        // First run, generate a random password
        const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        std::srand(static_cast<unsigned>(std::time(nullptr)));
        c.server_password.clear();
        for (int i = 0; i < 16; ++i) {
            c.server_password += charset[std::rand() % (sizeof(charset) - 1)];
        }
        save_config(c);
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        if (!s.empty() && s.back() == '\n') s.pop_back();
        auto eq = s.find('=');
        if (eq != std::string::npos) {
            std::string k = s.substr(0, eq);
            std::string v = s.substr(eq + 1);
            if (k == "server_mode") c.server_mode = (v == "1" || v == "true");
            else if (k == "server_port") c.server_port = std::atoi(v.c_str());
            else if (k == "server_user") c.server_user = v;
            else if (k == "server_password") c.server_password = v;
        }
    }
    fclose(f);
    
    // Ensure there is a password
    if (c.server_password.empty()) {
        const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        std::srand(static_cast<unsigned>(std::time(nullptr)));
        for (int i = 0; i < 16; ++i) {
            c.server_password += charset[std::rand() % (sizeof(charset) - 1)];
        }
        save_config(c);
    }
}

void save_config(const AppConfig& c) {
    if (c.config_path.empty()) return;
    FILE* f = fopen(c.config_path.c_str(), "w");
    if (!f) return;
    fprintf(f, "server_mode=%d\n", c.server_mode ? 1 : 0);
    fprintf(f, "server_port=%d\n", c.server_port);
    fprintf(f, "server_user=%s\n", c.server_user.c_str());
    fprintf(f, "server_password=%s\n", c.server_password.c_str());
    fclose(f);
}

} // namespace meshcli
