#include "mesh/mesh_codec.h"
#include "mesh/node_db.h"

#include <meshtastic/admin.pb.h>
#include <meshtastic/config.pb.h>
#include <meshtastic/mesh.pb.h>
#include <meshtastic/module_config.pb.h>
#include <meshtastic/portnums.pb.h>
#include <meshtastic/telemetry.pb.h>

#include "minitest.h"

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

TEST(MeshCodec, DecodePositionPacket) {
    meshtastic::FromRadio fr;
    auto* pkt = fr.mutable_packet();
    pkt->set_from(0x1234);
    pkt->set_to(kBroadcastNodeNum);
    
    meshtastic::Position p;
    p.set_latitude_i(488583700);   // 48.85837
    p.set_longitude_i(22944810);   // 2.294481
    p.set_altitude(300);
    
    auto* dec = pkt->mutable_decoded();
    dec->set_portnum(meshtastic::PortNum::POSITION_APP);
    dec->set_payload(p.SerializeAsString());
    
    uint32_t config_id = 0;
    auto ev = MeshCodec::decode_from_radio(fr.SerializeAsString(), "dev1", config_id);
    ASSERT_TRUE(ev.has_value());
    
    auto* position = std::get_if<EvPositionReceived>(&ev.value());
    ASSERT_TRUE(position != nullptr);
    EXPECT_EQ(position->device, "dev1");
    EXPECT_EQ(position->from_node, 0x1234);
    EXPECT_TRUE(std::abs(position->latitude - 48.85837) < 0.00001);
    EXPECT_TRUE(std::abs(position->longitude - 2.294481) < 0.00001);
    EXPECT_EQ(position->altitude, 300);
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

TEST(MeshCodec, EncodeDisconnect) {
    auto bytes = MeshCodec::encode_disconnect();
    ASSERT_FALSE(bytes.empty());
    meshtastic::ToRadio tr;
    ASSERT_TRUE(tr.ParseFromString(bytes));
    EXPECT_EQ(tr.payload_variant_case(), meshtastic::ToRadio::kDisconnect);
    EXPECT_TRUE(tr.disconnect());
}

TEST(MeshCodec, EncodeAdminPacket) {
    meshtastic::Config cfg;
    cfg.mutable_lora()->set_tx_power(18);
    std::string cfg_bytes = cfg.SerializeAsString();

    auto bytes = MeshCodec::encode_admin_packet(0x100, 0x200, cfg_bytes, false);
    ASSERT_FALSE(bytes.empty());
    meshtastic::ToRadio tr;
    ASSERT_TRUE(tr.ParseFromString(bytes));
    ASSERT_TRUE(tr.has_packet());
    EXPECT_EQ(tr.packet().from(), 0x100u);
    EXPECT_EQ(tr.packet().to(), 0x200u);
    EXPECT_TRUE(tr.packet().want_ack());
    EXPECT_EQ(tr.packet().decoded().portnum(), meshtastic::PortNum::ADMIN_APP);

    meshtastic::AdminMessage admin;
    ASSERT_TRUE(admin.ParseFromString(tr.packet().decoded().payload()));
    ASSERT_TRUE(admin.has_set_config());
    EXPECT_EQ(admin.set_config().lora().tx_power(), 18);

    // Module config variant
    meshtastic::ModuleConfig mod;
    mod.mutable_mqtt()->set_enabled(true);
    auto mod_bytes = MeshCodec::encode_admin_packet(0x100, 0x200, mod.SerializeAsString(), true);
    meshtastic::ToRadio tr_mod;
    ASSERT_TRUE(tr_mod.ParseFromString(mod_bytes));
    meshtastic::AdminMessage admin_mod;
    ASSERT_TRUE(admin_mod.ParseFromString(tr_mod.packet().decoded().payload()));
    EXPECT_TRUE(admin_mod.has_set_module_config());
    EXPECT_TRUE(admin_mod.set_module_config().mqtt().enabled());
}

TEST(MeshCodec, DecodeConfigLines) {
    meshtastic::Config cfg_lora;
    cfg_lora.mutable_lora()->set_tx_power(20);
    cfg_lora.mutable_lora()->set_channel_num(3);

    auto lines = MeshCodec::decode_config_lines(cfg_lora.SerializeAsString(), false);
    ASSERT_FALSE(lines.empty());
    bool found_power = false;
    for (const auto& l : lines) {
        if (l.find("lora.tx_power = 20") != std::string::npos) found_power = true;
    }
    EXPECT_TRUE(found_power);

    meshtastic::Config cfg_dev;
    cfg_dev.mutable_device()->set_serial_enabled(true);
    auto dev_lines = MeshCodec::decode_config_lines(cfg_dev.SerializeAsString(), false);
    ASSERT_FALSE(dev_lines.empty());
    bool found_serial = false;
    for (const auto& l : dev_lines) {
        if (l.find("device.serial_enabled = ON") != std::string::npos) found_serial = true;
    }
    EXPECT_TRUE(found_serial);

    meshtastic::ModuleConfig mod;
    mod.mutable_mqtt()->set_enabled(true);
    mod.mutable_mqtt()->set_address("mqtt.example.com");
    auto mod_lines = MeshCodec::decode_config_lines(mod.SerializeAsString(), true);
    ASSERT_FALSE(mod_lines.empty());
    bool found_mqtt = false;
    for (const auto& l : mod_lines) {
        if (l.find("mqtt.enabled = ON") != std::string::npos) found_mqtt = true;
    }
    EXPECT_TRUE(found_mqtt);
}

TEST(MeshCodec, DecodeFromRadioMyInfo) {
    meshtastic::FromRadio fr;
    fr.mutable_my_info()->set_my_node_num(0x12345678);
    uint32_t config_id = 0;
    auto ev = MeshCodec::decode_from_radio(fr.SerializeAsString(), "dev1", config_id);
    ASSERT_TRUE(ev.has_value());
    auto* info = std::get_if<EvMyInfo>(&*ev);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->device, "dev1");
    EXPECT_EQ(info->my_node_num, 0x12345678u);
}

