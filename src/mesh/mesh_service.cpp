#include "mesh_service.h"

#ifdef _WIN32
#include "ble/win_ble_client.h"
#else
#include "ble/bluez_client.h"
#endif

#include "mesh/mesh_codec.h"
#include "mesh/db_sync.h"
#include "util/log.h"
#include "util/compression.h"

#include <chrono>
#include <sstream>

namespace meshcli {

namespace {
    std::pair<std::string, uint16_t> parse_tcp_host(const std::string& tcp_host) {
        size_t colon = tcp_host.rfind(':');
        std::string host = tcp_host.substr(0, colon);
        uint16_t port = 4403;
        if (colon != std::string::npos && colon + 1 < tcp_host.size())
            port = static_cast<uint16_t>(std::stoul(tcp_host.substr(colon + 1)));
        return {host, port};
    }
} // namespace

bool MeshService::is_duplicate(uint32_t from_node, uint32_t packet_id) {
    if (packet_id == 0) return false;
    uint64_t key = (static_cast<uint64_t>(from_node) << 32) | packet_id;
    for (auto k : seen_messages_)
        if (k == key) return true;
    seen_messages_.push_back(key);
    if (seen_messages_.size() > kDedupMax)
        seen_messages_.pop_front();
    return false;
}

MeshService::MeshService() {
    // Seed the packet id generator like the python lib does.
    std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);
    packet_id_ = dist(rng_);
    sync_manager_ = std::make_unique<DbSyncManager>(db_, *this);
}

MeshService::~MeshService() {
    stop_stream_server();
    disconnect_all();
    set_event_sink(nullptr, nullptr);
}

void MeshService::set_event_sink(ConcurrentQueue<MeshEvent>* q, EventFd* wake) {
    ui_queue_ = q;
    ui_wake_ = wake;
}

bool MeshService::open_database(const std::string& path) {
    return db_.open(path);
}

std::string MeshService::connect_device(const BleDeviceSpec& spec, bool pair) {
    auto rt = std::make_shared<DeviceRuntime>();
    rt->spec = spec;
    rt->db = std::make_unique<NodeDb>();
    rt->display_name = spec.name;

    // The BluezClient will push events into our handler which forwards to UI.
    // The runtime is passed directly to handle_event so that events emitted
    // during the synchronous start() handshake are processed before the device
    // is registered in the devices_ map (which only happens after start()
    // returns).
    std::weak_ptr<DeviceRuntime> weak_rt = rt;
    auto sink = [this, weak_rt](MeshEvent ev) {
        if (auto rt_locked = weak_rt.lock()) {
            handle_event(rt_locked, ev);
        }
        dispatch_to_ui(std::move(ev));
    };

    std::string id;

    if (!spec.tcp_host.empty()) {
        // --- TCP transport ---
        auto [host, port] = parse_tcp_host(spec.tcp_host);
        int fd = tcp_connect(host, port);
        if (fd < 0) {
            EvError ev;
            ev.device = "";
            ev.message = "TCP connect to " + spec.tcp_host + " failed";
            dispatch_to_ui(std::move(ev));
            return {};
        }
        rt->stream = std::make_unique<StreamClient>(fd, "tcp:" + spec.tcp_host, std::move(sink));
        id = rt->stream->start();
    } else if (!spec.mesh_host.empty()) {
        // --- Mesh server transport (TLS) ---
        auto [host, port] = parse_tcp_host(spec.mesh_host);
        int fd = tcp_connect(host, port);
        if (fd < 0) {
            EvError ev;
            ev.device = "";
            ev.message = "Mesh connect to " + spec.mesh_host + " failed";
            dispatch_to_ui(std::move(ev));
            return {};
        }
        rt->stream = std::make_unique<StreamClient>(fd, "mesh:" + spec.mesh_host, std::move(sink));
#ifdef ENABLE_MESH_NET
        rt->stream->enable_tls(spec.mesh_user, spec.mesh_password);
#endif
        id = rt->stream->start();
    } else if (!spec.serial_port.empty()) {
        // --- Serial transport ---
        int fd = serial_open(spec.serial_port, spec.serial_baud);
        if (fd < 0) {
            EvError ev;
            ev.device = "";
            ev.message = "Serial open " + spec.serial_port + " failed";
            dispatch_to_ui(std::move(ev));
            return {};
        }
        rt->stream = std::make_unique<StreamClient>(fd, "serial:" + spec.serial_port, std::move(sink));
        id = rt->stream->start();
    } else {
        // --- BLE transport (default) ---
#ifdef _WIN32
        rt->client = std::make_unique<WinBleClient>(spec, std::move(sink));
#else
        rt->client = std::make_unique<BluezClient>(spec, std::move(sink));
#endif
        id = rt->client->start(pair);
        if (id.empty()) return {};
    }

    std::lock_guard<std::mutex> lock(devices_mu_);
    devices_[id] = rt;
    // Preload cached nodes/channels from the DB (if this device was seen
    // before). The live handshake will refresh them.
    db_.load_nodes(id, *rt->db);
    db_.load_channels(id, *rt->db);
    return id;
}

