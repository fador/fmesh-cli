#include "bluez_client.h"

#include "bluez_agent.h"
#include "mesh/mesh_codec.h"
#include "util/log.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <map>
#include <random>
#include <thread>

namespace meshcli {

using namespace std::chrono_literals;

// Meshtastic BLE UUIDs.
static constexpr const char* kServiceUuid  = "6ba1b218-15a8-461f-9fa8-5dcae273eafd";
static constexpr const char* kToradioUuid  = "f75c76d2-129e-4dad-a1dd-7866124401e7";
static constexpr const char* kFromradioUuid= "2c55e69e-4993-11ed-b878-0242ac120002";
static constexpr const char* kFromnumUuid  = "ed9da18c-a800-4f66-a670-aa7547e34453";
static constexpr const char* kLogradioUuid = "5a3d6e49-06e6-4423-9944-e9de8cdf9547";
static constexpr const char* kLegacyLogradioUuid = "6c6fd238-78fa-436b-aacf-15c5be1ef2e2";

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

static std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return s;
}

BluezClient::BluezClient(BleDeviceSpec spec, EventSink sink)
    : spec_(std::move(spec)), sink_(std::move(sink)) {}

BluezClient::~BluezClient() { LOG_DEBUG() << "~BluezClient"; stop(); }

std::string BluezClient::start(bool pair) {
    try {
        conn_ = sdbus::createSystemBusConnection();
        running_ = true;
    } catch (const sdbus::Error& e) {
        emit_error(std::string("D-Bus connect failed: ") + e.what());
        return {};
    }

    if (pair) {
        agent_ = std::make_unique<BluezAgent>(spec_.pin, [](std::string s) {
            LOG_INFO() << "[agent] " << s;
        });
        try {
            agent_->register_on(*conn_, "/meshcli/agent");
            loop_thread_ = std::thread([this] {
                try { conn_->enterEventLoop(); }
                catch (const sdbus::Error& e) { LOG_ERROR() << "event loop: " << e.what(); }
            });
        } catch (...) {}
    }

    // Connect, discover GATT, subscribe to notifications — all synchronous.
    run_connect_flow();
    if (device_id_.empty()) return {};

    // Start the event loop BEFORE sending want_config, so FROMNUM
    // notifications fire and trigger drain_from_radio() immediately.
    if (!loop_thread_.joinable()) {
        loop_thread_ = std::thread([this] {
            try { conn_->enterEventLoop(); }
            catch (const sdbus::Error& e) { LOG_ERROR() << "event loop: " << e.what(); }
        });
        // Give the event loop a moment to start.
        std::this_thread::sleep_for(100ms);
    }

    // Kick off the protocol handshake and drain the initial data stream.
    initial_handshake();
    return device_id_;
}

void BluezClient::run_connect_flow() {
    if (!find_adapter()) {
        emit_error("no BlueZ adapter found");
        return;
    }

    // Resolve device path: explicit address first, else scan by name.
    if (!spec_.address.empty()) {
        std::string mac = to_upper(spec_.address);
        std::string p;
        for (char c : mac) if (c != ':' && c != '-' && c != '_') p += c;
        std::string dev = "/org/bluez/";
        dev += adapter_path_.substr(adapter_path_.rfind('/') + 1);
        dev += "/dev_";
        for (size_t i = 0; i < p.size(); i += 2) {
            if (i) dev += '_';
            dev += p.substr(i, 2);
        }
        device_path_ = dev;
        LOG_INFO() << "using explicit device path " << device_path_;
    } else {
        auto found = scan_for_device(10);
        if (!found) {
            emit_error("device '" + spec_.name + "' not found in BLE scan");
            return;
        }
        device_path_ = *found;
    }
    device_id_ = device_path_;

    if (!ensure_paired_and_connected(/*do_pair=*/agent_ != nullptr)) {
        return;
    }

    EvConnected evc;
    evc.device = device_id_;
    evc.display_name = spec_.name;
    emit(evc);

    if (!discover_gatt()) {
        return;
    }
    subscribe_notifications();

    connected_ = true;
    LOG_INFO() << "BLE connected to " << spec_.name << " (" << device_id_ << ")";
}

