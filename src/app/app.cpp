#include "app.h"

#include "../tui/tui.h"
#include "config.h"
#include "mesh/event.h"
#include "mesh/mesh_service.h"
#include "mesh/node_db.h"
#include "util/event_loop.h"
#include "util/log.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <thread>

namespace meshcli {

namespace {

volatile std::sig_atomic_t g_got_sigint = 0;
void on_sigint(int) { g_got_sigint = 1; }

} // namespace

int run_app(int argc, char** argv, MeshService& service) {
    AppConfig cfg;
    if (!parse_args(argc, argv, cfg)) return 0;
    finalize_paths(cfg);

    Logger::instance().init(cfg.log_path, /*console=*/true,
                            cfg.log_debug ? LogLevel::Trace : LogLevel::Info);
    LOG_INFO() << "mesh-cli starting (device=" << cfg.device_name
               << " addr=" << (cfg.device_addr.empty() ? "(scan)" : cfg.device_addr)
               << " pair=" << cfg.pair << ")";

    if (!service.open_database(cfg.db_path)) {
        std::fprintf(stderr, "error: cannot open database at %s\n", cfg.db_path.c_str());
        return 1;
    }

    std::signal(SIGINT, on_sigint);
    std::signal(SIGTERM, on_sigint);

    // Set up the UI event channel BEFORE connecting, so connection events
    // (EvConnected / EvError) are captured for the TUI.
    ConcurrentQueue<MeshEvent> queue;
    EventFd wake;
    service.set_event_sink(&queue, &wake);

    // Connect to the device. The BLE handshake now runs synchronously so the
    // caller knows immediately whether the connection succeeded.
    BleDeviceSpec spec;
    spec.name = cfg.device_name;
    spec.address = cfg.device_addr;
    spec.pin = cfg.pin;
    spec.tcp_host = cfg.tcp_host;
    spec.serial_port = cfg.serial_port;
    spec.serial_baud = cfg.serial_baud;
    std::string device_id = service.connect_device(spec, cfg.pair);
    if (device_id.empty()) {
        // Drain any error events to stderr so the user sees why.
        for (auto& ev : queue.drain_all()) {
            std::visit([](const auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, EvError>)
                    std::fprintf(stderr, "error: %s\n", e.message.c_str());
                else if constexpr (std::is_same_v<T, EvDisconnected>)
                    std::fprintf(stderr, "disconnected: %s\n", e.reason.c_str());
            }, ev);
        }
        if (cfg.list_only) return 0;
        return 1;
    }

    if (cfg.list_only) {
        std::printf("connected to %s (%s)\n", spec.name.c_str(), device_id.c_str());
        service.disconnect_all();
        return 0;
    }

    if (cfg.headless) {
        // Headless mode: connect and log all events for 15 seconds, then exit.
        LOG_INFO() << "headless mode: logging events for 15 seconds...";
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < std::chrono::seconds(15)) {
            wake.drain();
            for (auto& ev : queue.drain_all()) {
                std::visit([](const auto& e) {
                    using T = std::decay_t<decltype(e)>;
                    if constexpr (std::is_same_v<T, EvConnected>)
                        LOG_INFO() << "[event] Connected: " << e.display_name;
                    else if constexpr (std::is_same_v<T, EvMyInfo>)
                        LOG_INFO() << "[event] MyInfo: node=" << node_num_to_id(e.my_node_num);
                    else if constexpr (std::is_same_v<T, EvMetadata>)
                        LOG_INFO() << "[event] Metadata: fw=" << e.firmware_version << " hw=" << e.hw_model;
                    else if constexpr (std::is_same_v<T, EvConfigComplete>)
                        LOG_INFO() << "[event] ConfigComplete rebooted=" << e.rebooted;
                    else if constexpr (std::is_same_v<T, EvNodeUpdated>)
                        LOG_INFO() << "[event] Node: " << e.node.long_name << " (" << e.node.node_id << ")";
                    else if constexpr (std::is_same_v<T, EvChannelUpdated>)
                        LOG_INFO() << "[event] Channel " << e.channel.index << ": " << e.channel.name << " [" << e.channel.role << "]";
                    else if constexpr (std::is_same_v<T, EvTextReceived>)
                        LOG_INFO() << "[event] Text from " << node_num_to_id(e.from_node) << " ch=" << e.channel_idx << ": " << e.text;
                    else if constexpr (std::is_same_v<T, EvAckReceived>)
                        LOG_INFO() << "[event] ACK " << e.packet_id << (e.success ? " OK" : " FAIL");
                    else if constexpr (std::is_same_v<T, EvLogLine>)
                        LOG_INFO() << "[event] Log: " << e.message;
                    else if constexpr (std::is_same_v<T, EvError>)
                        LOG_ERROR() << "[event] Error: " << e.message;
                    else if constexpr (std::is_same_v<T, EvDisconnected>)
                        LOG_WARN() << "[event] Disconnected: " << e.reason;
                    else if constexpr (std::is_same_v<T, EvConfigLine>)
                        LOG_INFO() << "[event] Config: " << e.line.size() << " bytes";
                }, ev);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        // Dump node DB summary.
        for (const auto& id : service.device_ids()) {
            const NodeDb* db = service.db_for(id);
            if (db) {
                LOG_INFO() << "=== node DB for " << id << " ===";
                LOG_INFO() << "  my node: " << node_num_to_id(db->my_node_num());
                LOG_INFO() << "  nodes: " << db->all().size();
                LOG_INFO() << "  channels: " << db->channels().size();
                for (const auto& n : db->all())
                    LOG_INFO() << "  - " << n.long_name << " (" << n.node_id << ")";
                for (const auto& c : db->channels())
                    LOG_INFO() << "  - ch" << c.index << ": " << c.name << " [" << c.role << "]";
            }
        }
        service.disconnect_all();
        return 0;
    }

    TuiApp app(service, queue, wake);
    int rc = app.run();

    service.disconnect_all();
    return rc;
}

} // namespace meshcli