bool MeshService::reconnect_device(const std::string& device_id) {
    std::shared_ptr<DeviceRuntime> rt;
    BleDeviceSpec spec;
    {
        std::lock_guard<std::mutex> lock(devices_mu_);
        auto it = devices_.find(device_id);
        if (it == devices_.end()) return false;
        rt = it->second;
        spec = rt->spec;
    }

    // Stop the old transport.
    if (rt->client) { rt->client->stop(); rt->client.reset(); }
    if (rt->stream) { rt->stream->stop(); rt->stream.reset(); }

    // Recreate the event sink.
    std::weak_ptr<DeviceRuntime> weak_rt = rt;
    auto sink = [this, weak_rt](MeshEvent ev) {
        if (auto r = weak_rt.lock()) {
            handle_event(r, ev);
        }
        dispatch_to_ui(std::move(ev));
    };

    std::string new_id;
    if (!spec.tcp_host.empty()) {
        auto [host, port] = parse_tcp_host(spec.tcp_host);
        int fd = tcp_connect(host, port);
        if (fd < 0) return false;
        rt->stream = std::make_unique<StreamClient>(fd, "tcp:" + spec.tcp_host, std::move(sink));
        new_id = rt->stream->start();
    } else if (!spec.mesh_host.empty()) {
        auto [host, port] = parse_tcp_host(spec.mesh_host);
        int fd = tcp_connect(host, port);
        if (fd < 0) return false;
        rt->stream = std::make_unique<StreamClient>(fd, "mesh:" + spec.mesh_host, std::move(sink));
#ifdef ENABLE_MESH_NET
        rt->stream->enable_tls(spec.mesh_user, spec.mesh_password);
#endif
        new_id = rt->stream->start();
    } else if (!spec.serial_port.empty()) {
        int fd = serial_open(spec.serial_port, spec.serial_baud);
        if (fd < 0) return false;
        rt->stream = std::make_unique<StreamClient>(fd, "serial:" + spec.serial_port, std::move(sink));
        new_id = rt->stream->start();
    } else {
#ifdef _WIN32
        rt->client = std::make_unique<WinBleClient>(spec, std::move(sink));
#else
        rt->client = std::make_unique<BluezClient>(spec, std::move(sink));
#endif
        new_id = rt->client->start(/*pair=*/false);
        if (new_id.empty()) return false;
    }

    // Update the map key if the device path changed.
    if (new_id != device_id) {
        std::lock_guard<std::mutex> lock(devices_mu_);
        devices_.erase(device_id);
        devices_[new_id] = rt;
    }

    rt->config_complete = false;
    LOG_INFO() << "reconnected " << spec.name << " (" << new_id << ")";
    return true;
}

bool MeshService::disconnect_device(const std::string& device_id) {
    std::shared_ptr<DeviceRuntime> rt;
    {
        std::lock_guard<std::mutex> lock(devices_mu_);
        auto it = devices_.find(device_id);
        if (it == devices_.end()) return false;
        rt = it->second;
        devices_.erase(it);
    }
    if (rt->client) {
        rt->client->send_to_radio(MeshCodec::encode_disconnect());
        rt->client->stop();
    }
    if (rt->stream) {
        rt->stream->send_to_radio(MeshCodec::encode_disconnect());
        rt->stream->stop();
    }
    LOG_INFO() << "disconnected " << device_id;
    return true;
}

void MeshService::disconnect_all() {
    std::map<std::string, std::shared_ptr<DeviceRuntime>> tmp;
    {
        std::lock_guard<std::mutex> lock(devices_mu_);
        tmp.swap(devices_);
    }
    for (auto& [_, rt] : tmp) {
        if (rt->client) {
            rt->client->send_to_radio(MeshCodec::encode_disconnect());
            rt->client->stop();
        }
        if (rt->stream) {
            rt->stream->send_to_radio(MeshCodec::encode_disconnect());
            rt->stream->stop();
        }
    }
}