// Send want_config_id and drain FROMRADIO until the config handshake
// completes. Runs with the event loop already active so FROMNUM
// notifications trigger drain_from_radio() in parallel.
void BluezClient::initial_handshake() {
    // Test: read FROMRADIO to verify GATT works before writing.
    try {
        auto test = read_char(fromradio_path_);
        LOG_DEBUG() << "initial FROMRADIO read: " << test.size() << " bytes";
    } catch (const sdbus::Error& e) {
        LOG_WARN() << "initial FROMRADIO read failed: " << e.what();
    }

    uint32_t config_id = static_cast<uint32_t>(std::random_device{}());
    LOG_INFO() << "sending want_config_id=" << config_id;
    try {
        auto payload = MeshCodec::encode_want_config(config_id);
        write_char(toradio_path_, payload);
    } catch (const sdbus::Error& e) {
        LOG_ERROR() << "TORADIO write failed: " << e.what();
    }

    // Poll FROMRADIO until config_complete_id arrives, or timeout.
    // The event loop is already running, so FROMNUM notifications also
    // trigger drain_from_radio() which processes data on the BLE thread.
    bool got_config = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (running_ && !got_config && std::chrono::steady_clock::now() < deadline) {
        try {
            auto bytes = read_char(fromradio_path_);
            if (!bytes.empty()) {
                emit_raw(bytes);
                uint32_t cid = 0;
                auto ev = MeshCodec::decode_from_radio(bytes, device_id_, cid);
                if (ev) emit(*ev);
                if (cid != 0) {
                    LOG_DEBUG() << "config_complete_id=" << cid;
                    got_config = true;
                }
            }
        } catch (const sdbus::Error& e) {
            LOG_DEBUG() << "FROMRADIO read during handshake: " << e.what();
        }
        std::this_thread::sleep_for(200ms);
    }

    // Give the event loop one more cycle to drain any remaining data.
    if (got_config) {
        LOG_DEBUG() << "config handshake complete, draining residual...";
        for (int i = 0; i < 5 && running_; ++i) {
            try {
                auto bytes = read_char(fromradio_path_);
                if (!bytes.empty()) {
                    emit_raw(bytes);
                    auto ev = MeshCodec::decode_from_radio(bytes, device_id_, config_id);
                    if (ev) emit(*ev);
                }
            } catch (const sdbus::Error&) {}
            std::this_thread::sleep_for(100ms);
        }
    }
}

void BluezClient::stop() {
    LOG_DEBUG() << "stop() called; running_ was " << running_.load();
    if (!running_.exchange(false)) return;
    if (conn_) {
        try { conn_->leaveEventLoop(); } catch (...) {}
    }
    if (loop_thread_.joinable()) loop_thread_.join();
    agent_.reset();
    {
        std::lock_guard<std::mutex> lock(proxy_mu_);
        toradio_proxy_.reset();
        fromradio_proxy_.reset();
        fromnum_proxy_.reset();
        logradio_proxy_.reset();
        conn_.reset();
    }
    connected_ = false;
}

bool BluezClient::send_to_radio(const std::string& bytes) {
    std::lock_guard<std::mutex> lock(proxy_mu_);
    if (!connected_ || toradio_path_.empty() || !toradio_proxy_ || !conn_) return false;
    try {
        write_char_locked(toradio_path_, bytes);
        return true;
    } catch (const sdbus::Error& e) {
        LOG_ERROR() << "TORADIO write failed: " << e.what();
        emit_error(std::string("send failed: ") + e.what());
        return false;
    }
}

void BluezClient::emit(MeshEvent ev) {
    if (sink_) sink_(std::move(ev));
}
void BluezClient::emit_error(std::string msg) {
    LOG_ERROR() << msg;
    EvError e;
    e.device = device_id_.empty() ? spec_.name : device_id_;
    e.message = std::move(msg);
    emit(e);
}

void BluezClient::emit_raw(const std::string& fromradio_bytes) {
    EvRawPacket ev;
    ev.device = device_id_;
    ev.ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
    ev.summary = MeshCodec::from_radio_summary(fromradio_bytes);
    ev.hex = MeshCodec::hex_dump(fromradio_bytes);
    if (sink_) sink_(std::move(ev));
}

// ---------------------------------------------------------------------------
// background D-Bus loop (notifications only; connect flow is synchronous)
// ---------------------------------------------------------------------------

// (run_connect_flow above replaced the old run_loop. The event loop itself
// runs in start()'s lambda on loop_thread_.)

// ---------------------------------------------------------------------------
// BlueZ helpers
// ---------------------------------------------------------------------------

