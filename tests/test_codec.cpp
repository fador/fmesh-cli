#include "mesh/mesh_codec.h"
#include "mesh/node_db.h"

#include <meshtastic/mesh.pb.h>
#include <meshtastic/portnums.pb.h>

#include <gtest/gtest.h>

using namespace meshcli;

TEST(MeshCodec, EncodeWantConfigRoundtrips) {
    auto bytes = MeshCodec::encode_want_config(0x12345);
    ASSERT_FALSE(bytes.empty());
    // Parse it back as a ToRadio.
    meshtastic::ToRadio tr;
    ASSERT_TRUE(tr.ParseFromString(bytes));
    ASSERT_EQ(tr.payload_variant_case(), meshtastic::ToRadio::kWantConfigId);
    EXPECT_EQ(tr.want_config_id(), 0x12345u);
}

TEST(MeshCodec, EncodeTextPacketBroadcast) {
    auto bytes = MeshCodec::encode_text_packet(
        0xABCD, kBroadcastNodeNum, /*channel_idx=*/0, "hello mesh",
        /*want_ack=*/false, /*hop_limit=*/3, /*pki_pubkey=*/{});
    meshtastic::ToRadio tr;
    ASSERT_TRUE(tr.ParseFromString(bytes));
    ASSERT_TRUE(tr.has_packet());
    EXPECT_EQ(tr.packet().to(), kBroadcastNodeNum);
    EXPECT_EQ(tr.packet().channel(), 0u);
    EXPECT_EQ(tr.packet().decoded().portnum(), meshtastic::PortNum::TEXT_MESSAGE_APP);
    EXPECT_EQ(tr.packet().decoded().payload(), "hello mesh");
    EXPECT_EQ(tr.packet().hop_limit(), 3u);
}

TEST(MeshCodec, EncodeTextPacketDmWithPubkey) {
    std::vector<uint8_t> pk = {1, 2, 3, 4, 5};
    auto bytes = MeshCodec::encode_text_packet(
        0x1, 0xdeadbeef, /*channel_idx=*/0, "private", true, 0, pk);
    meshtastic::ToRadio tr;
    ASSERT_TRUE(tr.ParseFromString(bytes));
    EXPECT_EQ(tr.packet().to(), 0xdeadbeefu);
    EXPECT_TRUE(tr.packet().pki_encrypted());
    EXPECT_EQ(tr.packet().public_key(), std::string("\x01\x02\x03\x04\x05", 5));
}

TEST(MeshCodec, DecodeTextReceived) {
    // Build a FromRadio{packet} with a text payload and decode it.
    meshtastic::FromRadio fr;
    auto* pkt = fr.mutable_packet();
    pkt->set_from(0x1234);
    pkt->set_to(kBroadcastNodeNum);
    pkt->set_channel(0);
    pkt->set_id(0x7777);
    pkt->set_rx_time(1700000000);
    pkt->set_rx_snr(7.5f);
    pkt->mutable_decoded()->set_portnum(meshtastic::PortNum::TEXT_MESSAGE_APP);
    pkt->mutable_decoded()->set_payload("hi");

    std::string bytes = fr.SerializeAsString();
    uint32_t config_id = 0;
    auto ev = MeshCodec::decode_from_radio(bytes, "dev1", config_id);
    ASSERT_TRUE(ev.has_value());
    auto* t = std::get_if<EvTextReceived>(&*ev);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->from_node, 0x1234u);
    EXPECT_EQ(t->to_node, kBroadcastNodeNum);
    EXPECT_TRUE(t->broadcast);
    EXPECT_EQ(t->text, "hi");
    EXPECT_EQ(t->packet_id, 0x7777u);
    EXPECT_EQ(t->rx_time, 1700000000u);
}

TEST(MeshCodec, DecodeConfigComplete) {
    meshtastic::FromRadio fr;
    fr.set_config_complete_id(42);
    // Note: config_complete_id and rebooted share a oneof, so they're mutually
    // exclusive on the wire. rebooted arrives as its own message.
    uint32_t config_id = 0;
    auto ev = MeshCodec::decode_from_radio(fr.SerializeAsString(), "dev1", config_id);
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(config_id, 42u);
    auto* c = std::get_if<EvConfigComplete>(&*ev);
    ASSERT_NE(c, nullptr);
}

TEST(MeshCodec, DecodeRebooted) {
    meshtastic::FromRadio fr;
    fr.set_rebooted(true);
    uint32_t config_id = 0;
    auto ev = MeshCodec::decode_from_radio(fr.SerializeAsString(), "dev1", config_id);
    // rebooted as a standalone message is currently dropped (not surfaced).
    EXPECT_FALSE(ev.has_value());
}
