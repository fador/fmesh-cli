// Live integration test: connects to a real device, sends a broadcast and a
// DM, and verifies the outgoing messages are persisted to SQLite. Requires a
// paired Meshtastic device in range. Run manually (not part of ctest).
#include "app/config.h"
#include "mesh/mesh_service.h"
#include "mesh/node_db.h"
#include "util/event_loop.h"
#include "util/log.h"

#include <chrono>
#include <cstdio>
#include <thread>

using namespace meshcli;
using namespace std::chrono_literals;

int main(int argc, char** argv) {
    AppConfig cfg;
    if (!parse_args(argc, argv, cfg) || cfg.list_only) return 0;
    finalize_paths(cfg);
    cfg.headless = true;
    Logger::instance().init(cfg.log_path, true, LogLevel::Trace);

    MeshService service;
    if (!service.open_database(cfg.db_path)) {
        std::fprintf(stderr, "cannot open db\n");
        return 1;
    }

    ConcurrentQueue<MeshEvent> queue;
    EventFd wake;
    service.set_event_sink(&queue, &wake);

    BleDeviceSpec spec;
    spec.name = cfg.device_name;
    spec.address = cfg.device_addr;
    spec.pin = cfg.pin;
    std::string id = service.connect_device(spec, cfg.pair);
    if (id.empty()) {
        std::fprintf(stderr, "connect failed\n");
        return 1;
    }
    std::printf("connected to %s\n", id.c_str());

    // Drain initial events so the node DB is ready.
    wake.drain();
    queue.drain_all();
    std::this_thread::sleep_for(2s);
    queue.drain_all();

    const NodeDb* db = service.db_for(id);
    if (!db) { std::fprintf(stderr, "no db for device\n"); return 1; }
    std::printf("my node: %s, nodes known: %zu, channels: %zu\n",
                node_num_to_id(db->my_node_num()).c_str(),
                db->all().size(), db->channels().size());

    // --- Test 1: channel broadcast on ch0 ---
    std::string test_msg = "fmesh-cli live test broadcast " +
                           std::to_string(std::chrono::system_clock::now()
                                              .time_since_epoch().count());
    uint32_t pid1 = service.send_text(id, kBroadcastNodeNum, 0, test_msg, true);
    std::printf("broadcast send: packet_id=%u text=%zu bytes\n", pid1, test_msg.size());
    if (pid1 == 0) { std::fprintf(stderr, "FAIL: broadcast send returned 0\n"); return 1; }

    // --- Test 2: DM to ourselves (loopback test) ---
    uint32_t me = db->my_node_num();
    std::string dm_msg = "fmesh-cli live test DM";
    uint32_t pid2 = service.send_text(id, me, 0, dm_msg, true);
    std::printf("DM send: packet_id=%u to=%s text=%zu bytes\n",
                pid2, node_num_to_id(me).c_str(), dm_msg.size());
    if (pid2 == 0) { std::fprintf(stderr, "FAIL: DM send returned 0\n"); return 1; }

    // Wait for acks / inbound messages.
    std::this_thread::sleep_for(5s);
    int acks_ok = 0, msgs_in = 0;
    for (auto& ev : queue.drain_all()) {
        std::visit([&](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, EvAckReceived>) {
                std::printf("ACK packet=%u success=%d\n", e.packet_id, e.success);
                if (e.success) ++acks_ok;
            } else if constexpr (std::is_same_v<T, EvTextReceived>) {
                std::printf("RECV from=%s ch=%u text=%s\n",
                            node_num_to_id(e.from_node).c_str(), e.channel_idx,
                            e.text.c_str());
                ++msgs_in;
            }
        }, ev);
    }

    // --- Verify persistence via SQLite ---
    WindowKey wk;
    wk.device = id;
    wk.kind = "channel";
    wk.target = 0;
    auto recent = service.database().recent_messages(wk);
    bool found_broadcast = false;
    for (const auto& m : recent) {
        if (m.direction == "out" && m.text == test_msg) {
            found_broadcast = true;
            std::printf("PERSISTED broadcast: rowid=%lld ack=%s\n",
                        (long long)m.rowid, m.ack_state.c_str());
            break;
        }
    }

    std::printf("\n=== RESULTS ===\n");
    std::printf("broadcast sent (pid=%u): %s\n", pid1, pid1 ? "PASS" : "FAIL");
    std::printf("DM sent (pid=%u): %s\n", pid2, pid2 ? "PASS" : "FAIL");
    std::printf("broadcast persisted: %s\n", found_broadcast ? "PASS" : "FAIL");
    std::printf("acks received: %d/%d\n", acks_ok, 2);
    std::printf("inbound messages: %d\n", msgs_in);

    service.disconnect_all();
    return (pid1 && pid2 && found_broadcast) ? 0 : 1;
}