bool BluezClient::find_adapter() {
    try {
        auto obj_mgr = sdbus::createProxy(*conn_, "org.bluez", "/");
        std::map<sdbus::ObjectPath, std::map<std::string, std::map<std::string, sdbus::Variant>>> objects;
        obj_mgr->callMethod("GetManagedObjects")
            .onInterface("org.freedesktop.DBus.ObjectManager")
            .storeResultsTo(objects);
        for (const auto& [path, ifaces] : objects) {
            auto it = ifaces.find("org.bluez.Adapter1");
            if (it != ifaces.end()) {
                adapter_path_ = path;
                LOG_INFO() << "found adapter " << adapter_path_;
                return true;
            }
        }
    } catch (const sdbus::Error& e) {
        emit_error(std::string("GetManagedObjects failed: ") + e.what());
    }
    return false;
}

std::optional<std::string> BluezClient::scan_for_device(int timeout_s) {
    auto adapter = sdbus::createProxy(*conn_, "org.bluez", adapter_path_);
    try {
        adapter->callMethod("StartDiscovery").onInterface("org.bluez.Adapter1");
    } catch (const sdbus::Error& e) {
        LOG_WARN() << "StartDiscovery failed: " << e.what();
    }
    LOG_INFO() << "scanning for " << spec_.name << " (" << timeout_s << "s)...";

    std::string needle = to_lower(spec_.name);
    std::optional<std::string> result;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    LOG_DEBUG() << "scan loop start; running_=" << running_.load() << " deadline_in=" << timeout_s << "s";
    int iter = 0;
    while (std::chrono::steady_clock::now() < deadline && running_) {
        std::this_thread::sleep_for(500ms);
        ++iter;
        LOG_DEBUG() << "scan iter " << iter << " running_=" << running_.load();
        auto obj_mgr = sdbus::createProxy(*conn_, "org.bluez", "/");
        std::map<sdbus::ObjectPath, std::map<std::string, std::map<std::string, sdbus::Variant>>> objects;
        try {
            obj_mgr->callMethod("GetManagedObjects")
                .onInterface("org.freedesktop.DBus.ObjectManager")
                .storeResultsTo(objects);
        } catch (const sdbus::Error&) { continue; }

        for (const auto& [path, ifaces] : objects) {
            auto it = ifaces.find("org.bluez.Device1");
            if (it == ifaces.end()) continue;
            const auto& props = it->second;
            auto name_it = props.find("Name");
            auto alias_it = props.find("Alias");
            auto addr_it = props.find("Address");
            std::string name, alias, addr;
            if (name_it != props.end()) name = name_it->second.get<std::string>();
            if (alias_it != props.end()) alias = alias_it->second.get<std::string>();
            if (addr_it != props.end()) addr = addr_it->second.get<std::string>();

            // Filter to this adapter.
            if (path.find(adapter_path_ + "/") != 0) continue;

            if (!needle.empty() &&
                (to_lower(name) == needle || to_lower(alias) == needle)) {
                result = path;
                spec_.address = addr;  // remember for next time
                LOG_INFO() << "found " << name << " at " << addr << " (" << path << ")";
                break;
            }
        }
        if (result) break;
    }
    try { adapter->callMethod("StopDiscovery").onInterface("org.bluez.Adapter1"); }
    catch (const sdbus::Error& e) { LOG_WARN() << "StopDiscovery failed: " << e.what(); }

    // Give BlueZ a moment to settle after stopping discovery before we try
    // to connect. Without this delay, Connect can fail with
    // "le-connection-abort-by-local".
    std::this_thread::sleep_for(1s);
    return result;
}

