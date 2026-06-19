#include "mesh/mesh_service.h"
#include "mesh/node_db.h"
#include "mesh/event.h"
#include "util/event_loop.h"
#include "minitest.h"

#include <iostream>
#include <chrono>
#include <thread>
#include <vector>

using namespace meshcli;
using namespace std::chrono_literals;

TEST(MeshSync, BasicDataTransfer) {
    // 1. Setup Server
    MeshService server_svc;
    EXPECT_TRUE(server_svc.open_database(":memory:"));

    // Insert some messages into the server's DB
    StoredMessage msg1;
    msg1.device = "test_radio";
    msg1.window_kind = "channel";
    msg1.window_target = 0;
    msg1.direction = "in";
    msg1.from_node = 1234;
    msg1.to_node = kBroadcastNodeNum;
    msg1.channel_idx = 0;
    msg1.text = "Hello from server 1";
    msg1.ts = 1000;
    msg1.packet_id = 100;
    server_svc.database().insert_message(msg1);

    StoredMessage msg2 = msg1;
    msg2.text = "Hello from server 2";
    msg2.ts = 2000;
    msg2.packet_id = 101;
    server_svc.database().insert_message(msg2);

    // Start stream server on an arbitrary port
    server_svc.start_stream_server(28491, "testuser", "testpass");

    // Give the server a moment to bind
    std::this_thread::sleep_for(100ms);

    // 2. Setup Client
    MeshService client_svc;
    EXPECT_TRUE(client_svc.open_database(":memory:"));

    // Set up an event sink for the client to handle events
    ConcurrentQueue<MeshEvent> queue;
    EventFd wake;
    client_svc.set_event_sink(&queue, &wake);

    BleDeviceSpec spec;
    spec.mesh_host = "127.0.0.1:28491";
    spec.mesh_user = "testuser";
    spec.mesh_password = "testpass";

    std::string client_device = client_svc.connect_device(spec, false);
    EXPECT_FALSE(client_device.empty());

    // Wait for the client to fully connect to the server (TLS handshake, etc)
    std::this_thread::sleep_for(200ms);

    // 3. Trigger sync on client
    client_svc.trigger_sync();

    // 4. Wait for sync to complete (should be very fast over local TCP)
    bool sync_completed = false;
    for (int i = 0; i < 20; ++i) {
        std::this_thread::sleep_for(50ms);
        WindowKey wk;
        wk.device = "test_radio";
        wk.kind = "channel";
        wk.target = 0;
        auto msgs = client_svc.database().recent_messages(wk);
        std::cout << "Attempt " << i << ": msgs.size()=" << msgs.size() << std::endl;
        if (msgs.size() >= 2) {
            sync_completed = true;
            // Verify message contents
            EXPECT_EQ(msgs[0].text, "Hello from server 1");
            EXPECT_EQ(msgs[1].text, "Hello from server 2");
            break;
        }
    }

    EXPECT_TRUE(sync_completed);
    
    server_svc.stop_stream_server();
    client_svc.disconnect_all();
}
