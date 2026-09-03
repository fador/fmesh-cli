#include "minitest.h"
#include "mesh/mesh_service.h"
#include "mesh/db_sync.h"
#include "mesh/mesh_codec.h"
#include "tui/command.h"
#include "tui/window_manager.h"
#include "app/config.h"
#include "ble/ble_client.h"
#include "meshtastic/mesh.pb.h"
#include "meshtastic/portnums.pb.h"

#include <vector>
#include <memory>
#include <chrono>
#include <thread>
#include <nlohmann/json.hpp>

using namespace meshcli;
using namespace std::chrono_literals;

class MockRadioClient : public BleClient {
public:
    explicit MockRadioClient(std::string id) : id_(std::move(id)) {}
    std::string start(bool) override { return id_; }
    void stop() override {}
    bool send_to_radio(const std::string& bytes) override {
        sent_packets.push_back(bytes);
        return true;
    }
    std::string device_id() const override { return id_; }
    bool is_connected() const override { return true; }

    std::string id_;
    std::vector<std::string> sent_packets;
};

TEST(VirtualNodesTest, VirtualDeviceHandling) {
    MeshService mesh;
    DbSyncManager* sync = mesh.sync_manager();
    
    // Simulate receiving a "devices" payload
    std::string device_id = "mesh:127.0.0.1:4404";
    std::string payload = R"({"devices":[{"id":"ttyUSB0","node_num":123},{"id":"ttyUSB1","node_num":456}],"type":"devices"})";
    
    sync->handle_sync_payload(device_id, payload);
    
    auto devs = mesh.device_ids();
    
    // There are NO physical devices or offline dbs, only the two virtual ones we just added
    ASSERT_EQ((int)devs.size(), 2);
    
    bool found_0 = false;
    bool found_1 = false;
    
    for (const auto& d : devs) {
        if (d == "virtual:mesh:127.0.0.1:4404:ttyUSB0") found_0 = true;
        if (d == "virtual:mesh:127.0.0.1:4404:ttyUSB1") found_1 = true;
    }
    
    ASSERT_TRUE(found_0);
    ASSERT_TRUE(found_1);
    
    // Test display name
    ASSERT_EQ(mesh.display_name_for("virtual:mesh:127.0.0.1:4404:ttyUSB0"), "Virtual ttyUSB0");
}

TEST(VirtualNodesTest, DeviceSelectionCommand) {
    MeshService mesh;
    EXPECT_TRUE(mesh.open_database(":memory:"));

    // 1. Add a local physical radio device
    auto local_rt = std::make_shared<DeviceRuntime>();
    local_rt->db = std::make_unique<NodeDb>();
    local_rt->display_name = "Local_LoRa";
    local_rt->my_node_num = 0x1111;
    {
        std::lock_guard<std::mutex> lock(mesh.devices_mu_for_test());
        mesh.devices_for_test()["ble:Local_LoRa"] = local_rt;
    }

    // 2. Add a virtual device via mesh link
    std::string stream_id = "mesh:127.0.0.1:4404";
    std::string payload = R"({"devices":[{"id":"serial:/dev/ttyUSB0","node_num":2222}],"type":"devices"})";
    mesh.sync_manager()->handle_sync_payload(stream_id, payload);

    auto devs = mesh.device_ids();
    EXPECT_EQ(devs.size(), 2u);

    // 3. Use CommandDispatcher to test /device command
    WindowManager wm(mesh);
    std::string active_dev = "ble:Local_LoRa";
    AppConfig cfg;
    std::vector<std::string> status_lines;
    auto status_cb = [&](const std::string& line, int) {
        status_lines.push_back(line);
    };

    CommandDispatcher dispatcher(mesh, wm, status_cb, active_dev, cfg);

    // List devices via /device
    status_lines.clear();
    dispatcher.execute("/device");
    std::string all_status;
    for (const auto& l : status_lines) all_status += l + "\n";
    EXPECT_NE(all_status.find("Local_LoRa"), std::string::npos);
    EXPECT_NE(all_status.find("Virtual serial:/dev/ttyUSB0"), std::string::npos);

    // Switch to virtual device by query "Virtual" or "ttyUSB0"
    status_lines.clear();
    dispatcher.execute("/device ttyUSB0");
    EXPECT_EQ(active_dev, "virtual:mesh:127.0.0.1:4404:serial:/dev/ttyUSB0");
    EXPECT_EQ(mesh.display_name_for(active_dev), "Virtual serial:/dev/ttyUSB0");

    // Switch back to local physical device by query "Local"
    status_lines.clear();
    dispatcher.execute("/device Local");
    EXPECT_EQ(active_dev, "ble:Local_LoRa");
    EXPECT_EQ(mesh.display_name_for(active_dev), "Local_LoRa");
}