bool BluezClient::ensure_paired_and_connected(bool do_pair) {
    auto device = sdbus::createProxy(*conn_, "org.bluez", device_path_);

    // Read current state — the device may not be immediately visible on
    // D-Bus right after scanning, so tolerate a transient ENXIO here.
    bool paired = false, connected = false, services_resolved = false;
    try {
        device->callMethod("Get").onInterface("org.freedesktop.DBus.Properties")
            .withArguments(std::string("org.bluez.Device1"), std::string("Paired"))
            .storeResultsTo(paired);
        device->callMethod("Get").onInterface("org.freedesktop.DBus.Properties")
            .withArguments(std::string("org.bluez.Device1"), std::string("Connected"))
            .storeResultsTo(connected);
        device->callMethod("Get").onInterface("org.freedesktop.DBus.Properties")
            .withArguments(std::string("org.bluez.Device1"), std::string("ServicesResolved"))
            .storeResultsTo(services_resolved);
    } catch (const sdbus::Error& e) {
        LOG_DEBUG() << "device state not yet available (expected transient): " << e.what();
    }
    LOG_DEBUG() << "device state: paired=" << paired << " connected=" << connected
                << " services_resolved=" << services_resolved;

    // If already connected with services resolved, we're good — many
    // Meshtastic devices allow unpaired BLE GATT access.
    if (connected && services_resolved) {
        LOG_INFO() << "device already connected with services resolved";
        return true;
    }

    // If not paired and pairing was requested, try to pair.
    if (!paired && do_pair && !connected) {
        LOG_INFO() << "pairing " << device_path_ << " (PIN " << spec_.pin << ")";
        try {
            device->callMethod("Pair").onInterface("org.bluez.Device1");
            device->callMethod("Set").onInterface("org.freedesktop.DBus.Properties")
                .withArguments(std::string("org.bluez.Device1"),
                               std::string("Trusted"), sdbus::Variant{true});
            LOG_INFO() << "pairing succeeded; device trusted";
            paired = true;
        } catch (const sdbus::Error& e) {
            LOG_WARN() << "Pair failed: " << e.what() << " — trying Connect anyway";
        }
    }

    // Connect (GATT profile). This works even without pairing on many devices.
    // Retry a few times — BLE connections can be flaky right after discovery.
    if (!connected) {
        for (int attempt = 1; attempt <= 3; ++attempt) {
            try {
                LOG_DEBUG() << "Connect attempt " << attempt;
                device->callMethod("Connect").onInterface("org.bluez.Device1");
                connected = true;
                break;
            } catch (const sdbus::Error& e) {
                LOG_WARN() << "Connect attempt " << attempt << " failed: " << e.what();
                if (attempt < 3) std::this_thread::sleep_for(2s);
                else if (!paired && do_pair) {
                    // Last resort: try Pair.
                    LOG_WARN() << "trying Pair as last resort";
                    try {
                        device->callMethod("Pair").onInterface("org.bluez.Device1");
                        device->callMethod("Set").onInterface("org.freedesktop.DBus.Properties")
                            .withArguments(std::string("org.bluez.Device1"),
                                           std::string("Trusted"), sdbus::Variant{true});
                        LOG_INFO() << "pairing succeeded";
                        connected = true;
                        break;
                    } catch (const sdbus::Error& e2) {
                        emit_error(std::string("Pair failed: ") + e2.what());
                        return false;
                    }
                } else {
                    emit_error(std::string("Connect failed: ") + e.what());
                    return false;
                }
            }
        }
    }

    // Wait for services to be resolved (can take a moment after Connect).
    if (!services_resolved) {
        LOG_DEBUG() << "waiting for services to resolve...";
        for (int i = 0; i < 20; ++i) {
            bool resolved = false;
            try {
                device->callMethod("Get").onInterface("org.freedesktop.DBus.Properties")
                    .withArguments(std::string("org.bluez.Device1"), std::string("ServicesResolved"))
                    .storeResultsTo(resolved);
            } catch (const std::exception& e) {
                if (i == 0)
                    LOG_DEBUG() << "waiting for ServicesResolved (transient): " << e.what();
            } catch (...) {
                if (i == 0)
                    LOG_DEBUG() << "waiting for ServicesResolved (unknown transient)";
            }
            if (resolved) { LOG_DEBUG() << "services resolved"; break; }
            std::this_thread::sleep_for(200ms);
        }
    }

    // Mark the device as trusted — many BLE devices require this for
    // GATT operations to work on Linux/BlueZ.
    try {
        device->callMethod("Set").onInterface("org.freedesktop.DBus.Properties")
            .withArguments(std::string("org.bluez.Device1"),
                           std::string("Trusted"), sdbus::Variant{true});
        LOG_DEBUG() << "device marked as trusted";
    } catch (const sdbus::Error& e) {
        LOG_WARN() << "could not set Trusted: " << e.what();
    }

    return true;
}

