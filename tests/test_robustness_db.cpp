#include "store/database.h"
#include "mesh/node_db.h"

#include "minitest.h"

#include <cstdio>
#include <random>

using namespace meshcli;

// -- Database error paths and edge cases --

struct DbRobust {
    Database db;
    std::string tmp_path;

    void SetUp() {
        int r = std::random_device{}();
        tmp_path = "mesh-cli-test-robust-" + std::to_string(r) + ".db";
        std::remove(tmp_path.c_str());
        db.open(tmp_path);
    }

    void TearDown() {
        db.close();
        std::remove(tmp_path.c_str());
    }
};

TEST_F(DbRobust, OperationsOnClosedDb) {
    TearDown(); // close the db
    Node n; n.node_num = 1; n.long_name = "Test";
    db.upsert_node("dev", n);       // should not crash
    Channel c{0, "test", true, "PRIMARY"};
    db.upsert_channel("dev", c);    // should not crash
    auto rm = db.insert_message(StoredMessage{}); // should return 0
    EXPECT_EQ(rm, 0);
    db.update_ack_state(1, "acked"); // should not crash
    EXPECT_FALSE(db.ok());
    EXPECT_FALSE(db.find_by_packet_id(42).has_value());
}

TEST_F(DbRobust, InsertNullFields) {
    // Nodes with empty optional fields should store/load cleanly.
    Node n;
    n.node_num = 42;
    n.long_name = "Minimal";
    n.short_name = "";
    n.node_id = "";
    n.hw_model = "";
    n.role = "";
    db.upsert_node("dev", n);

    NodeDb ndb;
    db.load_nodes("dev", ndb);
    auto got = ndb.get(42);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->long_name, "Minimal");
    EXPECT_TRUE(got->short_name.empty());
}

TEST_F(DbRobust, LoadNodesForUnknownDevice) {
    NodeDb ndb;
    db.load_nodes("unknown_device_xyz", ndb);
    EXPECT_TRUE(ndb.all().empty());
}

TEST_F(DbRobust, LoadChannelsForUnknownDevice) {
    NodeDb ndb;
    db.load_channels("unknown_device_xyz", ndb);
    EXPECT_TRUE(ndb.channels().empty());
}

TEST_F(DbRobust, RecentMessagesNoResults) {
    WindowKey wk{"dev", "channel", 99};
    auto msgs = db.recent_messages(wk, 10);
    EXPECT_TRUE(msgs.empty());
}

TEST_F(DbRobust, AckUpdateUnknownRow) {
    db.update_ack_state(999999, "acked"); // should not crash
}

TEST_F(DbRobust, MultipleOpsStability) {
    // Rapid sequences of upserts and loads.
    for (int i = 0; i < 50; ++i) {
        Node n;
        n.node_num = static_cast<uint32_t>(i);
        n.long_name = "Node" + std::to_string(i);
        n.short_name = "N" + std::to_string(i);
        db.upsert_node("dev", n);
    }
    NodeDb ndb;
    db.load_nodes("dev", ndb);
    EXPECT_EQ(ndb.all().size(), 50u);

    // Verify all survived.
    for (int i = 0; i < 50; ++i) {
        auto got = ndb.get(static_cast<uint32_t>(i));
        ASSERT_TRUE(got.has_value());
        EXPECT_EQ(got->long_name, "Node" + std::to_string(i));
    }
}

TEST_F(DbRobust, LargeMessageText) {
    std::string big(10000, 'A');
    StoredMessage m;
    m.device = "dev";
    m.window_kind = "channel";
    m.window_target = 0;
    m.direction = "in";
    m.from_node = 1;
    m.to_node = 0xFFFFFFFFu;
    m.text = big;
    m.ts = 1000;
    m.packet_id = 123;
    int64_t rowid = db.insert_message(m);
    EXPECT_GT(rowid, 0);

    WindowKey wk{"dev", "channel", 0};
    auto msgs = db.recent_messages(wk, 1);
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].text, big);
}

TEST_F(DbRobust, DoubleOpenSamePath) {
    // Opening again while already open should be fine (SQLite handles it).
    Database db2;
    EXPECT_TRUE(db2.open(tmp_path));
    db2.close();
}

TEST_F(DbRobust, CheckpointAfterManyWrites) {
    // Insert enough messages to trigger the periodic checkpoint.
    for (int i = 0; i < 120; ++i) {
        StoredMessage m;
        m.device = "dev";
        m.window_kind = "channel";
        m.window_target = 0;
        m.direction = "in";
        m.from_node = 1;
        m.text = "msg" + std::to_string(i);
        m.ts = i;
        m.packet_id = static_cast<uint32_t>(i);
        int64_t rowid = db.insert_message(m);
        EXPECT_GT(rowid, 0);
    }
    // Explicit checkpoint.
    db.checkpoint();
}

// -- NodeDb edge cases --

TEST_F(DbRobust, NodeDbFuzzyFindOnEmpty) {
    NodeDb ndb;
    auto result = ndb.find_fuzzy("anything");
    EXPECT_FALSE(result.has_value());
}

TEST_F(DbRobust, NodeDbGetOnEmpty) {
    NodeDb ndb;
    EXPECT_FALSE(ndb.get(0).has_value());
    EXPECT_FALSE(ndb.get(42).has_value());
}

TEST_F(DbRobust, NodeDbChannelOnEmpty) {
    NodeDb ndb;
    EXPECT_FALSE(ndb.channel(0).has_value());
}

TEST_F(DbRobust, NodeDbGetByIdInvalid) {
    NodeDb ndb;
    EXPECT_FALSE(ndb.get_by_id("not_a_node_id").has_value());
}

TEST_F(DbRobust, NodeDbUpsertSameNodeTwice) {
    NodeDb ndb;
    Node n;
    n.node_num = 7;
    n.long_name = "First";
    ndb.upsert_node(n);
    n.long_name = "Second";
    ndb.upsert_node(n);
    auto got = ndb.get(7);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->long_name, "Second");
    EXPECT_EQ(ndb.all().size(), 1u);
}

// -- WindowKey equality --
TEST(DbTypes, WindowKeyEquality) {
    WindowKey a{"dev", "channel", 0};
    WindowKey b{"dev", "channel", 0};
    WindowKey c{"dev", "channel", 1};
    WindowKey d{"dev2", "channel", 0};
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a == c);
    EXPECT_FALSE(a == d);
}
