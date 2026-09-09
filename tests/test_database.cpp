// Unit tests for SQLite persistence: stores nodes, channels, messages,
// reloads them, and verifies ack state updates.
#include "store/database.h"
#include "mesh/node_db.h"

#include "minitest.h"

#include <cstdio>
#include <chrono>
#include <random>

using namespace meshcli;

class DatabaseTest {
public:
    void SetUp() {
        // Use a temp file so tests are isolated.
        char tmppath[256];
        std::snprintf(tmppath, sizeof(tmppath),
                      "fmesh-cli-test-db-%d.db",
                      static_cast<int>(std::random_device{}()));
        db_path_ = tmppath;
        // Remove any stale file.
        std::remove(db_path_.c_str());
        ASSERT_TRUE(db_.open(db_path_));
    }

    void TearDown() {
        db_.close();
        std::remove(db_path_.c_str());
    }

    Database db_;
    std::string db_path_;
};

// --- nodes ---------------------------------------------------------------

TEST_F(DatabaseTest, StoreAndLoadNode) {
    Node n;
    n.node_num = 0xD4A70330;
    n.node_id = "!d4a70330";
    n.long_name = "Fador #3 Smolboi";
    n.short_name = "Fad3";
    n.hw_model = "SEEED_XIAO_S3";
    n.role = "CLIENT_MUTE";
    n.battery_level = 100;
    n.voltage = 4.2f;
    n.snr = 7.5f;
    db_.upsert_node("dev1", n);

    NodeDb ndb;
    db_.load_nodes("dev1", ndb);

    auto got = ndb.get(0xD4A70330);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->long_name, "Fador #3 Smolboi");
    EXPECT_EQ(got->short_name, "Fad3");
    EXPECT_EQ(got->hw_model, "SEEED_XIAO_S3");
    EXPECT_EQ(got->role, "CLIENT_MUTE");
    ASSERT_TRUE(got->battery_level.has_value());
    EXPECT_EQ(*got->battery_level, 100u);
    EXPECT_FLOAT_EQ(got->voltage.value(), 4.2f);
    EXPECT_FLOAT_EQ(got->snr.value(), 7.5f);
}

TEST_F(DatabaseTest, UpsertNodeUpdatesExisting) {
    Node n;
    n.node_num = 42;
    n.long_name = "Alice";
    n.short_name = "A";
    db_.upsert_node("dev1", n);

    n.long_name = "Alice2";
    n.short_name = "A2";
    n.battery_level = 80;
    db_.upsert_node("dev1", n);

    NodeDb ndb;
    db_.load_nodes("dev1", ndb);

    auto got = ndb.get(42);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->long_name, "Alice2");
    EXPECT_EQ(got->short_name, "A2");
    EXPECT_EQ(*got->battery_level, 80u);
    EXPECT_EQ(ndb.all().size(), 1u);
}

TEST_F(DatabaseTest, LoadNodesEmptyDbReturnsNothing) {
    NodeDb ndb;
    db_.load_nodes("nonexistent", ndb);
    EXPECT_EQ(ndb.all().size(), 0u);
}

TEST_F(DatabaseTest, MultipleNodes) {
    for (uint32_t i = 0; i < 10; ++i) {
        Node n;
        n.node_num = i;
        n.long_name = "Node" + std::to_string(i);
        db_.upsert_node("dev1", n);
    }
    NodeDb ndb;
    db_.load_nodes("dev1", ndb);
    EXPECT_EQ(ndb.all().size(), 10u);
}

// --- channels -------------------------------------------------------------

TEST_F(DatabaseTest, StoreAndLoadChannel) {
    Channel c{0, "EdgeFastLow", true, "PRIMARY"};
    db_.upsert_channel("dev1", c);

    NodeDb ndb;
    db_.load_channels("dev1", ndb);

    auto got = ndb.channel(0);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->name, "EdgeFastLow");
    EXPECT_EQ(got->role, "PRIMARY");
    EXPECT_TRUE(got->has_psk);
}

