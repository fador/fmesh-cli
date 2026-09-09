#include "minitest.h"
#include "mesh/mesh_codec.h"
#include "mesh/mesh_service.h"
#include <meshtastic/config.pb.h>
#include <meshtastic/module_config.pb.h>
#include <meshtastic/mesh.pb.h>

using namespace meshcli;

TEST(ConfigModify, LoraTxPower) {
    meshtastic::Config config;
    auto lora = config.mutable_lora();
    lora->set_tx_power(20);
    
    std::string config_bytes = config.SerializeAsString();
    std::string mod_bytes;
    bool is_module = false;
    std::string out_modified;
    
    bool ok = MeshCodec::set_config_value(config_bytes, mod_bytes, "lora.tx_power", "27", is_module, out_modified);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(!is_module);
    
    meshtastic::Config new_config;
    new_config.ParseFromString(out_modified);
    ASSERT_TRUE(new_config.has_lora());
    ASSERT_EQ((int)new_config.lora().tx_power(), 27);
}

TEST(ConfigModify, DisplayScreenOn) {
    meshtastic::Config config;
    auto disp = config.mutable_display();
    disp->set_screen_on_secs(300);
    
    std::string config_bytes = config.SerializeAsString();
    std::string mod_bytes;
    bool is_module = false;
    std::string out_modified;
    
    bool ok = MeshCodec::set_config_value(config_bytes, mod_bytes, "display.screen_on_secs", "600", is_module, out_modified);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(!is_module);
    
    meshtastic::Config new_config;
    new_config.ParseFromString(out_modified);
    ASSERT_TRUE(new_config.has_display());
    ASSERT_EQ((int)new_config.display().screen_on_secs(), 600);
}

TEST(ConfigModify, ModuleConfigTelemetry) {
    meshtastic::ModuleConfig config;
    auto tel = config.mutable_telemetry();
    tel->set_device_update_interval(100);
    
    std::string config_bytes;
    std::string mod_bytes = config.SerializeAsString();
    bool is_module = false;
    std::string out_modified;
    
    bool ok = MeshCodec::set_config_value(config_bytes, mod_bytes, "telemetry.device_update_interval", "200", is_module, out_modified);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(is_module);
    
    meshtastic::ModuleConfig new_config;
    new_config.ParseFromString(out_modified);
    ASSERT_TRUE(new_config.has_telemetry());
    ASSERT_EQ((int)new_config.telemetry().device_update_interval(), 200);
}

TEST(ConfigModify, InvalidKey) {
    std::string out_modified;
    bool is_module;
    bool ok = MeshCodec::set_config_value("", "", "lora.invalid_field", "123", is_module, out_modified);
    ASSERT_TRUE(!ok);
    
    ok = MeshCodec::set_config_value("", "", "invalid_module.tx_power", "123", is_module, out_modified);
    ASSERT_TRUE(!ok);
}

TEST(ConfigModify, BooleanVariants) {
    std::string out_modified;
    bool is_module = false;
    bool ok = MeshCodec::set_config_value("", "", "lora.use_preset", "ON", is_module, out_modified);
    ASSERT_TRUE(ok);
    meshtastic::Config c1;
    c1.ParseFromString(out_modified);
    ASSERT_TRUE(c1.lora().use_preset());

    ok = MeshCodec::set_config_value(out_modified, "", "lora.use_preset", "false", is_module, out_modified);
    ASSERT_TRUE(ok);
    meshtastic::Config c2;
    c2.ParseFromString(out_modified);
    ASSERT_TRUE(!c2.lora().use_preset());

    ok = MeshCodec::set_config_value(out_modified, "", "lora.use_preset", "1", is_module, out_modified);
    ASSERT_TRUE(ok);
    meshtastic::Config c3;
    c3.ParseFromString(out_modified);
    ASSERT_TRUE(c3.lora().use_preset());
}

TEST(ConfigModify, EnumValues) {
    std::string out_modified;
    bool is_module = false;
    bool ok = MeshCodec::set_config_value("", "", "device.role", "ROUTER", is_module, out_modified);
    ASSERT_TRUE(ok);
    meshtastic::Config c1;
    c1.ParseFromString(out_modified);
    ASSERT_EQ((int)c1.device().role(), (int)meshtastic::Config_DeviceConfig_Role_ROUTER);

    // Case-insensitive
    ok = MeshCodec::set_config_value(out_modified, "", "device.role", "tracker", is_module, out_modified);
    ASSERT_TRUE(ok);
    meshtastic::Config c2;
    c2.ParseFromString(out_modified);
    ASSERT_EQ((int)c2.device().role(), (int)meshtastic::Config_DeviceConfig_Role_TRACKER);
}

TEST(ConfigModify, AdminPacketHasNonZeroPacketId) {
    meshtastic::Config cfg;
    cfg.mutable_lora()->set_tx_power(22);
    auto raw = cfg.SerializeAsString();

    // With explicit packet_id
    auto pkt_bytes = MeshCodec::encode_admin_packet(0x100, 0x200, raw, false, 0x12345678);
    meshtastic::ToRadio tr;
    tr.ParseFromString(pkt_bytes);
    ASSERT_EQ(tr.packet().id(), 0x12345678u);

    // With default packet_id = 0, should auto-generate non-zero
    auto pkt_bytes_auto = MeshCodec::encode_admin_packet(0x100, 0x200, raw, false);
    meshtastic::ToRadio tr_auto;
    tr_auto.ParseFromString(pkt_bytes_auto);
    ASSERT_TRUE(tr_auto.packet().id() != 0u);
}

TEST(ConfigModify, DeviceRuntimeAliasMatching) {
    DeviceRuntime rt;
    rt.id = "EB:DE:DD:22:98:5D";
    rt.spec.address = "EB:DE:DD:22:98:5D";
    rt.spec.name = "Meshtastic_985d";
    rt.display_name = "My Radio";
    rt.my_node_num = 0x22985d;
    rt.aliases.push_back("/org/bluez/hci0/dev_EB_DE_DD_22_98_5D");

    // Exact MAC match
    ASSERT_TRUE(rt.matches_identifier("EB:DE:DD:22:98:5D"));
    // Lowercase MAC
    ASSERT_TRUE(rt.matches_identifier("eb:de:dd:22:98:5d"));
    // BlueZ path match
    ASSERT_TRUE(rt.matches_identifier("/org/bluez/hci0/dev_EB_DE_DD_22_98_5D"));
    // Dev prefix
    ASSERT_TRUE(rt.matches_identifier("dev_EB_DE_DD_22_98_5D"));
    // Spec name
    ASSERT_TRUE(rt.matches_identifier("Meshtastic_985d"));
    // Display name
    ASSERT_TRUE(rt.matches_identifier("My Radio"));
    // Node ID
    ASSERT_TRUE(rt.matches_identifier("!0022985d"));
    // Unrelated string
    ASSERT_TRUE(!rt.matches_identifier("Different_Radio"));
}



