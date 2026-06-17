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
                      "/tmp/mesh-cli-test-db-%d.db",
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
