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

    static std::atomic<int> base_port{28600};
    int port_a = base_port.fetch_add(2);
    int port_b = port_a + 1;

    // 1. Setup Node A
    MeshService node_a;
    EXPECT_TRUE(node_a.open_database(":memory:"));
    node_a.start_stream_server(port_a, "admin", "admin");

    std::this_thread::sleep_for(100ms);

    // 2. Setup Node B
    MeshService node_b;
    EXPECT_TRUE(node_b.open_database(":memory:"));
    node_b.start_stream_server(port_b, "admin", "admin");
    
    ConcurrentQueue<MeshEvent> queue_b;
    EventFd wake_b;
    node_b.set_event_sink(&queue_b, &wake_b);

    BleDeviceSpec spec_b_to_a;
    spec_b_to_a.mesh_host = "127.0.0.1:" + std::to_string(port_a);
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
    spec_c_to_b.mesh_host = "127.0.0.1:" + std::to_string(port_b);
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
    node_a.disconnect_all();
}

TEST(MeshSync, FiveClientStarMeshSharing) {
    // Topology: Star / Hub-and-Spoke with 1 Server (Hub) and 4 connected clients.
    // Total 5 distinct CLI node instances sharing messages and coordinates over TCP/TLS.
    static std::atomic<int> base_port{28800};
    int srv_port = base_port.fetch_add(10);

    // 1. Hub Node (Server)
    MeshService hub;
    EXPECT_TRUE(hub.open_database(":memory:"));
    ConcurrentQueue<MeshEvent> hub_queue;
    EventFd hub_wake;
    hub.set_event_sink(&hub_queue, &hub_wake);
    hub.start_stream_server(srv_port, "meshuser", "meshpass");

    std::this_thread::sleep_for(100ms);

    // 2. Setup 4 Client Nodes (Nodes 1 to 4)
    constexpr int kNumClients = 4;
    std::vector<std::unique_ptr<MeshService>> clients;
    std::vector<std::unique_ptr<ConcurrentQueue<MeshEvent>>> client_queues;
    std::vector<std::unique_ptr<EventFd>> client_wakes;

    for (int i = 0; i < kNumClients; ++i) {
        auto svc = std::make_unique<MeshService>();
        EXPECT_TRUE(svc->open_database(":memory:"));

        auto q = std::make_unique<ConcurrentQueue<MeshEvent>>();
        auto w = std::make_unique<EventFd>();
        svc->set_event_sink(q.get(), w.get());

        BleDeviceSpec spec;
        spec.mesh_host = "127.0.0.1:" + std::to_string(srv_port);
        spec.mesh_user = "meshuser";
        spec.mesh_password = "meshpass";

        std::string dev_id = svc->connect_device(spec, false);
        EXPECT_FALSE(dev_id.empty());

        clients.push_back(std::move(svc));
        client_queues.push_back(std::move(q));
        client_wakes.push_back(std::move(w));
    }

    // Allow all clients to establish TLS and authenticate
    std::this_thread::sleep_for(300ms);

    auto drain_all = [&]() {
        hub_queue.drain_all();
        for (auto& q : client_queues) {
            q->drain_all();
        }
    };

    // 3. Test Message Sharing: Client 3 sends a broadcast message
    StoredMessage msg;
    msg.device = "client_3_radio";
    msg.window_kind = "channel";
    msg.window_target = 0;
    msg.direction = "in";
    msg.from_node = 0x1337;
    msg.to_node = kBroadcastNodeNum;
    msg.channel_idx = 0;
    msg.text = "Hello 5-node TCP mesh network!";
    msg.ts = static_cast<uint64_t>(std::time(nullptr));
    msg.packet_id = 424242;

    clients[3]->database().insert_message(msg);
    if (clients[3]->sync_manager()) {
        clients[3]->sync_manager()->push_message(msg);
    }

    // Wait for message to propagate to Hub and all other 3 clients
    WindowKey wk{"client_3_radio", "channel", 0};
    bool all_got_msg = false;
    for (int step = 0; step < 50; ++step) {
        drain_all();
        std::this_thread::sleep_for(50ms);

        bool hub_has = !hub.database().recent_messages(wk).empty();
        bool c0_has = !clients[0]->database().recent_messages(wk).empty();
        bool c1_has = !clients[1]->database().recent_messages(wk).empty();
        bool c2_has = !clients[2]->database().recent_messages(wk).empty();

        if (hub_has && c0_has && c1_has && c2_has) {
            all_got_msg = true;
            EXPECT_EQ(hub.database().recent_messages(wk).back().text, "Hello 5-node TCP mesh network!");
            EXPECT_EQ(clients[0]->database().recent_messages(wk).back().text, "Hello 5-node TCP mesh network!");
            EXPECT_EQ(clients[1]->database().recent_messages(wk).back().text, "Hello 5-node TCP mesh network!");
            EXPECT_EQ(clients[2]->database().recent_messages(wk).back().text, "Hello 5-node TCP mesh network!");
            break;
        }
    }
    EXPECT_TRUE(all_got_msg);

    // 4. Test Location Telemetry Sharing: Client 1 sends GPS coordinate update
    Database::LocationRow loc;
    loc.device = "client_1_radio";
    loc.node_num = 0x8888;
    loc.latitude = 60.1699;
    loc.longitude = 24.9384;
    loc.altitude = 45;
    loc.ts = static_cast<uint64_t>(std::time(nullptr));

    clients[1]->database().insert_location(loc.device, loc.node_num, loc.latitude, loc.longitude, loc.altitude, loc.ts);
    if (clients[1]->sync_manager()) {
        clients[1]->sync_manager()->push_location(loc);
    }

    // Wait for location to propagate to Hub and other clients (0, 2, 3)
    bool all_got_loc = false;
    for (int step = 0; step < 50; ++step) {
        drain_all();
        std::this_thread::sleep_for(50ms);

        auto hub_locs = hub.database().get_locations_after(0, 10);
        auto c0_locs = clients[0]->database().get_locations_after(0, 10);
        auto c2_locs = clients[2]->database().get_locations_after(0, 10);
        auto c3_locs = clients[3]->database().get_locations_after(0, 10);

        if (!hub_locs.empty() && !c0_locs.empty() && !c2_locs.empty() && !c3_locs.empty()) {
            all_got_loc = true;
            EXPECT_EQ(hub_locs.back().node_num, 0x8888u);
            EXPECT_FLOAT_EQ(hub_locs.back().latitude, 60.1699);
            EXPECT_FLOAT_EQ(c0_locs.back().latitude, 60.1699);
            EXPECT_FLOAT_EQ(c2_locs.back().latitude, 60.1699);
            EXPECT_FLOAT_EQ(c3_locs.back().latitude, 60.1699);
            break;
        }
    }
    EXPECT_TRUE(all_got_loc);

    // 5. Cleanup
    for (auto& c : clients) {
        c->disconnect_all();
    }
    hub.stop_stream_server();
    hub.disconnect_all();
}

