#include "app/config.h"
#include "minitest.h"

#include <cstdio>
#include <fstream>
#include <random>

using namespace meshcli;

TEST(ConfigTest, ParseDeviceSpecBle) {
    BleDeviceSpec spec;
    EXPECT_TRUE(parse_device_spec("ble:MyNode:123456", spec));
    EXPECT_EQ(spec.name, "MyNode");
    EXPECT_EQ(spec.pin, "123456");

    spec = BleDeviceSpec{};
    EXPECT_TRUE(parse_device_spec("ble:MyNodeOnly", spec));
    EXPECT_EQ(spec.name, "MyNodeOnly");
    EXPECT_EQ(spec.pin, "123456");

    // Empty name fails
    spec = BleDeviceSpec{};
    EXPECT_FALSE(parse_device_spec("ble:", spec));
}

TEST(ConfigTest, ParseDeviceSpecAddr) {
    BleDeviceSpec spec;
    EXPECT_TRUE(parse_device_spec("addr:AA:BB:CC:DD:EE:FF:654321", spec));
    EXPECT_EQ(spec.address, "AA:BB:CC:DD:EE:FF");
    EXPECT_EQ(spec.pin, "654321");

    spec = BleDeviceSpec{};
    EXPECT_TRUE(parse_device_spec("addr:11:22:33:44:55:66", spec));
    EXPECT_EQ(spec.address, "11:22:33:44:55:66");
    EXPECT_EQ(spec.pin, "123456");
}

TEST(ConfigTest, ParseDeviceSpecTcp) {
    BleDeviceSpec spec;
    EXPECT_TRUE(parse_device_spec("tcp:192.168.1.100:4403", spec));
    EXPECT_EQ(spec.tcp_host, "192.168.1.100:4403");

    spec = BleDeviceSpec{};
    EXPECT_FALSE(parse_device_spec("tcp:", spec));
}

TEST(ConfigTest, ParseDeviceSpecSerial) {
    BleDeviceSpec spec;
    EXPECT_TRUE(parse_device_spec("serial:/dev/ttyUSB0:921600", spec));
    EXPECT_EQ(spec.serial_port, "/dev/ttyUSB0");
    EXPECT_EQ(spec.serial_baud, 921600);

    spec = BleDeviceSpec{};
    EXPECT_TRUE(parse_device_spec("serial:COM3", spec));
    EXPECT_EQ(spec.serial_port, "COM3");
    EXPECT_EQ(spec.serial_baud, 115200);
}

TEST(ConfigTest, ParseDeviceSpecMesh) {
    BleDeviceSpec spec;
    EXPECT_TRUE(parse_device_spec("mesh:localhost:4404", spec));
    EXPECT_EQ(spec.mesh_host, "localhost:4404");
}

TEST(ConfigTest, ParseDeviceSpecInvalid) {
    BleDeviceSpec spec;
    EXPECT_FALSE(parse_device_spec("", spec));
    EXPECT_FALSE(parse_device_spec("invalid", spec));
    EXPECT_FALSE(parse_device_spec("unknown:foo", spec));
}

TEST(ConfigTest, FormatDeviceSpecRoundtrip) {
    BleDeviceSpec spec_ble;
    spec_ble.name = "MyRadio";
    spec_ble.pin = "999999";
    EXPECT_EQ(format_device_spec(spec_ble), "ble:MyRadio:999999");

    BleDeviceSpec spec_addr;
    spec_addr.address = "00:11:22:33:44:55";
    EXPECT_EQ(format_device_spec(spec_addr), "addr:00:11:22:33:44:55");

    BleDeviceSpec spec_tcp;
    spec_tcp.tcp_host = "10.0.0.1:4403";
    EXPECT_EQ(format_device_spec(spec_tcp), "tcp:10.0.0.1:4403");

    BleDeviceSpec spec_serial;
    spec_serial.serial_port = "COM4";
    spec_serial.serial_baud = 57600;
    EXPECT_EQ(format_device_spec(spec_serial), "serial:COM4:57600");

    BleDeviceSpec spec_mesh;
    spec_mesh.mesh_host = "mesh.lan:4404";
    EXPECT_EQ(format_device_spec(spec_mesh), "mesh:mesh.lan:4404");
}

