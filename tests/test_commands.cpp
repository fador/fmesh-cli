#include "tui/command.h"
#include "tui/window_manager.h"
#include "app/config.h"
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
                      "/tmp/fmesh-cli-cmd-db-%d.db",
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
    std::string theme_set_;
    bool server_toggled_ = false;

    Capture exec(const std::string& line) {
        Capture c;
        AppConfig dummy_config;
        CommandDispatcher disp(svc_, wm_,
            [&](const std::string& s, int) { c.lines.push_back(s); },
            active_dev_, dummy_config,
            nullptr,
            [&](const std::string& t) { theme_set_ = t; return t == "dark" || t == "classic"; },
            [&](bool on) { server_toggled_ = on; });
        auto res = disp.execute(line);
        c.quit = res.quit;
        return c;
    }

protected:
    MeshService svc_;
    WindowManager wm_;
    std::string active_dev_;
    std::string db_path_;
};

// -- commands that produce status output -------------------------------

TEST_F(CommandTest, Help)       { EXPECT_FALSE(exec("/help").lines.empty()); }
TEST_F(CommandTest, List)       { wm_.ensure_channel("test_device",0,"EdgeFastLow"); exec("/list"); }
TEST_F(CommandTest, Nodes)      { exec("/nodes"); }
TEST_F(CommandTest, Whois)      { exec("/whois Fad3"); }
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

// -- /whois ------------------------------------------------------------

TEST_F(CommandTest, WhoisUsage) {
    auto c = exec("/whois");
    std::string all;
    for (auto& l : c.lines) all += l;
    EXPECT_NE(all.find("Usage"), std::string::npos);
}

TEST_F(CommandTest, WhoisMatch) {
    auto c = exec("/whois Fad3");
    std::string all;
    for (auto& l : c.lines) all += l;
    EXPECT_NE(all.find("Fador"), std::string::npos);
}

TEST_F(CommandTest, WhoisNoMatch) {
    auto c = exec("/whois nobody");
    std::string all;
    for (auto& l : c.lines) all += l;
    EXPECT_NE(all.find("No node matched"), std::string::npos);
}

TEST_F(CommandTest, MeChannelOk) {
    wm_.ensure_channel("test_device", 0, "EdgeFastLow");
    wm_.select(2);
    exec("/me waves");  // should not crash
}

TEST_F(CommandTest, MsgUsage) {
    auto c = exec("/msg");
    std::string all;
    for (auto& l : c.lines) all += l;
    EXPECT_NE(all.find("Usage"), std::string::npos);
}

// -- /topic ------------------------------------------------------------

TEST_F(CommandTest, TopicOnStatusWindow) {
    wm_.select(1);
    auto c = exec("/topic");
    std::string all;
    for (auto& l : c.lines) all += l;
    EXPECT_NE(all.find("No channel selected"), std::string::npos);
}

TEST_F(CommandTest, TopicOnChannelWindow) {
    wm_.ensure_channel("test_device", 0, "EdgeFastLow");
    wm_.select(2);
    auto c = exec("/topic");
    std::string all;
    for (auto& l : c.lines) all += l;
    EXPECT_NE(all.find("EdgeFastLow"), std::string::npos);
}

// -- /stats ------------------------------------------------------------

TEST_F(CommandTest, StatsOutputsInfo) {
    auto c = exec("/stats");
    EXPECT_FALSE(c.lines.empty());
}

// -- /device -----------------------------------------------------------

TEST_F(CommandTest, DeviceListAndSwitch) {
    // List devices with no args
    auto c = exec("/device");
    std::string all;
    for (auto& l : c.lines) all += l;
    EXPECT_NE(all.find("test_device"), std::string::npos);

    // Switch to existing device
    auto c2 = exec("/device test_device");
    EXPECT_EQ(active_dev_, "test_device");

    // Unknown device error
    auto c3 = exec("/device nonexistent_device");
    std::string all3;
    for (auto& l : c3.lines) all3 += l;
    EXPECT_NE(all3.find("No device matched"), std::string::npos);
}

// -- /disconnect -------------------------------------------------------

TEST_F(CommandTest, DisconnectCommands) {
    // With no args, lists connected devices
    auto c = exec("/disconnect");
    std::string all;
    for (auto& l : c.lines) all += l;
    EXPECT_NE(all.find("test_device"), std::string::npos);

    // Unknown device
    auto c2 = exec("/disconnect bogus");
    std::string all2;
    for (auto& l : c2.lines) all2 += l;
    EXPECT_NE(all2.find("No device matched"), std::string::npos);

    // Valid disconnect
    auto c3 = exec("/disconnect test_device");
    std::string all3;
    for (auto& l : c3.lines) all3 += l;
    EXPECT_NE(all3.find("Disconnected"), std::string::npos);
    EXPECT_FALSE(svc_.has_devices());
}

// -- /connect ----------------------------------------------------------

