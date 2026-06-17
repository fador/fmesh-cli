#include "tui/command.h"
#include "tui/window_manager.h"
#include "mesh/mesh_service.h"
#include "mesh/node_db.h"
#include "store/database.h"

#include "minitest.h"

#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace meshcli;

class CommandTest {
public:
    CommandTest() : wm_(svc_) {}

    void SetUp() {
        wm_.clear();
        char tmppath[256];
        std::snprintf(tmppath, sizeof(tmppath),
                      "/tmp/mesh-cli-cmd-db-%d.db",
                      static_cast<int>(std::random_device{}()));
        db_path_ = tmppath;
        std::remove(db_path_.c_str());
        svc_.open_database(db_path_);

        auto rt = std::make_shared<DeviceRuntime>();
        rt->db = std::make_unique<NodeDb>();
        Node n;
        n.node_num = 0xD4A70330;
        n.node_id = "!d4a70330";
        n.long_name = "Fador #3 Smolboi";
        n.short_name = "Fad3";
        rt->db->upsert_node(n);
        Channel c0{0, "EdgeFastLow", true, "PRIMARY"};
        Channel c1{1, "Radio", false, "SECONDARY"};
        rt->db->upsert_channel(c0);
        rt->db->upsert_channel(c1);
        {
            std::lock_guard<std::mutex> lock(svc_.devices_mu_for_test());
            svc_.devices_for_test()["test_device"] = rt;
        }
    }

    void TearDown() { std::remove(db_path_.c_str()); }

    struct Capture {
        std::vector<std::string> lines;
        bool quit = false;
    };
    Capture exec(const std::string& line) {
        Capture c;
        CommandDispatcher disp(svc_, wm_,
            [&](const std::string& s, int) { c.lines.push_back(s); });
        auto res = disp.execute(line);
        c.quit = res.quit;
        return c;
    }

protected:
    MeshService svc_;
    WindowManager wm_;
    std::string db_path_;
};

// -- commands that produce status output -------------------------------

TEST_F(CommandTest, Help)       { EXPECT_FALSE(exec("/help").lines.empty()); }
TEST_F(CommandTest, List)       { wm_.ensure_channel("test_device",0,"EdgeFastLow"); exec("/list"); }
TEST_F(CommandTest, Nodes)      { exec("/nodes"); }
TEST_F(CommandTest, Info)       { exec("/info"); }
TEST_F(CommandTest, Reconnect)  { exec("/reconnect"); }
TEST_F(CommandTest, Unknown)    { exec("/bogus"); }

// -- /quit sets the quit flag ------------------------------------------

TEST_F(CommandTest, QuitSetsFlag) {
    EXPECT_TRUE(exec("/quit").quit);
    EXPECT_TRUE(exec("/exit").quit);
}

// -- plain text routing ------------------------------------------------

TEST_F(CommandTest, TextFromStatusBlocked) {
    wm_.select(1);
    auto c = exec("hello");
    std::string all;
    for (auto& l : c.lines) all += l;
    EXPECT_NE(all.find("Cannot send text"), std::string::npos);
}

TEST_F(CommandTest, TextInChannelOk) {
    wm_.ensure_channel("test_device", 0, "EdgeFastLow");
    wm_.select(2);
    exec("hey");  // should not crash
}

// -- /me ---------------------------------------------------------------

TEST_F(CommandTest, MeStatusBlocked) {
    wm_.select(1);
    auto c = exec("/me waves");
    std::string all;
    for (auto& l : c.lines) all += l;
    EXPECT_NE(all.find("Cannot"), std::string::npos);
}

TEST_F(CommandTest, MsgNoMatch) {
    auto c = exec("/msg nobody hello");
    std::string all;
    for (auto& l : c.lines) all += l;
    EXPECT_NE(all.find("No node matched"), std::string::npos);
}

TEST_F(CommandTest, MeChannelOk) {
    wm_.ensure_channel("test_device", 0, "EdgeFastLow");
    wm_.select(2);
    exec("/me waves");  // should not crash
}

// -- /msg --------------------------------------------------------------

TEST_F(CommandTest, MsgUsage) {
    auto c = exec("/msg");
    std::string all;
    for (auto& l : c.lines) all += l;
    EXPECT_NE(all.find("Usage"), std::string::npos);
}