TEST(MeshCodec, DecodeFromRadioMetadata) {
    meshtastic::FromRadio fr;
    auto* meta = fr.mutable_metadata();
    meta->set_firmware_version("2.5.1");
    meta->set_hw_model(meshtastic::TBEAM);
    uint32_t config_id = 0;
    auto ev = MeshCodec::decode_from_radio(fr.SerializeAsString(), "dev1", config_id);
    ASSERT_TRUE(ev.has_value());
    auto* m = std::get_if<EvMetadata>(&*ev);
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->firmware_version, "2.5.1");
    EXPECT_EQ(m->hw_model, "TBEAM");
}

TEST(MeshCodec, DecodeFromRadioNodeInfo) {
    meshtastic::FromRadio fr;
    auto* ni = fr.mutable_node_info();
    ni->set_num(0xAABBCCDD);
    ni->mutable_user()->set_long_name("Test Long");
    ni->mutable_user()->set_short_name("TL");
    ni->mutable_user()->set_public_key("keybytes");
    ni->set_is_favorite(true);
    ni->set_is_muted(true);
    ni->mutable_device_metrics()->set_battery_level(88);
    ni->mutable_device_metrics()->set_voltage(4.05f);
    ni->set_snr(8.5f);
    ni->set_hops_away(2);

    uint32_t config_id = 0;
    auto ev = MeshCodec::decode_from_radio(fr.SerializeAsString(), "dev1", config_id);
    ASSERT_TRUE(ev.has_value());
    auto* upd = std::get_if<EvNodeUpdated>(&*ev);
    ASSERT_NE(upd, nullptr);
    EXPECT_EQ(upd->node.node_num, 0xAABBCCDDu);
    EXPECT_EQ(upd->node.long_name, "Test Long");
    EXPECT_EQ(upd->node.short_name, "TL");
    EXPECT_TRUE(upd->node.has_public_key);
    EXPECT_TRUE(upd->node.is_favorite);
    EXPECT_TRUE(upd->node.is_muted);
    ASSERT_TRUE(upd->node.battery_level.has_value());
    EXPECT_EQ(*upd->node.battery_level, 88u);
    EXPECT_FLOAT_EQ(upd->node.voltage.value(), 4.05f);
    EXPECT_FLOAT_EQ(upd->node.snr.value(), 8.5f);
    ASSERT_TRUE(upd->node.hops_away.has_value());
    EXPECT_EQ(*upd->node.hops_away, 2u);
}