void MeshService::start_stream_server(int port, const std::string& user, const std::string& password) {
#ifdef ENABLE_MESH_NET
    if (stream_server_) return;
    auto sink = [this](MeshEvent ev) {
        // StreamServer emits events here, which we can route locally.
        dispatch_to_ui(ev);
        // Wait, for EvSendRawToRadio we must handle it!
        if (std::holds_alternative<EvSendRawToRadio>(ev)) {
            // Forward it to all connected local radios.
            auto bytes = std::get<EvSendRawToRadio>(ev).bytes;
            std::lock_guard<std::mutex> lock(devices_mu_);
            for (auto& [_, rt] : devices_) {
                if (rt->client && rt->client->is_connected()) rt->client->send_to_radio(bytes);
                if (rt->stream && rt->stream->is_connected()) rt->stream->send_to_radio(bytes);
            }
        } else if (std::holds_alternative<EvDbSyncPayload>(ev)) {
            if (sync_manager_) {
                sync_manager_->handle_sync_payload(
                    std::get<EvDbSyncPayload>(ev).device,
                    std::get<EvDbSyncPayload>(ev).payload);
            }
        }
    };
    stream_server_ = std::make_unique<StreamServer>(port, user, password, sink);
    stream_server_->start();
#endif
}

void MeshService::trigger_sync() {
    if (sync_manager_) {
        sync_manager_->initiate_sync();
    }
}

void MeshService::stop_stream_server() {
#ifdef ENABLE_MESH_NET
    if (stream_server_) {
        stream_server_->stop();
        stream_server_.reset();
    }
#endif
}

uint32_t MeshService::next_packet_id() {
    std::lock_guard<std::mutex> lock(pid_mu_);
    // Mirror the meshtastic python algorithm: keep low 10 bits sequential,
    // randomize upper 22 bits.
    uint32_t next = (packet_id_ + 1) & 0xFFFFFFFFu;
    next &= 0x3FFu;
    uint32_t random_part = ((static_cast<uint32_t>(rng_()) & 0x3FFFFFu) << 10) & 0xFFFFFFFFu;
    packet_id_ = next | random_part;
    return packet_id_;
}

uint32_t MeshService::send_text(const std::string& device_id,
                                uint32_t to_node,
                                uint32_t channel_idx,
                                const std::string& text,
                                bool want_ack) {
    bool is_virtual = (device_id.find("virtual:") == 0);
    std::string virtual_stream = virtual_stream_for(device_id);
    std::string virtual_target = virtual_original_for(device_id);

    std::shared_ptr<DeviceRuntime> rt;
    {
        std::lock_guard<std::mutex> lock(devices_mu_);
        auto it = devices_.find(is_virtual ? virtual_stream : device_id);
        if (it == devices_.end()) return 0;
        rt = it->second;
    }
    if (!is_virtual && !rt->client && !rt->stream) return 0;
    if (!is_virtual && rt->client && !rt->client->is_connected()) return 0;
    if (rt->stream && !rt->stream->is_connected()) return 0;

    uint32_t pid = next_packet_id();
    // PKI public key lookup for DMs (if the peer has a known public key).
    std::vector<uint8_t> pubkey;
    if (to_node != kBroadcastNodeNum) {
        auto peer = rt->db->get(to_node);
        if (peer && peer->has_public_key) pubkey = peer->public_key;
    }
    auto bytes = MeshCodec::encode_text_packet(pid, to_node, channel_idx,
                                               text, want_ack, 0, pubkey);
                                               
    if (is_virtual) {
        if (sync_manager_) {
            sync_manager_->send_raw_to_device(virtual_target, bytes);
        }
    } else {
        if (rt->client && !rt->client->send_to_radio(bytes)) return 0;
        if (rt->stream && !rt->stream->send_to_radio(bytes)) return 0;
    }

    // Persist the outgoing message.
    StoredMessage m;
    m.device = device_id;
    if (to_node == kBroadcastNodeNum) {
        m.window_kind = "channel";
        m.window_target = channel_idx;
    } else {
        m.window_kind = "dm";
        m.window_target = to_node;
    }
    m.direction = "out";
    
    if (is_virtual) {
        std::lock_guard<std::mutex> lock(devices_mu_);
        auto vit = virtual_devices_.find(device_id);
        if (vit != virtual_devices_.end()) {
            m.from_node = vit->second.node_num;
        } else {
            m.from_node = rt->my_node_num;
        }
    } else {
        m.from_node = rt->my_node_num;
    }
    
    m.to_node = to_node;
    m.channel_idx = channel_idx;
    m.text = text;
    m.ts = std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
    m.packet_id = pid;
    m.ack_state = want_ack ? "pending" : "";
    int64_t rowid = db_.insert_message(m);
    
    if (want_ack) {
        // We only track acks if we're connected to the physical device OR the stream 
        // connection tracks them. Actually, ack responses come back on the stream.
        std::lock_guard<std::mutex> lock(devices_mu_);
        rt->pending_acks[pid] = rowid;
    }
    
    if (is_virtual && sync_manager_) {
        sync_manager_->push_message(m);
    }
    
    EvTextReceived out_ev;
    out_ev.device = device_id;
    out_ev.from_node = m.from_node;
    out_ev.to_node = to_node;
    out_ev.channel_idx = channel_idx;
    out_ev.packet_id = pid;
    out_ev.rx_time = static_cast<uint32_t>(m.ts);
    out_ev.broadcast = (to_node == kBroadcastNodeNum);
    out_ev.want_ack = want_ack;
    out_ev.text = text;
    dispatch_to_ui(out_ev);

    return pid;
}

