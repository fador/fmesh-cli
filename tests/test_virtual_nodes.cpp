#include "minitest.h"
#include "mesh/mesh_service.h"
#include "mesh/db_sync.h"
#include "mesh/mesh_codec.h"
#include <vector>

using namespace meshcli;

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