TEST_F(DatabaseTest, UpsertChannelUpdatesExisting) {
    Channel c{0, "old", false, "SECONDARY"};
    db_.upsert_channel("dev1", c);

    Channel c2{0, "new", true, "PRIMARY"};
    db_.upsert_channel("dev1", c2);

    NodeDb ndb;
    db_.load_channels("dev1", ndb);

    auto got = ndb.channel(0);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->name, "new");
    EXPECT_EQ(got->role, "PRIMARY");
    EXPECT_TRUE(got->has_psk);
    EXPECT_EQ(ndb.channels().size(), 1u);
}

TEST_F(DatabaseTest, DeviceConfigPersistence) {
    db_.upsert_device_config("dev1", "lora", "lora.tx_power", "27");
    db_.upsert_device_config("dev1", "lora", "lora.use_preset", "true");
    db_.upsert_device_config("dev1", "device", "device.role", "ROUTER");

    auto items = db_.load_device_config("dev1");
    ASSERT_EQ(items.size(), 3u);

    // Update existing
    db_.upsert_device_config("dev1", "lora", "lora.tx_power", "20");
    auto items2 = db_.load_device_config("dev1");
    ASSERT_EQ(items2.size(), 3u);

    bool found = false;
    for (const auto& it : items2) {
        if (it.key == "lora.tx_power") {
            EXPECT_EQ(it.value, "20");
            found = true;
        }
    }
    EXPECT_TRUE(found);

    // Empty for other device
    auto empty_items = db_.load_device_config("dev2");
    EXPECT_TRUE(empty_items.empty());
}

TEST_F(DatabaseTest, MultipleChannels) {
    for (uint32_t i = 0; i < 8; ++i) {
        Channel c{i, "ch" + std::to_string(i),
                  i == 0, i == 0 ? "PRIMARY" : "DISABLED"};
        db_.upsert_channel("dev1", c);
    }
    NodeDb ndb;
    db_.load_channels("dev1", ndb);
    EXPECT_EQ(ndb.channels().size(), 8u);
}

// --- messages -------------------------------------------------------------

TEST_F(DatabaseTest, InsertAndReloadMessage) {
    StoredMessage m;
    m.device = "dev1";
    m.window_kind = "channel";
    m.window_target = 0;
    m.direction = "in";
    m.from_node = 0xD4A70330;
    m.to_node = 0xFFFFFFFF;  // broadcast
    m.channel_idx = 0;
    m.text = "hello from test";
    m.ts = 1700000000;
    m.packet_id = 42;
    m.ack_state = "";
    int64_t rowid = db_.insert_message(m);
    EXPECT_GT(rowid, 0);

    WindowKey wk{"dev1", "channel", 0};
    auto recent = db_.recent_messages(wk);
    ASSERT_EQ(recent.size(), 1u);
    EXPECT_EQ(recent[0].text, "hello from test");
    EXPECT_EQ(recent[0].from_node, 0xD4A70330u);
    EXPECT_EQ(recent[0].direction, "in");
    EXPECT_EQ(recent[0].packet_id, 42u);
}

TEST_F(DatabaseTest, AckStateUpdate) {
    StoredMessage m;
    m.device = "dev1";
    m.window_kind = "dm";
    m.window_target = 0xABCD;
    m.direction = "out";
    m.from_node = 0xD4A70330;
    m.to_node = 0xABCD;
    m.channel_idx = 0;
    m.text = "DM test";
    m.ts = 1700000001;
    m.packet_id = 99;
    m.ack_state = "pending";
    int64_t rowid = db_.insert_message(m);

    db_.update_ack_state(rowid, "acked");

    WindowKey wk{"dev1", "dm", 0xABCD};
    auto recent = db_.recent_messages(wk);
    ASSERT_EQ(recent.size(), 1u);
    EXPECT_EQ(recent[0].ack_state, "acked");
}

TEST_F(DatabaseTest, RecentMessagesOrderDescByTs) {
    for (int i = 0; i < 5; ++i) {
        StoredMessage m;
        m.device = "dev1";
        m.window_kind = "channel";
        m.window_target = 0;
        m.direction = "in";
        m.text = "msg " + std::to_string(i);
        m.ts = static_cast<uint64_t>(1700000000 + i * 100);
        db_.insert_message(m);
    }
    WindowKey wk{"dev1", "channel", 0};
    auto recent = db_.recent_messages(wk);
    ASSERT_EQ(recent.size(), 5u);
    // recent_messages reverses for chronological order.
    for (int i = 0; i < 5; ++i)
        EXPECT_EQ(recent[i].text, "msg " + std::to_string(i));
}

