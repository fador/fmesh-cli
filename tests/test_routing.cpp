#include "mesh/node_db.h"

#include <gtest/gtest.h>

using namespace meshcli;

TEST(NodeDb, NodeIdRoundtrip) {
    EXPECT_EQ(node_num_to_id(0x1234abcd), "!1234abcd");
    uint32_t n = 0;
    ASSERT_TRUE(parse_node_id("!1234abcd", n));
    EXPECT_EQ(n, 0x1234abcdu);
    ASSERT_TRUE(parse_node_id("0xdeadbeef", n));
    EXPECT_EQ(n, 0xdeadbeefu);
    ASSERT_TRUE(parse_node_id("305419896", n));
    EXPECT_EQ(n, 0x12345678u);
    EXPECT_FALSE(parse_node_id("garbage", n));
}

TEST(NodeDb, UpsertAndGet) {
    NodeDb db;
    Node n;
    n.node_num = 0xabc;
    n.long_name = "Alice";
    n.short_name = "A";
    db.upsert_node(n);
    auto got = db.get(0xabc);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->long_name, "Alice");
    EXPECT_EQ(got->node_id, "!00000abc");

    // Update existing.
    n.long_name = "Alice2";
    db.upsert_node(n);
    got = db.get(0xabc);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->long_name, "Alice2");
}

TEST(NodeDb, FuzzyFind) {
    NodeDb db;
    Node a; a.node_num = 1; a.long_name = "Alice";   a.short_name = "A";
    Node b; b.node_num = 2; b.long_name = "Bob";     b.short_name = "B";
    Node c; c.node_num = 3; c.long_name = "Charlie"; c.short_name = "C";
    db.upsert_node(a);
    db.upsert_node(b);
    db.upsert_node(c);

    auto m = db.find_fuzzy("bob");
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->node_num, 2u);

    m = db.find_fuzzy("!00000001");
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->node_num, 1u);

    m = db.find_fuzzy("z");
    EXPECT_FALSE(m.has_value());
}

TEST(NodeDb, Channels) {
    NodeDb db;
    Channel ch{0, "primary", true, "PRIMARY"};
    db.upsert_channel(ch);
    auto got = db.channel(0);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->name, "primary");
    EXPECT_EQ(got->role, "PRIMARY");
    EXPECT_FALSE(db.channel(1).has_value());
}