TEST(MeshCodec, DecodeFromRadioChannel) {
    meshtastic::FromRadio fr;
    auto* ch = fr.mutable_channel();
    ch->set_index(0);
    ch->mutable_settings()->set_name("LongFast");
    ch->mutable_settings()->set_psk("default");
    ch->set_role(meshtastic::Channel_Role_PRIMARY);

    uint32_t config_id = 0;
    auto ev = MeshCodec::decode_from_radio(fr.SerializeAsString(), "dev1", config_id);
    ASSERT_TRUE(ev.has_value());
    auto* c = std::get_if<EvChannelUpdated>(&*ev);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->channel.index, 0u);
    EXPECT_EQ(c->channel.name, "LongFast");
    EXPECT_EQ(c->channel.role, "PRIMARY");
    EXPECT_TRUE(c->channel.has_psk);
}

TEST(MeshCodec, DecodeFromRadioLogRecord) {
    meshtastic::FromRadio fr;
    fr.mutable_log_record()->set_message("Radio packet rx");
    fr.mutable_log_record()->set_source("mesh");

    uint32_t config_id = 0;
    auto ev = MeshCodec::decode_from_radio(fr.SerializeAsString(), "dev1", config_id);
    ASSERT_TRUE(ev.has_value());
    auto* l = std::get_if<EvLogLine>(&*ev);
    ASSERT_NE(l, nullptr);
    EXPECT_EQ(l->message, "Radio packet rx");
    EXPECT_EQ(l->source, "mesh");
}

TEST(MeshCodec, DecodeRoutingAckAndNak) {
    // 1. ACK
    meshtastic::FromRadio fr_ack;
    auto* pkt_ack = fr_ack.mutable_packet();
    pkt_ack->set_from(0x1111);
    pkt_ack->set_id(0x5555);
    auto* dec_ack = pkt_ack->mutable_decoded();
    dec_ack->set_portnum(meshtastic::PortNum::ROUTING_APP);
    dec_ack->set_request_id(0x4444);
    meshtastic::Routing r_ack;
    r_ack.set_error_reason(meshtastic::Routing_Error_NONE);
    dec_ack->set_payload(r_ack.SerializeAsString());

    uint32_t config_id = 0;
    auto ev_ack = MeshCodec::decode_from_radio(fr_ack.SerializeAsString(), "dev1", config_id);
    ASSERT_TRUE(ev_ack.has_value());
    auto* a = std::get_if<EvAckReceived>(&*ev_ack);
    ASSERT_NE(a, nullptr);
    EXPECT_TRUE(a->success);
    EXPECT_EQ(a->packet_id, 0x4444u);
    EXPECT_EQ(a->from_node, 0x1111u);

    // 2. NAK with NO_ROUTE
    meshtastic::FromRadio fr_nak;
    auto* pkt_nak = fr_nak.mutable_packet();
    pkt_nak->set_from(0x1111);
    pkt_nak->set_id(0x6666);
    auto* dec_nak = pkt_nak->mutable_decoded();
    dec_nak->set_portnum(meshtastic::PortNum::ROUTING_APP);
    meshtastic::Routing r_nak;
    r_nak.set_error_reason(meshtastic::Routing_Error_NO_ROUTE);
    dec_nak->set_payload(r_nak.SerializeAsString());

    auto ev_nak = MeshCodec::decode_from_radio(fr_nak.SerializeAsString(), "dev1", config_id);
    ASSERT_TRUE(ev_nak.has_value());
    auto* nak = std::get_if<EvAckReceived>(&*ev_nak);
    ASSERT_NE(nak, nullptr);
    EXPECT_FALSE(nak->success);
    EXPECT_EQ(nak->error_reason, "NO_ROUTE");
}