TEST_F(DatabaseTest, RecentMessagesFiltersByWindowKey) {
    StoredMessage m1;
    m1.device = "dev1"; m1.window_kind = "channel"; m1.window_target = 0;
    m1.direction = "in"; m1.text = "ch0"; m1.ts = 100;
    db_.insert_message(m1);

    StoredMessage m2;
    m2.device = "dev1"; m2.window_kind = "channel"; m2.window_target = 1;
    m2.direction = "in"; m2.text = "ch1"; m2.ts = 200;
    db_.insert_message(m2);

    StoredMessage m3;
    m3.device = "dev2"; m3.window_kind = "channel"; m3.window_target = 0;
    m3.direction = "in"; m3.text = "other"; m3.ts = 300;
    db_.insert_message(m3);

    WindowKey wk{"dev1", "channel", 0};
    auto recent = db_.recent_messages(wk);
    EXPECT_EQ(recent.size(), 1u);
    EXPECT_EQ(recent[0].text, "ch0");
}

TEST_F(DatabaseTest, OkReturnsFalseWhenClosed) {
    db_.close();
    EXPECT_FALSE(db_.ok());
}

// --- survival across reopen -----------------------------------------------

TEST_F(DatabaseTest, DataSurvivesReopen) {
    Node n; n.node_num = 1; n.long_name = "Persistent"; db_.upsert_node("d", n);
    Channel c{0, "main", true, "PRIMARY"}; db_.upsert_channel("d", c);
    StoredMessage m;
    m.device = "d"; m.window_kind = "channel"; m.window_target = 0;
    m.direction = "in"; m.text = "survive"; m.ts = 100;
    db_.insert_message(m);

    db_.close();
    ASSERT_TRUE(db_.open(db_path_));

    NodeDb ndb;
    db_.load_nodes("d", ndb);
    EXPECT_EQ(ndb.all().size(), 1u);

    db_.load_channels("d", ndb);
    EXPECT_EQ(ndb.channels().size(), 1u);

    WindowKey wk{"d", "channel", 0};
    EXPECT_EQ(db_.recent_messages(wk).size(), 1u);
}

// --- location history & deduplication -------------------------------------

TEST_F(DatabaseTest, InsertLocationDeduplication) {
    EXPECT_TRUE(db_.insert_location("dev1", 0x1234, 45.123, 9.456, 120, 1000));
    // Exact duplicate should return false
    EXPECT_FALSE(db_.insert_location("dev1", 0x1234, 45.123, 9.456, 120, 1000));
    // Different timestamp should succeed
    EXPECT_TRUE(db_.insert_location("dev1", 0x1234, 45.124, 9.457, 125, 1001));
    // Different device should succeed
    EXPECT_TRUE(db_.insert_location("dev2", 0x1234, 45.123, 9.456, 120, 1000));
}

TEST_F(DatabaseTest, LocationQueryAndMaxTs) {
    db_.insert_location("dev1", 0x10, 10.0, 20.0, 50, 100);
    db_.insert_location("dev1", 0x10, 10.1, 20.1, 55, 200);
    db_.insert_location("dev1", 0x10, 10.2, 20.2, 60, 300);

    EXPECT_EQ(db_.max_location_ts(), 300u);

    auto locs = db_.get_locations_after(150, 10);
    ASSERT_EQ(locs.size(), 2u);
    EXPECT_EQ(locs[0].ts, 200u);
    EXPECT_FLOAT_EQ(locs[0].latitude, 10.1);
    EXPECT_EQ(locs[1].ts, 300u);
    EXPECT_FLOAT_EQ(locs[1].latitude, 10.2);

    // Limit capped
    auto locs_lim = db_.get_locations_after(50, 1);
    EXPECT_EQ(locs_lim.size(), 1u);
    EXPECT_EQ(locs_lim[0].ts, 100u);
}