bool BluezClient::discover_gatt() {
    // Enumerate managed objects under the device path and find our chars.
    auto obj_mgr = sdbus::createProxy(*conn_, "org.bluez", "/");
    std::map<sdbus::ObjectPath, std::map<std::string, std::map<std::string, sdbus::Variant>>> objects;
    try {
        obj_mgr->callMethod("GetManagedObjects")
            .onInterface("org.freedesktop.DBus.ObjectManager")
            .storeResultsTo(objects);
    } catch (const sdbus::Error& e) {
        emit_error(std::string("GATT enum failed: ") + e.what());
        return false;
    }

    for (const auto& [path, ifaces] : objects) {
        if (path.find(device_path_ + "/") != 0) continue;
        auto it = ifaces.find("org.bluez.GattCharacteristic1");
        if (it == ifaces.end()) continue;
        auto uuid_it = it->second.find("UUID");
        if (uuid_it == it->second.end()) continue;
        std::string uuid = to_lower(uuid_it->second.get<std::string>());

        if (uuid == kToradioUuid)        toradio_path_ = path;
        else if (uuid == kFromradioUuid) fromradio_path_ = path;
        else if (uuid == kFromnumUuid)   fromnum_path_ = path;
        else if (uuid == kLogradioUuid || uuid == kLegacyLogradioUuid) logradio_path_ = path;
    }

    if (toradio_path_.empty() || fromradio_path_.empty() || fromnum_path_.empty()) {
        emit_error("missing required Meshtastic GATT characteristics (not a Meshtastic device?)");
        return false;
    }
    LOG_DEBUG() << "GATT: toradio=" << toradio_path_
                << " fromradio=" << fromradio_path_
                << " fromnum=" << fromnum_path_
                << " logradio=" << (logradio_path_.empty() ? "(none)" : logradio_path_);

    // Cache proxies for the characteristics we'll use repeatedly.
    toradio_proxy_   = sdbus::createProxy(*conn_, "org.bluez", toradio_path_);
    fromradio_proxy_ = sdbus::createProxy(*conn_, "org.bluez", fromradio_path_);
    return true;
}

void BluezClient::subscribe_notifications() {
    // FROMNUM -> drain FROMRADIO.
    if (!fromnum_path_.empty()) {
        fromnum_proxy_ = sdbus::createProxy(*conn_, "org.bluez", fromnum_path_);
        try {
            fromnum_proxy_->callMethod("StartNotify").onInterface("org.bluez.GattCharacteristic1");
            LOG_DEBUG() << "FROMNUM StartNotify succeeded";
        } catch (const sdbus::Error& e) {
            emit_error(std::string("FROMNUM StartNotify failed: ") + e.what());
            return;
        }
        fromnum_proxy_->uponSignal("PropertiesChanged")
            .onInterface("org.freedesktop.DBus.Properties")
            .call([this](const std::string& iface, const std::map<std::string, sdbus::Variant>&,
                         const std::vector<std::string>&) {
                if (iface != "org.bluez.GattCharacteristic1") return;
                drain_from_radio();
            });
    }

    // LOGRADIO (optional) -> emit log line events.
    if (!logradio_path_.empty()) {
        logradio_proxy_ = sdbus::createProxy(*conn_, "org.bluez", logradio_path_);
        try {
            logradio_proxy_->callMethod("StartNotify").onInterface("org.bluez.GattCharacteristic1");
        } catch (const sdbus::Error&) {}
        logradio_proxy_->uponSignal("PropertiesChanged")
            .onInterface("org.freedesktop.DBus.Properties")
            .call([this](const std::string& iface, const std::map<std::string, sdbus::Variant>& changed,
                         const std::vector<std::string>&) {
                if (iface != "org.bluez.GattCharacteristic1") return;
                auto it = changed.find("Value");
                if (it == changed.end()) return;
                auto bytes = it->second.get<std::vector<uint8_t>>();
                std::string msg(bytes.begin(), bytes.end());
                EvLogLine ev;
                ev.device = device_id_;
                ev.source = "serial";
                ev.message = msg;
                emit(ev);
            });
    }
}

void BluezClient::drain_from_radio() {
    if (fromradio_path_.empty()) return;
    int msgs = 0;
    while (running_) {
        std::string bytes;
        {
            std::lock_guard<std::mutex> lock(proxy_mu_);
            if (!conn_ || fromradio_path_.empty()) return;
            try {
                bytes = read_char_locked(fromradio_path_);
            } catch (const sdbus::Error& e) {
                LOG_WARN() << "FROMRADIO read error: " << e.what();
                return;
            }
        }
        if (bytes.empty()) {
            if (msgs > 0) LOG_DEBUG() << "drained " << msgs << " FromRadio messages";
            return;
        }
        ++msgs;
        emit_raw(bytes);
        uint32_t config_id = 0;
        auto ev = MeshCodec::decode_from_radio(bytes, device_id_, config_id);
        if (ev) {
            LOG_DEBUG() << "FromRadio: emitting event (variant index=" << ev->index() << ")";
            emit(*ev);
        } else {
            LOG_TRACE() << "FromRadio: message ignored (no event)";
        }
    }
}