TEST(MeshCodec, DecodeTelemetryPacket) {
    meshtastic::FromRadio fr;
    auto* pkt = fr.mutable_packet();
    pkt->set_from(0xCAFE);
    pkt->set_rx_time(1705000000);
    pkt->set_rx_snr(10.5f);
    pkt->set_hop_start(3);
    pkt->set_hop_limit(1);

    meshtastic::Telemetry t;
    t.mutable_device_metrics()->set_battery_level(95);
    t.mutable_device_metrics()->set_voltage(4.18f);
    t.mutable_device_metrics()->set_channel_utilization(12.5f);
    pkt->mutable_decoded()->set_portnum(meshtastic::PortNum::TELEMETRY_APP);
    pkt->mutable_decoded()->set_payload(t.SerializeAsString());

    uint32_t config_id = 0;
    auto ev = MeshCodec::decode_from_radio(fr.SerializeAsString(), "dev1", config_id);
    ASSERT_TRUE(ev.has_value());
    auto* upd = std::get_if<EvNodeUpdated>(&*ev);
    ASSERT_NE(upd, nullptr);
    EXPECT_EQ(upd->node.node_num, 0xCAFEu);
    ASSERT_TRUE(upd->node.battery_level.has_value());
    EXPECT_EQ(*upd->node.battery_level, 95u);
    EXPECT_FLOAT_EQ(upd->node.voltage.value(), 4.18f);
    EXPECT_FLOAT_EQ(upd->node.channel_util.value(), 12.5f);
    EXPECT_FLOAT_EQ(upd->node.snr.value(), 10.5f);
    ASSERT_TRUE(upd->node.hops_away.has_value());
    EXPECT_EQ(*upd->node.hops_away, 2u); // 3 - 1
    EXPECT_EQ(upd->node.last_heard.value(), 1705000000u);
}

TEST(MeshCodec, FromRadioSummaryVariants) {
    meshtastic::FromRadio fr;
    fr.mutable_my_info()->set_my_node_num(0x1234abcd);
    EXPECT_NE(MeshCodec::from_radio_summary(fr.SerializeAsString()).find("MyInfo"), std::string::npos);

    fr.Clear();
    fr.mutable_node_info()->set_num(0x11223344);
    EXPECT_NE(MeshCodec::from_radio_summary(fr.SerializeAsString()).find("NodeInfo"), std::string::npos);

    fr.Clear();
    fr.mutable_channel()->set_index(2);
    fr.mutable_channel()->mutable_settings()->set_name("Ch2");
    EXPECT_NE(MeshCodec::from_radio_summary(fr.SerializeAsString()).find("Channel idx=2"), std::string::npos);

    fr.Clear();
    fr.mutable_metadata()->set_firmware_version("2.5.0");
    EXPECT_NE(MeshCodec::from_radio_summary(fr.SerializeAsString()).find("Metadata fw=2.5.0"), std::string::npos);

    fr.Clear();
    fr.set_config_complete_id(999);
    EXPECT_NE(MeshCodec::from_radio_summary(fr.SerializeAsString()).find("ConfigComplete id=999"), std::string::npos);

    fr.Clear();
    fr.mutable_log_record()->set_message("log text");
    EXPECT_NE(MeshCodec::from_radio_summary(fr.SerializeAsString()).find("LogRecord"), std::string::npos);
}

TEST(MeshCodec, EncodeTraceroutePacket) {
    auto bytes = MeshCodec::encode_traceroute_packet(101, 0x11112222, 0x33334444, 0, 5);
    meshtastic::ToRadio tr;
    ASSERT_TRUE(tr.ParseFromString(bytes));
    ASSERT_TRUE(tr.has_packet());
    const auto& pkt = tr.packet();
    EXPECT_EQ(pkt.id(), 101u);
    EXPECT_EQ(pkt.from(), 0x11112222u);
    EXPECT_EQ(pkt.to(), 0x33334444u);
    EXPECT_EQ(pkt.channel(), 0u);
    EXPECT_TRUE(pkt.want_ack());
    EXPECT_EQ(pkt.hop_limit(), 5u);
    ASSERT_TRUE(pkt.has_decoded());
    EXPECT_EQ(pkt.decoded().portnum(), meshtastic::PortNum::TRACEROUTE_APP);
    EXPECT_TRUE(pkt.decoded().want_response());
}