// --- devices and windows discovery ----------------------------------------

TEST_F(DatabaseTest, GetAllDevicesAndWindows) {
    Node n; n.node_num = 1; db_.upsert_node("dev_node", n);
    Channel c{0, "main", true, "PRIMARY"}; db_.upsert_channel("dev_chan", c);
    StoredMessage m;
    m.device = "dev_msg"; m.window_kind = "channel"; m.window_target = 0;
    m.direction = "in"; m.text = "hi"; m.ts = 100;
    db_.insert_message(m);

    auto devs = db_.get_all_devices();
    EXPECT_EQ(devs.size(), 3u);
    bool has_node = false, has_chan = false, has_msg = false;
    for (const auto& d : devs) {
        if (d == "dev_node") has_node = true;
        if (d == "dev_chan") has_chan = true;
        if (d == "dev_msg") has_msg = true;
    }
    EXPECT_TRUE(has_node);
    EXPECT_TRUE(has_chan);
    EXPECT_TRUE(has_msg);

    // Add another window to dev_msg
    StoredMessage dm;
    dm.device = "dev_msg"; dm.window_kind = "dm"; dm.window_target = 0x999;
    dm.direction = "out"; dm.text = "private"; dm.ts = 105;
    db_.insert_message(dm);

    auto wins = db_.get_all_windows("dev_msg");
    EXPECT_EQ(wins.size(), 2u);
}

// --- message pagination & max rowid/ts ------------------------------------

TEST_F(DatabaseTest, MessagePaginationAndMaxTs) {
    StoredMessage m1;
    m1.device = "dev"; m1.window_kind = "channel"; m1.window_target = 0;
    m1.direction = "in"; m1.text = "m1"; m1.ts = 10;
    int64_t r1 = db_.insert_message(m1);

    StoredMessage m2 = m1;
    m2.text = "m2"; m2.ts = 20;
    int64_t r2 = db_.insert_message(m2);

    StoredMessage m3 = m1;
    m3.text = "m3"; m3.ts = 30;
    int64_t r3 = db_.insert_message(m3);

    EXPECT_EQ(db_.max_message_rowid(), r3);
    EXPECT_EQ(db_.max_message_ts(), 30u);

    auto after_ts = db_.get_messages_after_ts(15, 10);
    ASSERT_EQ(after_ts.size(), 2u);
    EXPECT_EQ(after_ts[0].text, "m2");
    EXPECT_EQ(after_ts[1].text, "m3");

    auto after_rowid = db_.get_messages_after(r1, 1);
    ASSERT_EQ(after_rowid.size(), 1u);
    EXPECT_EQ(after_rowid[0].rowid, r2);
    EXPECT_EQ(after_rowid[0].text, "m2");
}

TEST_F(DatabaseTest, UnicodeAndSpecialCharacters) {
    StoredMessage m;
    m.device = "dev"; m.window_kind = "channel"; m.window_target = 0;
    m.direction = "in";
    m.text = "📡 Meshtastic testing: 'quotes', \"double quotes\", emoji 🌲🎉";
    m.ts = 12345;
    db_.insert_message(m);

    WindowKey wk{"dev", "channel", 0};
    auto recent = db_.recent_messages(wk, 1);
    ASSERT_EQ(recent.size(), 1u);
    EXPECT_EQ(recent[0].text, "📡 Meshtastic testing: 'quotes', \"double quotes\", emoji 🌲🎉");
}

