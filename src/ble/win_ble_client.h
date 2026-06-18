#pragma once

#include "ble_client.h"

#ifdef _WIN32

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

namespace meshcli {

class WinBleClient : public BleClient {
public:
    WinBleClient(BleDeviceSpec spec, EventSink sink);
    ~WinBleClient() override;

    std::string start(bool pair) override;
    void stop() override;
    bool send_to_radio(const std::string& bytes) override;

    [[nodiscard]] std::string device_id() const override { return device_id_; }
    [[nodiscard]] bool is_connected() const override { return connected_; }

private:
    void run_connect_flow();
    void emit(MeshEvent ev);
    void emit_error(const std::string& msg);
    void emit_raw(const std::string& fromradio_bytes);

    BleDeviceSpec spec_;
    EventSink sink_;
    std::string device_id_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};
    std::thread loop_thread_;

    winrt::Windows::Devices::Bluetooth::BluetoothLEDevice ble_device_{nullptr};
    winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic toradio_char_{nullptr};
    winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic fromradio_char_{nullptr};
    winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic fromnum_char_{nullptr};
    
    winrt::event_token fromradio_token_;
    winrt::event_token fromnum_token_;
    winrt::event_token connection_status_token_;

    mutable std::mutex mu_;
};

} // namespace meshcli

#endif
