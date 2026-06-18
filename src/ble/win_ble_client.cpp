#include "win_ble_client.h"

#ifdef _WIN32

#include "util/log.h"
#include "mesh/mesh_codec.h"
#include <chrono>
#include <iomanip>
#include <sstream>
#include <winrt/Windows.Foundation.Collections.h>

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace winrt::Windows::Storage::Streams;

namespace meshcli {

static const guid kServiceUuid{"6ba1b218-15a8-461f-9fa8-5dcae273eafd"};
static const guid kToradioUuid{"f75c76d2-129e-4dad-a1dd-7866124401e7"};
static const guid kFromradioUuid{"2c55e69e-4993-11ed-b878-0242ac120002"};
static const guid kFromnumUuid{"ed9da18c-a800-4f66-a670-aa7547e34453"};

WinBleClient::WinBleClient(BleDeviceSpec spec, EventSink sink)
    : spec_(std::move(spec)), sink_(std::move(sink)) {
    init_apartment();
}

WinBleClient::~WinBleClient() {
    stop();
}

std::string WinBleClient::start(bool pair) {
    (void)pair; // WinRT pairing is handled by Windows automatically when writing/reading encrypted characteristics if needed.
    
    // device_id is usually a bluetooth address
    if (!spec_.address.empty()) {
        device_id_ = spec_.address;
    } else {
        device_id_ = spec_.name;
    }

    running_ = true;
    loop_thread_ = std::thread([this]() { run_connect_flow(); });
    
    return device_id_;
}

void WinBleClient::stop() {
    if (!running_) return;
    running_ = false;

    if (ble_device_) {
        if (fromradio_char_) {
            fromradio_char_.ValueChanged(fromradio_token_);
            fromradio_char_ = nullptr;
        }
        if (fromnum_char_) {
            fromnum_char_.ValueChanged(fromnum_token_);
            fromnum_char_ = nullptr;
        }
        ble_device_.ConnectionStatusChanged(connection_status_token_);
        ble_device_.Close();
        ble_device_ = nullptr;
    }

    if (loop_thread_.joinable()) {
        loop_thread_.join();
    }
}

void WinBleClient::run_connect_flow() {
    try {
        uint64_t mac = 0;
        if (!spec_.address.empty()) {
            std::string clean_mac;
            for (char c : spec_.address) if (c != ':') clean_mac += c;
            mac = std::stoull(clean_mac, nullptr, 16);
            ble_device_ = BluetoothLEDevice::FromBluetoothAddressAsync(mac).get();
        } else {
            // Need to scan for device by name first if no MAC is provided.
            emit_error("Connecting by name without prior scan is currently unsupported on Windows. Please use TUI scan.");
            return;
        }

        if (!ble_device_) {
            emit_error("Failed to find BLE device.");
            return;
        }

        connection_status_token_ = ble_device_.ConnectionStatusChanged([this](BluetoothLEDevice const& dev, auto const&) {
            if (dev.ConnectionStatus() == BluetoothConnectionStatus::Disconnected) {
                std::lock_guard<std::mutex> lock(mu_);
                if (connected_) {
                    connected_ = false;
                    EvDisconnected ev;
                    ev.device = device_id_;
                    ev.reason = "BLE Connection dropped";
                    emit(ev);
                }
            }
        });

        auto servicesResult = ble_device_.GetGattServicesForUuidAsync(kServiceUuid).get();
        if (servicesResult.Status() != GattCommunicationStatus::Success || servicesResult.Services().Size() == 0) {
            emit_error("Failed to find Meshtastic GATT service.");
            return;
        }

        auto service = servicesResult.Services().GetAt(0);

        auto charsResult = service.GetCharacteristicsAsync().get();
        if (charsResult.Status() != GattCommunicationStatus::Success) {
            emit_error("Failed to get GATT characteristics.");
            return;
        }

        for (auto&& ch : charsResult.Characteristics()) {
            if (ch.Uuid() == kToradioUuid) {
                toradio_char_ = ch;
            } else if (ch.Uuid() == kFromradioUuid) {
                fromradio_char_ = ch;
            } else if (ch.Uuid() == kFromnumUuid) {
                fromnum_char_ = ch;
            }
        }

        if (!toradio_char_ || !fromradio_char_) {
            emit_error("Missing required ToRadio/FromRadio characteristics.");
            return;
        }

        // Subscribe to FromRadio notifications
        auto status = fromradio_char_.WriteClientCharacteristicConfigurationDescriptorAsync(GattClientCharacteristicConfigurationDescriptorValue::Notify).get();
        if (status == GattCommunicationStatus::Success) {
            fromradio_token_ = fromradio_char_.ValueChanged([this](GattCharacteristic const&, GattValueChangedEventArgs const& args) {
                auto reader = DataReader::FromBuffer(args.CharacteristicValue());
                std::string data(reader.UnconsumedBufferLength(), '\0');
                reader.ReadBytes(winrt::array_view<uint8_t>(reinterpret_cast<uint8_t*>(data.data()), static_cast<uint32_t>(data.size())));
                emit_raw(data);
            });
        } else {
            emit_error("Failed to subscribe to FromRadio.");
            return;
        }

        // Subscribe to FromNum notifications
        if (fromnum_char_) {
            auto numStatus = fromnum_char_.WriteClientCharacteristicConfigurationDescriptorAsync(GattClientCharacteristicConfigurationDescriptorValue::Notify).get();
            if (numStatus == GattCommunicationStatus::Success) {
                fromnum_token_ = fromnum_char_.ValueChanged([this](GattCharacteristic const&, GattValueChangedEventArgs const& args) {
                    auto reader = DataReader::FromBuffer(args.CharacteristicValue());
                    std::string data(reader.UnconsumedBufferLength(), '\0');
                    reader.ReadBytes(winrt::array_view<uint8_t>(reinterpret_cast<uint8_t*>(data.data()), static_cast<uint32_t>(data.size())));
                    emit_raw(data);
                });
            }
        }

        connected_ = true;
        EvConnected ev;
        ev.device = device_id_;
        ev.display_name = spec_.name.empty() ? spec_.address : spec_.name;
        emit(ev);

        // Send want_config_id to trigger download of config, nodes, and messages
        uint32_t config_id = 1337;
        send_to_radio(MeshCodec::encode_want_config(config_id));

        // Block thread to keep async handlers alive
        while (running_ && connected_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

    } catch (const winrt::hresult_error& e) {
        emit_error("WinRT exception: " + winrt::to_string(e.message()));
    } catch (const std::exception& e) {
        emit_error(std::string("Exception: ") + e.what());
    }
}

bool WinBleClient::send_to_radio(const std::string& bytes) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!connected_ || !toradio_char_) return false;