TEST_F(DatabaseTest, UpsertAndLoadTelemetry) {
    Node n;
    n.node_num = 42;
    n.long_name = "Weather Station";
    n.short_name = "WX";
    n.battery_level = 90;
    n.voltage = 4.15f;
    n.temperature = 23.5f;
    n.relative_humidity = 55.0f;
    n.barometric_pressure = 1013.25f;
    n.channel_util = 0.08f;
    n.air_util_tx = 0.02f;
    n.uptime_seconds = 172800;

    db_.upsert_node("dev1", n);

    // Reopen DB to verify persistence on disk
    db_.close();
    ASSERT_TRUE(db_.open(db_path_));

    NodeDb restored_db;
    db_.load_nodes("dev1", restored_db);

    auto loaded = restored_db.get(42);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->long_name, "Weather Station");
    EXPECT_EQ(loaded->short_name, "WX");
    ASSERT_TRUE(loaded->battery_level.has_value());
    EXPECT_EQ(*loaded->battery_level, 90u);
    ASSERT_TRUE(loaded->voltage.has_value());
    EXPECT_FLOAT_EQ(*loaded->voltage, 4.15f);
    ASSERT_TRUE(loaded->temperature.has_value());
    EXPECT_FLOAT_EQ(*loaded->temperature, 23.5f);
    ASSERT_TRUE(loaded->relative_humidity.has_value());
    EXPECT_FLOAT_EQ(*loaded->relative_humidity, 55.0f);
    ASSERT_TRUE(loaded->barometric_pressure.has_value());
    EXPECT_FLOAT_EQ(*loaded->barometric_pressure, 1013.25f);
    ASSERT_TRUE(loaded->channel_util.has_value());
    EXPECT_FLOAT_EQ(*loaded->channel_util, 0.08f);
    ASSERT_TRUE(loaded->air_util_tx.has_value());
    EXPECT_FLOAT_EQ(*loaded->air_util_tx, 0.02f);
    ASSERT_TRUE(loaded->uptime_seconds.has_value());
    EXPECT_EQ(*loaded->uptime_seconds, 172800u);
}

TEST_F(DatabaseTest, MessageSignalAndHopsPersistence) {
    StoredMessage direct_msg;
    direct_msg.device = "dev1";
    direct_msg.window_kind = "channel";
    direct_msg.window_target = 0;
    direct_msg.direction = "in";
    direct_msg.from_node = 0x12345678;
    direct_msg.to_node = 0xFFFFFFFF;
    direct_msg.channel_idx = 0;
    direct_msg.text = "Direct 0-hop message";
    direct_msg.ts = 1700000001;
    direct_msg.packet_id = 1111;
    direct_msg.rx_snr = 7.5f;
    direct_msg.rx_rssi = -82;
    direct_msg.hop_start = 3;
    direct_msg.hop_limit = 3;
    direct_msg.relay_node = 0;
    int64_t row1 = db_.insert_message(direct_msg);
    EXPECT_GT(row1, 0);

    StoredMessage multi_hop_msg;
    multi_hop_msg.device = "dev1";
    multi_hop_msg.window_kind = "channel";
    multi_hop_msg.window_target = 0;
    multi_hop_msg.direction = "in";
    multi_hop_msg.from_node = 0x87654321;
    multi_hop_msg.to_node = 0xFFFFFFFF;
    multi_hop_msg.channel_idx = 0;
    multi_hop_msg.text = "Relayed message via relay";
    multi_hop_msg.ts = 1700000002;
    multi_hop_msg.packet_id = 2222;
    multi_hop_msg.rx_snr = 3.2f;
    multi_hop_msg.rx_rssi = -95;
    multi_hop_msg.hop_start = 3;
    multi_hop_msg.hop_limit = 1; // 2 hops
    multi_hop_msg.relay_node = 0x30;
    int64_t row2 = db_.insert_message(multi_hop_msg);
    EXPECT_GT(row2, 0);

    // Reopen DB to verify round-trip to disk
    db_.close();
    ASSERT_TRUE(db_.open(db_path_));

    WindowKey wk{"dev1", "channel", 0};
    auto msgs = db_.recent_messages(wk);
    ASSERT_EQ(msgs.size(), 2u);

    EXPECT_EQ(msgs[0].text, "Direct 0-hop message");
    EXPECT_FLOAT_EQ(msgs[0].rx_snr, 7.5f);
    EXPECT_EQ(msgs[0].rx_rssi, -82);
    EXPECT_EQ(msgs[0].hop_start, 3u);
    EXPECT_EQ(msgs[0].hop_limit, 3u);
    EXPECT_EQ(msgs[0].relay_node, 0u);

    EXPECT_EQ(msgs[1].text, "Relayed message via relay");
    EXPECT_FLOAT_EQ(msgs[1].rx_snr, 3.2f);
    EXPECT_EQ(msgs[1].rx_rssi, -95);
    EXPECT_EQ(msgs[1].hop_start, 3u);
    EXPECT_EQ(msgs[1].hop_limit, 1u);
    EXPECT_EQ(msgs[1].relay_node, 0x30u);

    // Also test find_by_packet_id
    auto p1 = db_.find_by_packet_id(1111);
    ASSERT_TRUE(p1.has_value());
    EXPECT_FLOAT_EQ(p1->rx_snr, 7.5f);
    EXPECT_EQ(p1->rx_rssi, -82);

    auto p2 = db_.find_by_packet_id(2222);
    ASSERT_TRUE(p2.has_value());
    EXPECT_EQ(p2->relay_node, 0x30u);
    EXPECT_EQ(p2->hop_start, 3u);
    EXPECT_EQ(p2->hop_limit, 1u);
}