TEST(MeshCodec, DecodeTraceroutePacket) {
    meshtastic::RouteDiscovery rd;
    rd.add_route(0x22223333);
    rd.add_route(0x44445555);
    rd.add_snr_towards(40); // 10.0 dB (scaled by 4)
    rd.add_snr_towards(32); // 8.0 dB
    rd.add_route_back(0x66667777);
    rd.add_snr_back(48);    // 12.0 dB

    meshtastic::FromRadio fr;
    auto* pkt = fr.mutable_packet();
    pkt->set_from(0x88889999);
    pkt->set_to(0x11112222);
    auto* d = pkt->mutable_decoded();
    d->set_portnum(meshtastic::PortNum::TRACEROUTE_APP);
    d->set_payload(rd.SerializeAsString());

    uint32_t config_id = 0;
    auto ev = MeshCodec::decode_from_radio(fr.SerializeAsString(), "dev1", config_id);
    ASSERT_TRUE(ev.has_value());
    auto* tr = std::get_if<EvTracerouteReceived>(&*ev);
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->device, "dev1");
    EXPECT_EQ(tr->from_node, 0x88889999u);
    EXPECT_EQ(tr->to_node, 0x11112222u);
    ASSERT_EQ(tr->route.size(), 2u);
    EXPECT_EQ(tr->route[0], 0x22223333u);
    EXPECT_EQ(tr->route[1], 0x44445555u);
    ASSERT_EQ(tr->snr_towards.size(), 2u);
    EXPECT_FLOAT_EQ(tr->snr_towards[0], 10.0f);
    EXPECT_FLOAT_EQ(tr->snr_towards[1], 8.0f);
    ASSERT_EQ(tr->route_back.size(), 1u);
    EXPECT_EQ(tr->route_back[0], 0x66667777u);
    ASSERT_EQ(tr->snr_back.size(), 1u);
    EXPECT_FLOAT_EQ(tr->snr_back[0], 12.0f);
}

TEST(MeshCodec, DecodeTelemetryEnvironment) {
    meshtastic::Telemetry t;
    t.set_time(1710000000);
    auto* env = t.mutable_environment_metrics();
    env->set_temperature(22.4f);
    env->set_relative_humidity(48.5f);
    env->set_barometric_pressure(1012.3f);
    env->set_gas_resistance(45.2f);
    env->set_iaq(35);
    env->set_current(150.0f);

    meshtastic::FromRadio fr;
    auto* pkt = fr.mutable_packet();
    pkt->set_from(0x12345678);
    auto* d = pkt->mutable_decoded();
    d->set_portnum(meshtastic::PortNum::TELEMETRY_APP);
    d->set_payload(t.SerializeAsString());

    uint32_t config_id = 0;
    auto ev = MeshCodec::decode_from_radio(fr.SerializeAsString(), "dev1", config_id);
    ASSERT_TRUE(ev.has_value());
    auto* upd = std::get_if<EvNodeUpdated>(&*ev);
    ASSERT_NE(upd, nullptr);
    EXPECT_EQ(upd->node.node_num, 0x12345678u);
    ASSERT_TRUE(upd->node.temperature.has_value());
    EXPECT_FLOAT_EQ(*upd->node.temperature, 22.4f);
    ASSERT_TRUE(upd->node.relative_humidity.has_value());
    EXPECT_FLOAT_EQ(*upd->node.relative_humidity, 48.5f);
    ASSERT_TRUE(upd->node.barometric_pressure.has_value());
    EXPECT_FLOAT_EQ(*upd->node.barometric_pressure, 1012.3f);
    ASSERT_TRUE(upd->node.gas_resistance.has_value());
    EXPECT_FLOAT_EQ(*upd->node.gas_resistance, 45.2f);
    ASSERT_TRUE(upd->node.iaq.has_value());
    EXPECT_EQ(*upd->node.iaq, 35u);
    ASSERT_TRUE(upd->node.current.has_value());
    EXPECT_FLOAT_EQ(*upd->node.current, 150.0f);
}