TEST(MeshSync, FiveNodeChainPropagation) {
    // Topology: E <-> D <-> C <-> B <-> A (5 nodes in a multi-hop daisy chain)
    // Node A (Hub 1): StreamServer on port_a
    // Node B (Hub 2): StreamServer on port_b, connects to A
    // Node C (Hub 3): StreamServer on port_c, connects to B
    // Node D (Hub 4): StreamServer on port_d, connects to C
    // Node E (Client): connects to D
    static std::atomic<int> chain_base_port{28900};
    int p_a = chain_base_port.fetch_add(5);
    int p_b = p_a + 1;
    int p_c = p_b + 1;
    int p_d = p_c + 1;

    MeshService node_a, node_b, node_c, node_d, node_e;
    EXPECT_TRUE(node_a.open_database(":memory:"));
    EXPECT_TRUE(node_b.open_database(":memory:"));
    EXPECT_TRUE(node_c.open_database(":memory:"));
    EXPECT_TRUE(node_d.open_database(":memory:"));
    EXPECT_TRUE(node_e.open_database(":memory:"));

    node_a.start_stream_server(p_a, "chain", "chain");
    node_b.start_stream_server(p_b, "chain", "chain");
    node_c.start_stream_server(p_c, "chain", "chain");
    node_d.start_stream_server(p_d, "chain", "chain");

    std::this_thread::sleep_for(100ms);

    ConcurrentQueue<MeshEvent> q_b, q_c, q_d, q_e;
    EventFd w_b, w_c, w_d, w_e;
    node_b.set_event_sink(&q_b, &w_b);
    node_c.set_event_sink(&q_c, &w_c);
    node_d.set_event_sink(&q_d, &w_d);
    node_e.set_event_sink(&q_e, &w_e);

    auto connect_to = [](MeshService& client, int port) {
        BleDeviceSpec spec;
        spec.mesh_host = "127.0.0.1:" + std::to_string(port);
        spec.mesh_user = "chain";
        spec.mesh_password = "chain";
        std::string id = client.connect_device(spec, false);
        EXPECT_FALSE(id.empty());
    };

    connect_to(node_b, p_a);
    connect_to(node_c, p_b);
    connect_to(node_d, p_c);
    connect_to(node_e, p_d);

    std::this_thread::sleep_for(400ms);

    auto drain = [&]() {
        q_b.drain_all();
        q_c.drain_all();
        q_d.drain_all();
        q_e.drain_all();
    };

    // Inject message at the edge (Node E)
    StoredMessage msg;
    msg.device = "edge_radio_e";
    msg.window_kind = "channel";
    msg.window_target = 0;
    msg.direction = "in";
    msg.from_node = 0xEEEE;
    msg.to_node = kBroadcastNodeNum;
    msg.channel_idx = 0;
    msg.text = "Propagating through 4 TCP hops!";
    msg.ts = static_cast<uint64_t>(std::time(nullptr));
    msg.packet_id = 99999;

    node_e.database().insert_message(msg);
    if (node_e.sync_manager()) {
        node_e.sync_manager()->push_message(msg);
    }

    // Verify it travels all the way to Node A
    WindowKey wk{"edge_radio_e", "channel", 0};
    bool a_got_it = false;
    for (int step = 0; step < 60; ++step) {
        drain();
        std::this_thread::sleep_for(50ms);

        auto msgs_a = node_a.database().recent_messages(wk);
        if (!msgs_a.empty()) {
            EXPECT_EQ(msgs_a.back().text, "Propagating through 4 TCP hops!");
            EXPECT_EQ(msgs_a.back().from_node, 0xEEEEu);
            a_got_it = true;
            break;
        }
    }
    EXPECT_TRUE(a_got_it);

    // Cleanup
    node_e.disconnect_all();
    node_d.stop_stream_server();
    node_d.disconnect_all();
    node_c.stop_stream_server();
    node_c.disconnect_all();
    node_b.stop_stream_server();
    node_b.disconnect_all();
    node_a.stop_stream_server();
    node_a.disconnect_all();
}