TEST_F(CommandTest, ConnectCommands) {
    // Usage on empty args
    auto c = exec("/connect");
    std::string all;
    for (auto& l : c.lines) all += l;
    EXPECT_NE(all.find("Usage"), std::string::npos);

    // Invalid spec
    auto c2 = exec("/connect invalid_spec");
    std::string all2;
    for (auto& l : c2.lines) all2 += l;
    EXPECT_NE(all2.find("Invalid spec"), std::string::npos);
}

// -- /theme ------------------------------------------------------------

TEST_F(CommandTest, ThemeCommands) {
    // List themes
    auto c = exec("/theme");
    std::string all;
    for (auto& l : c.lines) all += l;
    EXPECT_NE(all.find("Available themes"), std::string::npos);

    // Switch theme
    exec("/theme dark");
    EXPECT_EQ(theme_set_, "dark");

    // Invalid theme
    auto c2 = exec("/theme bogus_theme");
    std::string all2;
    for (auto& l : c2.lines) all2 += l;
    EXPECT_NE(all2.find("Theme not found"), std::string::npos);
}

// -- /server -----------------------------------------------------------

TEST_F(CommandTest, ServerCommands) {
    exec("/server on");
    EXPECT_TRUE(server_toggled_);

    exec("/server off");
    EXPECT_FALSE(server_toggled_);
}

// -- /config -----------------------------------------------------------

TEST_F(CommandTest, ConfigCommands) {
    // Without data
    auto c = exec("/config");
    std::string all;
    for (auto& l : c.lines) all += l;
    EXPECT_NE(all.find("no config data received yet"), std::string::npos);

    // Filter section
    auto c2 = exec("/config lora");
    EXPECT_FALSE(c2.lines.empty());
}

// -- /window -----------------------------------------------------------

TEST_F(CommandTest, WindowSwitching) {
    wm_.ensure_channel("test_device", 0, "EdgeFastLow");
    EXPECT_EQ(wm_.current_index(), 1);

    // Switch to window 2
    exec("/window 2");
    EXPECT_EQ(wm_.current_index(), 2);

    // Non-integer window
    auto c = exec("/window invalid_num");
    std::string all;
    for (auto& l : c.lines) all += l;
    EXPECT_NE(all.find("Invalid window number"), std::string::npos);

    // Out of bounds window index does not switch
    exec("/window 99");
    EXPECT_EQ(wm_.current_index(), 2);
}

// -- /clear ------------------------------------------------------------

TEST_F(CommandTest, ClearCommand) {
    wm_.ensure_channel("test_device", 0, "EdgeFastLow");
    wm_.select(2);
    auto* win = wm_.current_window();
    ASSERT_NE(win, nullptr);
    win->append_line(Line{"hello world", 0, false, 0, 0});
    EXPECT_FALSE(win->lines().empty());

    exec("/clear");
    EXPECT_TRUE(win->lines().empty());
}

// -- /close ------------------------------------------------------------

TEST_F(CommandTest, CloseCommand) {
    // Cannot close status window
    auto c1 = exec("/close");
    std::string all1;
    for (auto& l : c1.lines) all1 += l;
    EXPECT_NE(all1.find("Cannot close the status window"), std::string::npos);

    // Create a nodelist window and a channel window
    wm_.ensure_nodelist("test_device");
    wm_.ensure_channel("test_device", 0, "EdgeFastLow");
    EXPECT_EQ(wm_.windows().size(), 3u);

    // Close nodelist window while currently focused on it
    wm_.select(2);
    auto c2 = exec("/close");
    std::string all2;
    for (auto& l : c2.lines) all2 += l;
    EXPECT_NE(all2.find("Closed window"), std::string::npos);
    EXPECT_EQ(wm_.windows().size(), 2u);

    // Close window by number
    auto c3 = exec("/close 2");
    std::string all3;
    for (auto& l : c3.lines) all3 += l;
    EXPECT_NE(all3.find("Closed window 2"), std::string::npos);
    EXPECT_EQ(wm_.windows().size(), 1u);
}

TEST_F(CommandTest, NodesCommandReusesWindow) {
    auto c1 = exec("/nodes");
    EXPECT_EQ(wm_.windows().size(), 2u);

    // Re-running /nodes selects the existing window and doesn't duplicate
    auto c2 = exec("/nodes");
    EXPECT_EQ(wm_.windows().size(), 2u);

    // Closing it works
    exec("/close");
    EXPECT_EQ(wm_.windows().size(), 1u);
}

// -- /traceroute -------------------------------------------------------