uint32_t MeshService::send_traceroute(const std::string& device_id,
                                     uint32_t to_node,
                                     uint32_t channel_idx,
                                     uint32_t hop_limit) {
    DeviceRuntime* rt = nullptr;
    bool is_virtual = false;
    std::string virtual_target;
    {
        std::lock_guard<std::mutex> lock(devices_mu_);
        auto vit = virtual_devices_.find(device_id);
        if (vit != virtual_devices_.end()) {
            is_virtual = true;
            virtual_target = device_id;
            auto it = devices_.find(vit->second.stream_id);
            if (it == devices_.end()) return 0;
            rt = it->second.get();
        } else {
            auto it = devices_.find(device_id);
            if (it == devices_.end()) return 0;
            rt = it->second.get();
        }
    }

    if (!is_virtual && rt->client && !rt->client->is_connected()) return 0;
    if (rt->stream && !rt->stream->is_connected()) return 0;

    uint32_t pid = next_packet_id();
    uint32_t from_node = rt->my_node_num;
    if (is_virtual) {
        std::lock_guard<std::mutex> lock(devices_mu_);
        auto vit = virtual_devices_.find(device_id);
        if (vit != virtual_devices_.end()) {
            from_node = vit->second.node_num;
        }
    }

    auto bytes = MeshCodec::encode_traceroute_packet(pid, from_node, to_node, channel_idx, hop_limit);

    if (is_virtual) {
        if (sync_manager_) {
            sync_manager_->send_raw_to_device(virtual_target, bytes);
        }
    } else {
        if (rt->client && !rt->client->send_to_radio(bytes)) return 0;
        if (rt->stream && !rt->stream->send_to_radio(bytes)) return 0;
    }

    return pid;
}

bool MeshService::set_config(const std::string& device_id, const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(devices_mu_);
    
    // First, check virtual nodes
    auto vit = virtual_devices_.find(device_id);
    if (vit != virtual_devices_.end()) {
        auto rt = devices_.find(vit->second.stream_id);
        if (rt == devices_.end()) return false;
        
        bool is_module = false;
        std::string modified_bytes;
        if (!MeshCodec::set_config_value(rt->second->raw_config, rt->second->raw_module_config, key, value, is_module, modified_bytes)) {
            return false;
        }
        
        auto admin_pkt = MeshCodec::encode_admin_packet(rt->second->my_node_num, vit->second.node_num, modified_bytes, is_module);
        if (sync_manager_) {
            sync_manager_->send_raw_to_device(device_id, admin_pkt);
        }
        return true;
    }
    
    // Check local nodes
    auto it = devices_.find(device_id);
    if (it == devices_.end()) return false;
    
    auto rt = it->second.get();
    
    bool is_module = false;
    std::string modified_bytes;
    if (!MeshCodec::set_config_value(rt->raw_config, rt->raw_module_config, key, value, is_module, modified_bytes)) {
        return false;
    }
    
    auto admin_pkt = MeshCodec::encode_admin_packet(rt->my_node_num, rt->my_node_num, modified_bytes, is_module);
    
    if (rt->client) {
        rt->client->send_to_radio(admin_pkt);
    } else if (rt->stream) {
        rt->stream->send_to_radio(admin_pkt);
    }
    
    return true;
}

void MeshService::load_offline_history() {
    auto devs = db_.get_all_devices();
    std::lock_guard<std::mutex> lock(devices_mu_);
    for (const auto& d : devs) {
        if (d.empty()) continue;
        if (devices_.find(d) == devices_.end() && offline_dbs_.find(d) == offline_dbs_.end()) {
            auto ndb = std::make_unique<NodeDb>();
            db_.load_nodes(d, *ndb);
            db_.load_channels(d, *ndb);
            offline_dbs_[d] = std::move(ndb);
            LOG_INFO() << "loaded offline history for device " << d;
        }
    }
}

std::vector<std::string> MeshService::device_ids() const {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lock(devices_mu_);
    out.reserve(devices_.size() + offline_dbs_.size());
    for (const auto& [id, _] : devices_) out.push_back(id);
    for (const auto& [id, _] : offline_dbs_) {
        if (!id.empty() && devices_.find(id) == devices_.end()) out.push_back(id);
    }
    for (const auto& [id, _] : virtual_devices_) {
        out.push_back(id);
    }
    return out;
}