TEST(ConfigTest, ParseArgsFlags) {
    AppConfig cfg;
    const char* argv[] = {
        "fmesh-cli",
        "--device", "tcp:192.168.1.50",
        "--device", "ble:TestNode:000000",
        "--debug",
        "--headless",
        "pair"
    };
    int argc = static_cast<int>(sizeof(argv) / sizeof(argv[0]));
    EXPECT_TRUE(parse_args(argc, const_cast<char**>(argv), cfg));
    EXPECT_EQ(cfg.devices.size(), 2u);
    EXPECT_EQ(cfg.devices[0].tcp_host, "192.168.1.50");
    EXPECT_EQ(cfg.devices[1].name, "TestNode");
    EXPECT_EQ(cfg.devices[1].pin, "000000");
    EXPECT_TRUE(cfg.log_debug);
    EXPECT_TRUE(cfg.headless);
    EXPECT_TRUE(cfg.pair);
}

TEST(ConfigTest, ParseArgsLegacyFallback) {
    AppConfig cfg;
    const char* argv[] = {
        "fmesh-cli",
        "--name", "LegacyRadio",
        "--pin", "111222"
    };
    int argc = static_cast<int>(sizeof(argv) / sizeof(argv[0]));
    EXPECT_TRUE(parse_args(argc, const_cast<char**>(argv), cfg));
    ASSERT_EQ(cfg.devices.size(), 1u);
    EXPECT_EQ(cfg.devices[0].name, "LegacyRadio");
    EXPECT_EQ(cfg.devices[0].pin, "111222");
}

TEST(ConfigTest, LoadAndSaveConfig) {
    char tmppath[256];
    std::snprintf(tmppath, sizeof(tmppath),
                  "fmesh-cli-test-cfg-%d.txt",
                  static_cast<int>(std::random_device{}()));
    std::string path = tmppath;
    std::remove(path.c_str());

    AppConfig cfg_save;
    cfg_save.config_path = path;
    cfg_save.server_mode = true;
    cfg_save.server_port = 5555;
    cfg_save.server_user = "meshuser";
    cfg_save.server_password = "supersecretpassword";

    BleDeviceSpec spec;
    spec.tcp_host = "1.2.3.4:4403";
    cfg_save.devices.push_back(spec);

    save_config(cfg_save);

    // Read back
    AppConfig cfg_load;
    cfg_load.config_path = path;
    load_config(cfg_load);

    EXPECT_TRUE(cfg_load.server_mode);
    EXPECT_EQ(cfg_load.server_port, 5555);
    EXPECT_EQ(cfg_load.server_user, "meshuser");
    EXPECT_EQ(cfg_load.server_password, "supersecretpassword");
    ASSERT_EQ(cfg_load.devices.size(), 1u);
    EXPECT_EQ(cfg_load.devices[0].tcp_host, "1.2.3.4:4403");

    std::remove(path.c_str());
}

TEST(ConfigTest, LoadConfigAutoGeneratesPassword) {
    char tmppath[256];
    std::snprintf(tmppath, sizeof(tmppath),
                  "fmesh-cli-test-cfg-auto-%d.txt",
                  static_cast<int>(std::random_device{}()));
    std::string path = tmppath;
    std::remove(path.c_str());

    AppConfig cfg;
    cfg.config_path = path;
    load_config(cfg);

    EXPECT_FALSE(cfg.server_password.empty());
    EXPECT_EQ(cfg.server_password.size(), 16u);

    // Ensure it was written to file
    AppConfig cfg2;
    cfg2.config_path = path;
    load_config(cfg2);
    EXPECT_EQ(cfg2.server_password, cfg.server_password);

    std::remove(path.c_str());
}