TEST(MeshCodec, DecodeTelemetryAirQualityAndPower) {
    meshtastic::Telemetry t;
    auto* aq = t.mutable_air_quality_metrics();
    aq->set_pm25_standard(18);
    aq->set_co2(550);

    meshtastic::FromRadio fr;
    auto* pkt = fr.mutable_packet();
    pkt->set_from(0x87654321);
    auto* d = pkt->mutable_decoded();
    d->set_portnum(meshtastic::PortNum::TELEMETRY_APP);
    d->set_payload(t.SerializeAsString());

    uint32_t config_id = 0;
    auto ev = MeshCodec::decode_from_radio(fr.SerializeAsString(), "dev1", config_id);
    ASSERT_TRUE(ev.has_value());
    auto* upd = std::get_if<EvNodeUpdated>(&*ev);
    ASSERT_NE(upd, nullptr);
    ASSERT_TRUE(upd->node.pm25.has_value());
    EXPECT_EQ(*upd->node.pm25, 18u);
    ASSERT_TRUE(upd->node.co2.has_value());
    EXPECT_EQ(*upd->node.co2, 550u);
}

TEST(MeshCodec, DecodeTelemetryDeviceUptime) {
    meshtastic::Telemetry t;
    auto* dev = t.mutable_device_metrics();
    dev->set_battery_level(95);
    dev->set_voltage(4.12f);
    dev->set_uptime_seconds(86400);

    meshtastic::FromRadio fr;
    auto* pkt = fr.mutable_packet();
    pkt->set_from(0xaabbccdd);
    auto* d = pkt->mutable_decoded();
    d->set_portnum(meshtastic::PortNum::TELEMETRY_APP);
    d->set_payload(t.SerializeAsString());

    uint32_t config_id = 0;
    auto ev = MeshCodec::decode_from_radio(fr.SerializeAsString(), "dev1", config_id);
    ASSERT_TRUE(ev.has_value());
    auto* upd = std::get_if<EvNodeUpdated>(&*ev);
    ASSERT_NE(upd, nullptr);
    ASSERT_TRUE(upd->node.battery_level.has_value());
    EXPECT_EQ(*upd->node.battery_level, 95u);
    ASSERT_TRUE(upd->node.voltage.has_value());
    EXPECT_FLOAT_EQ(*upd->node.voltage, 4.12f);
    ASSERT_TRUE(upd->node.uptime_seconds.has_value());
    EXPECT_EQ(*upd->node.uptime_seconds, 86400u);
}

TEST(MeshCodec, DecodeNodeInfoPacket) {
    meshtastic::User u;
    u.set_id("!12345678");
    u.set_long_name("Test Long Name");
    u.set_short_name("TLN");
    u.set_hw_model(meshtastic::HardwareModel::HELTEC_V3);
    u.set_role(meshtastic::Config_DeviceConfig_Role_ROUTER);

    meshtastic::FromRadio fr;
    auto* pkt = fr.mutable_packet();
    pkt->set_from(0x12345678);
    pkt->set_rx_time(1710000000);
    pkt->set_rx_snr(9.5f);
    pkt->set_hop_start(3);
    pkt->set_hop_limit(2);
    auto* d = pkt->mutable_decoded();
    d->set_portnum(meshtastic::PortNum::NODEINFO_APP);
    d->set_payload(u.SerializeAsString());

    uint32_t config_id = 0;
    auto ev = MeshCodec::decode_from_radio(fr.SerializeAsString(), "dev1", config_id);
    ASSERT_TRUE(ev.has_value());
    auto* upd = std::get_if<EvNodeUpdated>(&*ev);
    ASSERT_NE(upd, nullptr);
    EXPECT_EQ(upd->node.node_num, 0x12345678u);
    EXPECT_EQ(upd->node.node_id, "!12345678");
    EXPECT_EQ(upd->node.long_name, "Test Long Name");
    EXPECT_EQ(upd->node.short_name, "TLN");
    EXPECT_EQ(upd->node.hw_model, "HELTEC_V3");
    EXPECT_EQ(upd->node.role, "ROUTER");
    ASSERT_TRUE(upd->node.hops_away.has_value());
    EXPECT_EQ(*upd->node.hops_away, 1u);
}