TEST_F(DatabaseTest, RecentMessagesDeviceFallback) {
    // Insert channel broadcast with empty device string
    StoredMessage ch_msg;
    ch_msg.device = "";
    ch_msg.window_kind = "channel";
    ch_msg.window_target = 0;
    ch_msg.direction = "in";
    ch_msg.from_node = 0x1234;
    ch_msg.to_node = 0xFFFFFFFF;
    ch_msg.channel_idx = 0;
    ch_msg.text = "Broadcast on empty device";
    ch_msg.ts = 1000;
    db_.insert_message(ch_msg);

    // Insert private DM message with empty device string
    StoredMessage dm_msg;
    dm_msg.device = "";
    dm_msg.window_kind = "dm";
    dm_msg.window_target = 0x5678;
    dm_msg.direction = "in";
    dm_msg.from_node = 0x5678;
    dm_msg.to_node = 0x1234;
    dm_msg.channel_idx = 0;
    dm_msg.text = "Private DM on empty device";
    dm_msg.ts = 1010;
    db_.insert_message(dm_msg);

    // Query with concrete device string that doesn't match directly
    WindowKey ch_key{"stream:mesh:192.168.178.23:4404", "channel", 0};
    auto ch_recent = db_.recent_messages(ch_key, 50);
    ASSERT_EQ(ch_recent.size(), 1u);
    EXPECT_EQ(ch_recent[0].text, "Broadcast on empty device");

    auto ch_paginated = db_.get_messages_paginated(ch_key, 50, 0);
    ASSERT_EQ(ch_paginated.size(), 1u);
    EXPECT_EQ(ch_paginated[0].text, "Broadcast on empty device");

    WindowKey dm_key{"stream:mesh:192.168.178.23:4404", "dm", 0x5678};
    auto dm_recent = db_.recent_messages(dm_key, 50);
    ASSERT_EQ(dm_recent.size(), 1u);
    EXPECT_EQ(dm_recent[0].text, "Private DM on empty device");

    auto dm_paginated = db_.get_messages_paginated(dm_key, 50, 0);
    ASSERT_EQ(dm_paginated.size(), 1u);
    EXPECT_EQ(dm_paginated[0].text, "Private DM on empty device");
}

TEST_F(DatabaseTest, GetAllDevicesFiltersEmpty) {
    StoredMessage m;
    m.device = "";
    m.window_kind = "channel";
    m.window_target = 0;
    m.text = "empty device msg";
    db_.insert_message(m);

    Node n;
    n.node_num = 123;
    n.long_name = "Node on Dev1";
    db_.upsert_node("dev1", n);

    auto devs = db_.get_all_devices();
    ASSERT_EQ(devs.size(), 1u);
    EXPECT_EQ(devs[0], "dev1");
}

TEST_F(DatabaseTest, GetNodeAnyDevice) {
    Node n;
    n.node_num = 0x55aa;
    n.node_id = "!000055aa";
    n.long_name = "Global Node";
    n.short_name = "GN";
    db_.upsert_node("dev_xyz", n);

    auto found = db_.get_node_any_device(0x55aa);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->long_name, "Global Node");
    EXPECT_EQ(found->short_name, "GN");

    auto not_found = db_.get_node_any_device(0x9999);
    EXPECT_FALSE(not_found.has_value());
}