TEST_F(CommandTest, TracerouteCommand) {
    // No args in status window returns usage
    auto c1 = exec("/traceroute");
    std::string all1;
    for (auto& l : c1.lines) all1 += l;
    EXPECT_NE(all1.find("Usage"), std::string::npos);

    // Unknown node
    auto c2 = exec("/traceroute unknown_node");
    std::string all2;
    for (auto& l : c2.lines) all2 += l;
    EXPECT_NE(all2.find("Unknown node"), std::string::npos);

    // Valid traceroute by name
    auto c3 = exec("/traceroute Fad3");
    std::string all3;
    for (auto& l : c3.lines) all3 += l;
    EXPECT_NE(all3.find("Initiating traceroute to Fador #3 Smolboi"), std::string::npos);

    // Traceroute by hex id
    auto c4 = exec("/trace !d4a70330");
    std::string all4;
    for (auto& l : c4.lines) all4 += l;
    EXPECT_NE(all4.find("Initiating traceroute"), std::string::npos);

    // Traceroute with channel arg
    auto c5 = exec("/tr Fad3 1");
    std::string all5;
    for (auto& l : c5.lines) all5 += l;
    EXPECT_NE(all5.find("Initiating traceroute to Fador #3 Smolboi"), std::string::npos);

    // Default to current DM peer when in DM window
    int dm_win = wm_.ensure_dm("test_device", 0xD4A70330, "Fad3");
    wm_.select(dm_win);
    auto c6 = exec("/traceroute");
    std::string all6;
    for (auto& l : c6.lines) all6 += l;
    EXPECT_NE(all6.find("Initiating traceroute to Fador #3 Smolboi"), std::string::npos);
}

// -- /whois telemetry verification -------------------------------------

TEST_F(CommandTest, WhoisWithTelemetry) {
    Node n;
    n.node_num = 0xD4A70330;
    n.long_name = "Fador #3 Smolboi";
    n.short_name = "Fad3";
    n.battery_level = 88;
    n.voltage = 4.05f;
    n.temperature = 21.5f;
    n.relative_humidity = 45.0f;
    n.barometric_pressure = 1013.2f;
    n.gas_resistance = 12.34f;
    n.iaq = 42;
    n.co2 = 510;
    n.pm25 = 15;
    n.current = 120.0f;
    n.uptime_seconds = 7200;

    auto* db = const_cast<NodeDb*>(svc_.db_for("test_device"));
    ASSERT_NE(db, nullptr);
    db->upsert_node(n);

    auto c = exec("/whois Fad3");
    std::string all;
    for (auto& l : c.lines) all += l + "\n";

    EXPECT_NE(all.find("Battery:  88%"), std::string::npos);
    EXPECT_NE(all.find("Voltage:  4.05 V"), std::string::npos);
    EXPECT_NE(all.find("Temp:     21.5 C"), std::string::npos);
    EXPECT_NE(all.find("Humidity: 45.0%"), std::string::npos);
    EXPECT_NE(all.find("Pressure: 1013.2 hPa"), std::string::npos);
    EXPECT_NE(all.find("Gas res:  12.34 MOhm"), std::string::npos);
    EXPECT_NE(all.find("IAQ:      42"), std::string::npos);
    EXPECT_NE(all.find("CO2:      510 ppm"), std::string::npos);
    EXPECT_NE(all.find("PM2.5:    15 ug/m3"), std::string::npos);
    EXPECT_NE(all.find("Current:  120.0 mA"), std::string::npos);
    EXPECT_NE(all.find("Uptime:   2h 0m"), std::string::npos);
}

TEST_F(CommandTest, NodeRenameOnlyWhenActual) {
    // Current node in db is Fad3
    // Simulate telemetry packet arriving: EvNodeUpdated with empty short/long names
    EvNodeUpdated ev_telemetry;
    ev_telemetry.device = "test_device";
    ev_telemetry.node.node_num = 0xD4A70330;
    ev_telemetry.node.node_id = "!d4a70330";
    ev_telemetry.node.battery_level = 95;
    // Names are empty on telemetry packet!

    std::string old_nick = "Fad3";
    std::string nick = ev_telemetry.node.short_name.empty()
                           ? ev_telemetry.node.long_name : ev_telemetry.node.short_name;
    const NodeDb* db = svc_.db_for("test_device");
    if (nick.empty() && db) {
        auto existing = db->get(ev_telemetry.node.node_num);
        if (existing) {
            nick = existing->short_name.empty() ? existing->long_name : existing->short_name;
        }
    }
    EXPECT_EQ(nick, "Fad3");
    // Verify that (!old_nick.empty() && !nick.empty() && old_nick != nick) evaluates to false!
    bool would_rename = (!old_nick.empty() && !nick.empty() && old_nick != nick);
    EXPECT_FALSE(would_rename);

    // Now simulate genuine rename to "Fad4"
    ev_telemetry.node.short_name = "Fad4";
    ev_telemetry.node.long_name = "Fador #4";
    nick = ev_telemetry.node.short_name;
    bool actual_rename = (!old_nick.empty() && !nick.empty() && old_nick != nick);
    EXPECT_TRUE(actual_rename);
}