const NodeDb* MeshService::db_for(const std::string& device_id) const {
    if (device_id.find("virtual:") == 0) {
        std::string sid = virtual_stream_for(device_id);
        if (!sid.empty()) return db_for(sid);
    }
    std::lock_guard<std::mutex> lock(devices_mu_);
    if (!device_id.empty()) {
        auto it = devices_.find(device_id);
        if (it != devices_.end()) return it->second->db.get();
        auto it2 = offline_dbs_.find(device_id);
        if (it2 != offline_dbs_.end()) return it2->second.get();
    }
    // If device_id is empty or not found, check if there's only 1 active or offline device
    if (devices_.size() == 1) return devices_.begin()->second->db.get();
    if (devices_.empty() && offline_dbs_.size() == 1) return offline_dbs_.begin()->second.get();
    return nullptr;
}

std::optional<Node> MeshService::find_node(uint32_t node_num) const {
    {
        std::lock_guard<std::mutex> lock(devices_mu_);
        // 1. Check live devices
        for (const auto& [_, rt] : devices_) {
            if (rt && rt->db) {
                auto n = rt->db->get(node_num);
                if (n && (!n->long_name.empty() || !n->short_name.empty())) return n;
            }
        }
        // 2. Check offline dbs
        for (const auto& [_, db] : offline_dbs_) {
            if (db) {
                auto n = db->get(node_num);
                if (n && (!n->long_name.empty() || !n->short_name.empty())) return n;
            }
        }
    }
    // 3. Check SQLite database directly
    auto from_db = const_cast<Database&>(db_).get_node_any_device(node_num);
    if (from_db && (!from_db->long_name.empty() || !from_db->short_name.empty())) {
        return from_db;
    }
    return std::nullopt;
}

std::string MeshService::firmware_for(const std::string& device_id) const {
    std::lock_guard<std::mutex> lock(devices_mu_);
    auto it = devices_.find(device_id);
    if (it == devices_.end()) return {};
    return it->second->firmware_version;
}

std::string MeshService::hw_model_for(const std::string& device_id) const {
    std::lock_guard<std::mutex> lock(devices_mu_);
    auto it = devices_.find(device_id);
    if (it == devices_.end()) return {};
    return it->second->hw_model;
}

std::string MeshService::display_name_for(const std::string& device_id) const {
    if (device_id.find("virtual:") == 0) {
        return "Virtual " + virtual_original_for(device_id);
    }
    std::lock_guard<std::mutex> lock(devices_mu_);
    auto it = devices_.find(device_id);
    if (it == devices_.end()) return {};
    return it->second->display_name;
}

BleDeviceSpec MeshService::spec_for(const std::string& device_id) const {
    std::lock_guard<std::mutex> lock(devices_mu_);
    auto it = devices_.find(device_id);
    if (it == devices_.end()) return {};
    return it->second->spec;
}

std::string MeshService::virtual_stream_for(const std::string& device_id) const {
    std::lock_guard<std::mutex> lock(devices_mu_);
    auto it = virtual_devices_.find(device_id);
    if (it != virtual_devices_.end()) return it->second.stream_id;
    return {};
}

std::string MeshService::virtual_original_for(const std::string& device_id) const {
    std::lock_guard<std::mutex> lock(devices_mu_);
    auto it = virtual_devices_.find(device_id);
    if (it != virtual_devices_.end()) return it->second.original_id;
    return {};
}

void MeshService::update_virtual_devices(const std::string& stream_id, const std::vector<VirtualDevice>& vdevs) {
    std::lock_guard<std::mutex> lock(devices_mu_);
    
    // First remove any existing virtual devices that came from this stream
    for (auto it = virtual_devices_.begin(); it != virtual_devices_.end(); ) {
        if (it->second.stream_id == stream_id) {
            it = virtual_devices_.erase(it);
        } else {
            ++it;
        }
    }
    
    // Now add the new ones
    for (const auto& vd : vdevs) {
        virtual_devices_[vd.id] = vd;
    }
}

void MeshService::send_raw_to_physical(const std::string& target_original_id, const std::string& bytes) {
    std::shared_ptr<DeviceRuntime> rt;
    {
        std::lock_guard<std::mutex> lock(devices_mu_);
        auto it = devices_.find(target_original_id);
        if (it != devices_.end()) rt = it->second;
    }
    if (rt) {
        if (rt->client && rt->client->is_connected()) rt->client->send_to_radio(bytes);
        // If it's a physical device connected via a local stream? Well, local stream acts like a client.
        if (rt->stream && rt->stream->is_connected()) rt->stream->send_to_radio(bytes);
    }
}