TEST_F(DatabaseTest, InsertAndQueryNodeTelemetry) {
    Database::TelemetryRow r1;
    r1.device = "dev1";
    r1.node_num = 0x12345678;
    r1.ts = 1000;
    r1.temperature = 22.5f;
    r1.relative_humidity = 45.0f;
    r1.barometric_pressure = 1013.25f;
    r1.battery_level = 98;
    r1.voltage = 4.15f;
    r1.channel_util = 5.4f;

    ASSERT_TRUE(db_.insert_telemetry(r1));

    Database::TelemetryRow r2;
    r2.device = "dev1";
    r2.node_num = 0x12345678;
    r2.ts = 1060;
    r2.temperature = 23.0f;
    r2.relative_humidity = 44.5f;
    r2.battery_level = 97;

    ASSERT_TRUE(db_.insert_telemetry(r2));

    auto rows = db_.get_node_telemetry(0x12345678, 900, 1100, 50);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].ts, 1000u);
    ASSERT_TRUE(rows[0].temperature.has_value());
    EXPECT_FLOAT_EQ(*rows[0].temperature, 22.5f);
    ASSERT_TRUE(rows[0].battery_level.has_value());
    EXPECT_EQ(*rows[0].battery_level, 98);
    ASSERT_TRUE(rows[0].barometric_pressure.has_value());
    EXPECT_FLOAT_EQ(*rows[0].barometric_pressure, 1013.25f);

    EXPECT_EQ(rows[1].ts, 1060u);
    ASSERT_TRUE(rows[1].temperature.has_value());
    EXPECT_FLOAT_EQ(*rows[1].temperature, 23.0f);
}

TEST_F(DatabaseTest, TelemetryUpsertMergesOnSameTimestamp) {
    Database::TelemetryRow r1;
    r1.device = "dev1";
    r1.node_num = 0xABCD;
    r1.ts = 2000;
    r1.battery_level = 85;
    r1.voltage = 3.95f;
    ASSERT_TRUE(db_.insert_telemetry(r1));

    // Another packet arrives for same node at same second with environment data
    Database::TelemetryRow r2;
    r2.device = "dev1";
    r2.node_num = 0xABCD;
    r2.ts = 2000;
    r2.temperature = 18.2f;
    r2.relative_humidity = 60.0f;
    ASSERT_TRUE(db_.insert_telemetry(r2));

    auto rows = db_.get_node_telemetry(0xABCD);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].ts, 2000u);
    ASSERT_TRUE(rows[0].battery_level.has_value());
    EXPECT_EQ(*rows[0].battery_level, 85);
    ASSERT_TRUE(rows[0].voltage.has_value());
    EXPECT_FLOAT_EQ(*rows[0].voltage, 3.95f);
    ASSERT_TRUE(rows[0].temperature.has_value());
    EXPECT_FLOAT_EQ(*rows[0].temperature, 18.2f);
    ASSERT_TRUE(rows[0].relative_humidity.has_value());
    EXPECT_FLOAT_EQ(*rows[0].relative_humidity, 60.0f);
}

TEST_F(DatabaseTest, GetRecentTelemetryAndMaxTs) {
    Database::TelemetryRow r1;
    r1.device = "dev1";
    r1.node_num = 0x1111;
    r1.ts = 500;
    r1.temperature = 10.0f;
    db_.insert_telemetry(r1);

    Database::TelemetryRow r2;
    r2.device = "dev1";
    r2.node_num = 0x2222;
    r2.ts = 600;
    r2.temperature = 12.5f;
    db_.insert_telemetry(r2);

    EXPECT_EQ(db_.max_telemetry_ts(), 600u);

    auto recent = db_.get_recent_telemetry(0, 10);
    ASSERT_TRUE(recent.size() >= 2u);

    auto after = db_.get_telemetry_after_ts(550, 10);
    ASSERT_EQ(after.size(), 1u);
    EXPECT_EQ(after[0].node_num, 0x2222u);
}



