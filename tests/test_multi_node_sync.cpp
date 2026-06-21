#include "mesh/mesh_service.h"
#include "mesh/node_db.h"
#include "mesh/db_sync.h"
#include "mesh/event.h"
#include "util/event_loop.h"
#include "minitest.h"

#include <iostream>
#include <chrono>
#include <thread>
#include <vector>

using namespace meshcli;
using namespace std::chrono_literals;

TEST(MeshSync, MultiNodePropagation) {
    // Topology: C <-> B <-> A
    // Node A (Hub 1): StreamServer on 28601
    // Node B (Hub 2): StreamServer on 28602, Client to A
    // Node C (Client 1): Client to B

    // 1. Setup Node A
    MeshService node_a;
    EXPECT_TRUE(node_a.open_database(":memory:"));
    node_a.start_stream_server(28601, "admin", "admin");

    std::this_thread::sleep_for(100ms);

    // 2. Setup Node B
    MeshService node_b;
    EXPECT_TRUE(node_b.open_database(":memory:"));
    node_b.start_stream_server(28602, "admin", "admin");
    
    ConcurrentQueue<MeshEvent> queue_b;
    EventFd wake_b;
    node_b.set_event_sink(&queue_b, &wake_b);

    BleDeviceSpec spec_b_to_a;
    spec_b_to_a.mesh_host = "127.0.0.1:28601";
    spec_b_to_a.mesh_user = "admin";
    spec_b_to_a.mesh_password = "admin";
    std::string device_b_to_a = node_b.connect_device(spec_b_to_a, false);
    EXPECT_FALSE(device_b_to_a.empty());

    std::this_thread::sleep_for(200ms);

    // 3. Setup Node C
    MeshService node_c;
    EXPECT_TRUE(node_c.open_database(":memory:"));
    
    ConcurrentQueue<MeshEvent> queue_c;
    EventFd wake_c;
    node_c.set_event_sink(&queue_c, &wake_c);

    BleDeviceSpec spec_c_to_b;
    spec_c_to_b.mesh_host = "127.0.0.1:28602";
    spec_c_to_b.mesh_user = "admin";
    spec_c_to_b.mesh_password = "admin";
    std::string device_c_to_b = node_c.connect_device(spec_c_to_b, false);
    EXPECT_FALSE(device_c_to_b.empty());

    std::this_thread::sleep_for(200ms);

    // 4. Inject a message on Node C
    StoredMessage msg;
    msg.device = "local_simulated_radio";
    msg.window_kind = "channel";
    msg.window_target = 0;
    msg.direction = "in";
    msg.from_node = 0xC0FFEE;
    msg.to_node = kBroadcastNodeNum;
    msg.channel_idx = 0;
    msg.text = "Hello from Node C";
    msg.ts = static_cast<uint32_t>(std::time(nullptr));
    msg.packet_id = 999;
    
    // Insert into Node C DB and simulate reception
    node_c.database().insert_message(msg);
    
    // We need to trigger a push to DbSyncManager on Node C
    if (node_c.sync_manager()) {
        node_c.sync_manager()->push_message(msg);
    }

    // Drain queues so background handlers don't block
    auto process_queues = [&]() {
        queue_b.drain_all();
        queue_c.drain_all();
    };

    // 5. Wait for message to propagate to Node A
    bool a_received = false;
    for (int i = 0; i < 40; ++i) {
        process_queues();
        std::this_thread::sleep_for(50ms);
        
        WindowKey wk;
        wk.device = "local_simulated_radio";
        wk.kind = "channel";
        wk.target = 0;
        auto msgs_a = node_a.database().recent_messages(wk);
        
        if (!msgs_a.empty()) {
            EXPECT_EQ(msgs_a.back().text, "Hello from Node C");
            EXPECT_EQ(msgs_a.back().from_node, 0xC0FFEE);
            a_received = true;
            break;
        }
    }

    EXPECT_TRUE(a_received);

    // 6. Cleanup
    node_c.disconnect_all();
    node_b.stop_stream_server();
    node_b.disconnect_all();
    node_a.stop_stream_server();
}