TEST(VirtualNodesTest, DirectPhysicalVsMeshVirtualRouting) {
    // Top-level topology:
    // Instance A (Server): Has physical radio "ttyUSB_local" attached
    // Instance B (Client): Connects to Instance A via TCP mesh stream
    //                      Also has its own physical radio "ble_local"
    static std::atomic<int> test_port{29100};
    int srv_port = test_port.fetch_add(1);

    // 1. Setup Instance A (Server)
    MeshService srv;
    EXPECT_TRUE(srv.open_database(":memory:"));
    srv.start_stream_server(srv_port, "testuser", "testpass");

    // Attach mock physical radio to Instance A
    auto srv_mock = std::make_unique<MockRadioClient>("ttyUSB_local");
    MockRadioClient* srv_mock_ptr = srv_mock.get();
    auto srv_rt = std::make_shared<DeviceRuntime>();
    srv_rt->db = std::make_unique<NodeDb>();
    srv_rt->my_node_num = 0xAAAA;
    srv_rt->display_name = "InstanceA_Radio";
    srv_rt->client = std::move(srv_mock);
    {
        std::lock_guard<std::mutex> lock(srv.devices_mu_for_test());
        srv.devices_for_test()["ttyUSB_local"] = srv_rt;
    }

    std::this_thread::sleep_for(100ms);

    // 2. Setup Instance B (Client)
    MeshService client;
    EXPECT_TRUE(client.open_database(":memory:"));
    ConcurrentQueue<MeshEvent> client_q;
    EventFd client_w;
    client.set_event_sink(&client_q, &client_w);

    // Attach local mock physical radio to Instance B
    auto client_mock = std::make_unique<MockRadioClient>("ble_local");
    MockRadioClient* client_mock_ptr = client_mock.get();
    auto client_rt = std::make_shared<DeviceRuntime>();
    client_rt->db = std::make_unique<NodeDb>();
    client_rt->my_node_num = 0xBBBB;
    client_rt->display_name = "InstanceB_Radio";
    client_rt->client = std::move(client_mock);
    {
        std::lock_guard<std::mutex> lock(client.devices_mu_for_test());
        client.devices_for_test()["ble_local"] = client_rt;
    }

    // Connect Instance B to Instance A via TCP mesh stream
    BleDeviceSpec spec;
    spec.mesh_host = "127.0.0.1:" + std::to_string(srv_port);
    spec.mesh_user = "testuser";
    spec.mesh_password = "testpass";
    std::string stream_dev_id = client.connect_device(spec, false);
    EXPECT_FALSE(stream_dev_id.empty());

    std::this_thread::sleep_for(200ms);

    // Instance A advertises its physical radio to the mesh stream
    srv.sync_manager()->push_devices({{"ttyUSB_local", 0xAAAA}});

    // Wait for Instance B to receive the devices announcement
    std::string expected_vdev = "virtual:" + stream_dev_id + ":ttyUSB_local";
    bool vdev_found = false;
    for (int i = 0; i < 40; ++i) {
        client_q.drain_all();
        std::this_thread::sleep_for(50ms);
        auto devs = client.device_ids();
        for (const auto& d : devs) {
            if (d == expected_vdev) {
                vdev_found = true;
                break;
            }
        }
        if (vdev_found) break;
    }
    EXPECT_TRUE(vdev_found);
    EXPECT_EQ(client.display_name_for(expected_vdev), "Virtual ttyUSB_local");

    // Clear any startup handshake packets (e.g. want_config) before testing application sends
    srv_mock_ptr->sent_packets.clear();
    client_mock_ptr->sent_packets.clear();

    // 3. Test sending from Local Physical Radio ("ble_local") on Instance B
    EXPECT_EQ(client_mock_ptr->sent_packets.size(), 0u);
    EXPECT_EQ(srv_mock_ptr->sent_packets.size(), 0u);

    uint32_t pid_local = client.send_text("ble_local", kBroadcastNodeNum, 0, "Sent directly from local BLE radio", false);
    EXPECT_NE(pid_local, 0u);

    // Verify it was dispatched to Instance B's local mock physical radio
    EXPECT_EQ(client_mock_ptr->sent_packets.size(), 1u);
    // Instance A's radio should NOT have received this packet
    EXPECT_EQ(srv_mock_ptr->sent_packets.size(), 0u);

    // Parse the packet sent locally to verify radio payload
    meshtastic::ToRadio tr_local;
    EXPECT_TRUE(tr_local.ParseFromString(client_mock_ptr->sent_packets[0]));
    EXPECT_EQ(tr_local.packet().to(), kBroadcastNodeNum);
    EXPECT_EQ(tr_local.packet().decoded().payload(), "Sent directly from local BLE radio");

    // Verify database stored message has correct local from_node
    auto msgs_local = client.database().recent_messages(WindowKey{"ble_local", "channel", 0});
    EXPECT_FALSE(msgs_local.empty());
    EXPECT_EQ(msgs_local.back().from_node, 0xBBBBu);

    // 4. Test sending from Virtual Radio ("virtual:...:ttyUSB_local") on Instance B over TCP
    uint32_t pid_virtual = client.send_text(expected_vdev, kBroadcastNodeNum, 0, "Sent remotely via TCP mesh link", false);
    EXPECT_NE(pid_virtual, 0u);

    // Verify database stored message has correct virtual remote from_node
    auto msgs_virt = client.database().recent_messages(WindowKey{expected_vdev, "channel", 0});
    EXPECT_FALSE(msgs_virt.empty());
    EXPECT_EQ(msgs_virt.back().from_node, 0xAAAAu);

    // Wait for the send_raw packet to arrive over TCP at Instance A and be sent to srv_mock
    bool srv_got_pkt = false;
    for (int i = 0; i < 40; ++i) {
        client_q.drain_all();
        std::this_thread::sleep_for(50ms);
        if (!srv_mock_ptr->sent_packets.empty()) {
            srv_got_pkt = true;
            break;
        }
    }
    EXPECT_TRUE(srv_got_pkt);

    // Verify Instance A's physical radio received the packet for transmission!
    EXPECT_EQ(srv_mock_ptr->sent_packets.size(), 1u);
    meshtastic::ToRadio tr_remote;
    EXPECT_TRUE(tr_remote.ParseFromString(srv_mock_ptr->sent_packets[0]));
    EXPECT_EQ(tr_remote.packet().to(), kBroadcastNodeNum);
    EXPECT_EQ(tr_remote.packet().decoded().payload(), "Sent remotely via TCP mesh link");

    // Instance B's local radio count should still be 1 (not affected by virtual send)
    EXPECT_EQ(client_mock_ptr->sent_packets.size(), 1u);

    // 5. Cleanup
    client.disconnect_all();
    srv.stop_stream_server();
    srv.disconnect_all();
}