std::vector<std::string> MeshService::config_lines_for(const std::string& device_id) const {
    std::lock_guard<std::mutex> lock(devices_mu_);
    auto it = devices_.find(device_id);
    if (it == devices_.end()) return {};
    return it->second->config_lines;
}

std::vector<EvRawPacket> MeshService::raw_packets_for(const std::string& device_id) const {
    std::lock_guard<std::mutex> lock(devices_mu_);
    auto it = devices_.find(device_id);
    if (it == devices_.end()) return {};
    return it->second->raw_packets;
}

bool MeshService::is_my_node(uint32_t node_num) const {
    if (node_num == 0) return false;
    std::lock_guard<std::mutex> lock(devices_mu_);
    for (const auto& [_, rt] : devices_) {
        if (rt && (rt->my_node_num == node_num || (rt->db && rt->db->my_node_num() == node_num))) {
            return true;
        }
    }
    for (const auto& [_, db] : offline_dbs_) {
        if (db && db->my_node_num() == node_num) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// event handling (runs on the BLE thread of whichever device emitted it)
// ---------------------------------------------------------------------------

void MeshService::handle_event(const std::shared_ptr<DeviceRuntime>& rt, MeshEvent& ev) {
    // Capture old node names before upsert for retroactive nick updates.
    if (auto* n = std::get_if<EvNodeUpdated>(&ev)) {
        auto old = rt->db->get(n->node.node_num);
        if (old) {
            n->old_short_name = old->short_name;
            n->old_long_name = old->long_name;
            // Preserve existing names if this update packet did not carry user/name fields
            if (n->node.short_name.empty()) n->node.short_name = old->short_name;
            if (n->node.long_name.empty()) n->node.long_name = old->long_name;
        } else {
            n->is_new = true;
        }
    }
    std::visit([this, &rt](const auto& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, EvMyInfo>) {
            std::lock_guard<std::mutex> lock(devices_mu_);
            rt->my_node_num = e.my_node_num;
            rt->db->set_my_node_num(e.my_node_num);
        } else if constexpr (std::is_same_v<T, EvMetadata>) {
            std::lock_guard<std::mutex> lock(devices_mu_);
            rt->firmware_version = e.firmware_version;
            rt->hw_model = e.hw_model;
        } else if constexpr (std::is_same_v<T, EvConfigComplete>) {
            std::lock_guard<std::mutex> lock(devices_mu_);
            rt->config_complete = true;
            // Pull our own long/short name from the node DB now that we
            // know our node num.
            auto me = rt->db->get(rt->my_node_num);
            if (me) {
                rt->my_long_name = me->long_name;
                rt->my_short_name = me->short_name;
            }
            if ((e.device.find("tcp:") == 0 || e.device.find("stream:") == 0 || e.device.find("mesh:") == 0) && sync_manager_) {
                sync_manager_->initiate_sync();
            }
        } else if constexpr (std::is_same_v<T, EvPositionReceived>) {
            rt->db->update_position(e.from_node, e.latitude, e.longitude, e.altitude);
            
            uint64_t ts = e.rx_time;
            if (ts == 0) ts = static_cast<uint64_t>(std::time(nullptr));
            db_.insert_location(e.device, e.from_node, e.latitude, e.longitude, e.altitude, ts);
            
            Database::LocationRow loc;
            loc.device = e.device;
            loc.node_num = e.from_node;
            loc.latitude = e.latitude;
            loc.longitude = e.longitude;
            loc.altitude = e.altitude;
            loc.ts = ts;
            if (sync_manager_) {
                sync_manager_->push_location(loc);
            }
        } else if constexpr (std::is_same_v<T, EvNodeUpdated>) {
            auto old = rt->db->get(e.node.node_num);
            Node merged = e.node;
            if (old) {
                if (merged.long_name.empty()) merged.long_name = old->long_name;
                if (merged.short_name.empty()) merged.short_name = old->short_name;
                if (merged.hw_model.empty()) merged.hw_model = old->hw_model;
                if (merged.role.empty()) merged.role = old->role;
                if (!merged.battery_level.has_value()) merged.battery_level = old->battery_level;
                if (!merged.voltage.has_value()) merged.voltage = old->voltage;
                if (!merged.channel_util.has_value()) merged.channel_util = old->channel_util;
                if (!merged.air_util_tx.has_value()) merged.air_util_tx = old->air_util_tx;
                if (!merged.uptime_seconds.has_value()) merged.uptime_seconds = old->uptime_seconds;
                if (!merged.temperature.has_value()) merged.temperature = old->temperature;
                if (!merged.relative_humidity.has_value()) merged.relative_humidity = old->relative_humidity;
                if (!merged.barometric_pressure.has_value()) merged.barometric_pressure = old->barometric_pressure;
                if (!merged.gas_resistance.has_value()) merged.gas_resistance = old->gas_resistance;
                if (!merged.iaq.has_value()) merged.iaq = old->iaq;
                if (!merged.pm25.has_value()) merged.pm25 = old->pm25;
                if (!merged.co2.has_value()) merged.co2 = old->co2;
                if (!merged.current.has_value()) merged.current = old->current;
                if (!merged.snr.has_value()) merged.snr = old->snr;
                if (!merged.hops_away.has_value()) merged.hops_away = old->hops_away;
                if (!merged.last_heard.has_value()) merged.last_heard = old->last_heard;
                if (!merged.latitude.has_value()) merged.latitude = old->latitude;
                if (!merged.longitude.has_value()) merged.longitude = old->longitude;
                if (!merged.altitude.has_value()) merged.altitude = old->altitude;
                if (!merged.has_public_key && old->has_public_key) {
                    merged.has_public_key = true;
                    merged.public_key = old->public_key;
                }
            }
            rt->db->upsert_node(merged);
            db_.upsert_node(e.device, merged);
            if (e.node.latitude.has_value() || e.node.longitude.has_value()) {
                uint64_t ts = e.node.last_heard.value_or(0);
                if (ts == 0) ts = static_cast<uint64_t>(std::time(nullptr));
                db_.insert_location(e.device, e.node.node_num, 
                                    e.node.latitude.value_or(0.0), 
                                    e.node.longitude.value_or(0.0), 
                                    e.node.altitude.value_or(0), ts);
                Database::LocationRow loc;
                loc.device = e.device;
                loc.node_num = e.node.node_num;
                loc.latitude = e.node.latitude.value_or(0.0);
                loc.longitude = e.node.longitude.value_or(0.0);
                loc.altitude = e.node.altitude.value_or(0);
                loc.ts = ts;
                if (sync_manager_) sync_manager_->push_location(loc);
            }

            bool has_telemetry = e.node.battery_level.has_value() ||
                                 e.node.voltage.has_value() ||
                                 e.node.channel_util.has_value() ||
                                 e.node.air_util_tx.has_value() ||
                                 e.node.uptime_seconds.has_value() ||
                                 e.node.temperature.has_value() ||
                                 e.node.relative_humidity.has_value() ||
                                 e.node.barometric_pressure.has_value() ||
                                 e.node.gas_resistance.has_value() ||
                                 e.node.iaq.has_value() ||
                                 e.node.pm25.has_value() ||
                                 e.node.co2.has_value() ||
                                 e.node.current.has_value();
            if (has_telemetry) {
                uint64_t ts = e.node.last_heard.value_or(0);
                if (ts == 0) ts = static_cast<uint64_t>(std::time(nullptr));
                Database::TelemetryRow telem;
                telem.device = e.device;
                telem.node_num = e.node.node_num;
                telem.ts = ts;
                telem.battery_level = e.node.battery_level;
                telem.voltage = e.node.voltage;
                telem.channel_util = e.node.channel_util;
                telem.air_util_tx = e.node.air_util_tx;
                telem.uptime_seconds = e.node.uptime_seconds;
                telem.temperature = e.node.temperature;
                telem.relative_humidity = e.node.relative_humidity;
                telem.barometric_pressure = e.node.barometric_pressure;
                telem.gas_resistance = e.node.gas_resistance;
                telem.iaq = e.node.iaq;
                telem.pm25 = e.node.pm25;
                telem.co2 = e.node.co2;
                telem.current = e.node.current;
                telem.snr = e.node.snr;
                telem.hops_away = e.node.hops_away;
                db_.insert_telemetry(telem);
                if (sync_manager_) sync_manager_->push_telemetry(telem);
            }
        } else if constexpr (std::is_same_v<T, EvChannelUpdated>) {
            rt->db->upsert_channel(e.channel);
            db_.upsert_channel(e.device, e.channel);
        } else if constexpr (std::is_same_v<T, EvTextReceived>) {
            LOG_INFO() << "Received text message. Broadcast: " << e.broadcast 
                       << ", From: " << e.from_node << ", To: " << e.to_node 
                       << ", Text: " << e.text;
            // Dedup: check if this message already arrived via another device.
            if (is_duplicate(e.from_node, e.packet_id)) {
                StoredMessage dup;
                dup.device = e.device;
                if (e.broadcast) {
                    dup.window_kind = "channel";
                    dup.window_target = e.channel_idx;
                } else {
                    dup.window_kind = "dm";
                    uint32_t peer = e.from_node;
                    if (rt->my_node_num && e.from_node == rt->my_node_num) {
                        peer = e.to_node;
                    }
                    dup.window_target = peer;
                }
                dup.direction = "in";
                dup.from_node = e.from_node;
                dup.to_node = e.to_node;
                dup.channel_idx = e.channel_idx;
                dup.text = e.text;
                dup.ts = e.rx_time ? e.rx_time :
                       std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch()).count();
                dup.packet_id = e.packet_id;
                dup.rx_snr = e.rx_snr;
                dup.rx_rssi = e.rx_rssi;
                dup.hop_start = e.hop_start;
                dup.hop_limit = e.hop_limit;
                dup.relay_node = e.relay_node;
                db_.insert_message(dup);
                return;
            }
            // Persist inbound message.
            StoredMessage m;
            m.device = e.device;
            if (e.broadcast) {
                m.window_kind = "channel";
                m.window_target = e.channel_idx;
            } else {
                m.window_kind = "dm";
                uint32_t peer = e.from_node;
                if (rt->my_node_num && e.from_node == rt->my_node_num) {
                    peer = e.to_node;
                }
                m.window_target = peer;
            }
            m.direction = "in";
            m.from_node = e.from_node;
            m.to_node = e.to_node;
            m.channel_idx = e.channel_idx;
            m.text = e.text;
            m.ts = e.rx_time ? e.rx_time :
                   std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch()).count();
            m.packet_id = e.packet_id;
            m.rx_snr = e.rx_snr;
            m.rx_rssi = e.rx_rssi;
            m.hop_start = e.hop_start;
            m.hop_limit = e.hop_limit;
            m.relay_node = e.relay_node;
            m.rowid = db_.insert_message(m);
            if (sync_manager_) sync_manager_->push_message(m);
        } else if constexpr (std::is_same_v<T, EvAckReceived>) {
            // Match pending outbound message and update its ack state.
            std::lock_guard<std::mutex> lock(devices_mu_);
            auto pit = rt->pending_acks.find(e.packet_id);
            if (pit != rt->pending_acks.end()) {
                db_.update_ack_state(pit->second,
                                     e.success ? "acked" : "naked");
                rt->pending_acks.erase(pit);
            }
        } else if constexpr (std::is_same_v<T, EvConfigLine>) {
            auto& dst = rt->config_lines;
            dst.clear();
            std::istringstream iss(e.line);
            std::string ln;
            while (std::getline(iss, ln)) {
                if (!ln.empty()) dst.push_back(ln);
            }
        } else if constexpr (std::is_same_v<T, EvConfigBytes>) {
            if (e.is_module) {
                rt->raw_module_config = e.bytes;
            } else {
                rt->raw_config = e.bytes;
            }
            auto lines = MeshCodec::decode_config_lines(e.bytes, e.is_module);
            auto& dst = rt->config_lines;
            if (!e.is_module) dst.clear(); // Clear on base Config, append ModuleConfig
            for (const auto& line : lines) {
                dst.push_back(line);
            }
        } else if constexpr (std::is_same_v<T, EvRawPacket>) {
            rt->raw_packets.push_back(e);
            if (rt->raw_packets.size() > DeviceRuntime::kMaxRawPackets)
                rt->raw_packets.erase(rt->raw_packets.begin());
        } else if constexpr (std::is_same_v<T, EvRawRxBytes>) {
#ifdef ENABLE_MESH_NET
            if (stream_server_) {
                stream_server_->broadcast(e.bytes);
            }
#endif
        } else if constexpr (std::is_same_v<T, EvDbSyncPayload>) {
            if (sync_manager_) sync_manager_->handle_sync_payload(e.device, e.payload);
        } else {
            // EvConnected / EvDisconnected / EvLogLine / EvError / EvSendRawToRadio: no DB work.
        }
    }, ev);
}

void MeshService::add_event_listener(EventListener cb) {
    std::lock_guard<std::mutex> lock(listeners_mu_);
    listeners_.push_back(std::move(cb));
}

void MeshService::dispatch_to_ui(MeshEvent ev) {
    {
        std::lock_guard<std::mutex> lock(listeners_mu_);
        for (const auto& cb : listeners_) {
            try {
                cb(ev);
            } catch (...) {}
        }
    }
    if (ui_queue_) {
        ui_queue_->push(std::move(ev));
        if (ui_wake_) ui_wake_->notify();
    }
}

void MeshService::send_db_sync(const std::string& payload) {
#ifdef ENABLE_MESH_NET
    unsigned char marker = 0xD0;
    std::string to_send = Compression::compress_adaptive(payload, marker, 0xD0, 0xD1);

    if (stream_server_) {
        stream_server_->broadcast(to_send, marker);
    }
    std::lock_guard<std::mutex> lock(devices_mu_);
    for (auto& [id, dev] : devices_) {
        if (dev->stream) {
            dev->stream->send_to_radio(to_send, marker);
        }
    }
#endif
}

} // namespace meshcli