std::string BluezClient::read_char(const std::string& path) {
    std::lock_guard<std::mutex> lock(proxy_mu_);
    return read_char_locked(path);
}

std::string BluezClient::read_char_locked(const std::string& path) {
    // Caller must hold proxy_mu_.
    sdbus::IProxy* ch = nullptr;
    std::unique_ptr<sdbus::IProxy> tmp;
    if (path == fromradio_path_ && fromradio_proxy_) ch = fromradio_proxy_.get();
    else if (!conn_) return {};
    else { tmp = sdbus::createProxy(*conn_, "org.bluez", path); ch = tmp.get(); }
    std::vector<uint8_t> result;
    ch->callMethod("ReadValue")
        .onInterface("org.bluez.GattCharacteristic1")
        .withArguments(std::map<std::string, sdbus::Variant>{})
        .storeResultsTo(result);
    return std::string(result.begin(), result.end());
}

void BluezClient::write_char(const std::string& path, const std::string& bytes) {
    std::lock_guard<std::mutex> lock(proxy_mu_);
    write_char_locked(path, bytes);
}

void BluezClient::write_char_locked(const std::string& path, const std::string& bytes) {
    // Caller must hold proxy_mu_.
    sdbus::IProxy* ch = nullptr;
    std::unique_ptr<sdbus::IProxy> tmp;
    if (path == toradio_path_ && toradio_proxy_) ch = toradio_proxy_.get();
    else if (!conn_) return;
    else { tmp = sdbus::createProxy(*conn_, "org.bluez", path); ch = tmp.get(); }
    std::vector<uint8_t> v(bytes.begin(), bytes.end());
    std::map<std::string, sdbus::Variant> opts;
    opts["type"] = sdbus::Variant{std::string{"request"}};
    ch->callMethod("WriteValue")
        .onInterface("org.bluez.GattCharacteristic1")
        .withArguments(v, opts);
}

// Global scanner state for Linux
static std::thread g_scan_thread;
static std::atomic<bool> g_scanning{false};

void BleClient::scan_async(int timeout_s, std::function<void(const std::string&, const std::string&)> on_device, std::function<void()> on_complete) {
    stop_scan();
    
    g_scanning = true;
    g_scan_thread = std::thread([timeout_s, on_device, on_complete]() {
        try {
            auto conn = sdbus::createSystemBusConnection();
            
            // Find adapter
            std::string adapter_path;
            auto obj_mgr = sdbus::createProxy(*conn, "org.bluez", "/");
            std::map<sdbus::ObjectPath, std::map<std::string, std::map<std::string, sdbus::Variant>>> objects;
            obj_mgr->callMethod("GetManagedObjects")
                .onInterface("org.freedesktop.DBus.ObjectManager")
                .storeResultsTo(objects);
            for (const auto& [path, ifaces] : objects) {
                if (ifaces.find("org.bluez.Adapter1") != ifaces.end()) {
                    adapter_path = path; break;
                }
            }

            if (adapter_path.empty()) {
                g_scanning = false;
                if (on_complete) on_complete();
                return;
            }

            // Start discovery
            auto adapter = sdbus::createProxy(*conn, "org.bluez", adapter_path);
            adapter->callMethod("StartDiscovery").onInterface("org.bluez.Adapter1");

            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
            while (g_scanning && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (!g_scanning) break;

                std::map<sdbus::ObjectPath, std::map<std::string, std::map<std::string, sdbus::Variant>>> objs;
                obj_mgr->callMethod("GetManagedObjects")
                    .onInterface("org.freedesktop.DBus.ObjectManager")
                    .storeResultsTo(objs);

                for (const auto& [path, ifaces] : objs) {
                    auto it = ifaces.find("org.bluez.Device1");
                    if (it != ifaces.end()) {
                        auto name_it = it->second.find("Name");
                        auto addr_it = it->second.find("Address");
                        if (name_it != it->second.end() && addr_it != it->second.end()) {
                            std::string name = name_it->second.get<std::string>();
                            std::string addr = addr_it->second.get<std::string>();
                            if (on_device) on_device(name, addr);
                        }
                    }
                }
            }

            adapter->callMethod("StopDiscovery").onInterface("org.bluez.Adapter1");

        } catch (const sdbus::Error&) {}

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