    DataWriter writer;
    writer.WriteBytes(winrt::array_view<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()), static_cast<uint32_t>(bytes.size())));
    
    // Send without waiting for response to avoid blocking UI thread
    toradio_char_.WriteValueAsync(writer.DetachBuffer(), GattWriteOption::WriteWithoutResponse);
    return true;
}

void WinBleClient::emit(MeshEvent ev) {
    if (sink_) sink_(std::move(ev));
}

void WinBleClient::emit_error(const std::string& msg) {
    EvError ev;
    ev.device = device_id_;
    ev.message = msg;
    emit(ev);
}

void WinBleClient::emit_raw(const std::string& fromradio_bytes) {
    EvRawRxBytes rx_ev;
    rx_ev.device = device_id_;
    rx_ev.bytes = fromradio_bytes;
    if (sink_) sink_(rx_ev);

    EvRawPacket ev;
    ev.device = device_id_;
    ev.ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
    ev.summary = MeshCodec::from_radio_summary(fromradio_bytes);
    ev.hex = MeshCodec::hex_dump(fromradio_bytes);
    if (sink_) sink_(std::move(ev));
}

// Global scanner state
static winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher g_watcher{nullptr};
static winrt::event_token g_watcher_token;
static std::thread g_scan_thread;
static std::atomic<bool> g_scanning{false};

void BleClient::scan_async(int timeout_s, std::function<void(const std::string&, const std::string&)> on_device, std::function<void()> on_complete) {
    stop_scan();
    
    g_scanning = true;
    g_scan_thread = std::thread([timeout_s, on_device, on_complete]() {
        init_apartment();
        try {
            g_watcher = winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher();
            g_watcher.ScanningMode(winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEScanningMode::Active);

            g_watcher_token = g_watcher.Received([on_device](winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher const&, winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementReceivedEventArgs const& args) {
                std::string name = winrt::to_string(args.Advertisement().LocalName());
                if (name.empty()) return; // skip unnamed
                
                uint64_t addr = args.BluetoothAddress();
                std::stringstream ss;
                ss << std::hex << std::setfill('0') << std::setw(12) << addr;
                std::string mac = ss.str();
                
                // Format MAC nicely
                std::string formatted_mac;
                for (size_t i = 0; i < mac.size(); i += 2) {
                    if (i > 0) formatted_mac += ":";
                    formatted_mac += mac.substr(i, 2);
                }

                if (on_device) on_device(name, formatted_mac);
            });

            g_watcher.Start();

            // Wait for timeout or manual stop
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
            while (g_scanning && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            g_watcher.Stop();
            g_watcher.Received(g_watcher_token);
            g_watcher = nullptr;

        } catch (...) { }
        
        g_scanning = false;
        if (on_complete) on_complete();
    });
}

void BleClient::stop_scan() {
    g_scanning = false;
    if (g_scan_thread.joinable()) {
        g_scan_thread.join();
    }
}

} // namespace meshcli

#endif
