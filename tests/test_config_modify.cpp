#include "minitest.h"
#include "mesh/mesh_codec.h"
#include <meshtastic/config.pb.h>
#include <meshtastic/module_config.pb.h>

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


