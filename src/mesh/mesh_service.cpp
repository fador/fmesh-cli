#include "mesh_service.h"

#include "mesh/mesh_codec.h"
#include "util/log.h"

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
}

MeshService::~MeshService() { disconnect_all(); }

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
#ifndef _WIN32
        rt->client = std::make_unique<BluezClient>(spec, std::move(sink));
        id = rt->client->start(pair);
        if (id.empty()) return {};
#else
        EvError ev;
        ev.device = "";
        ev.message = "BLE is not supported on Windows yet.";
        dispatch_to_ui(std::move(ev));
        return {};
#endif
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
#ifndef _WIN32
    if (rt->client) { rt->client->stop(); rt->client.reset(); }
#endif
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
    } else if (!spec.serial_port.empty()) {
        int fd = serial_open(spec.serial_port, spec.serial_baud);
        if (fd < 0) return false;
        rt->stream = std::make_unique<StreamClient>(fd, "serial:" + spec.serial_port, std::move(sink));
        new_id = rt->stream->start();
    } else {
#ifndef _WIN32
        rt->client = std::make_unique<BluezClient>(spec, std::move(sink));
        new_id = rt->client->start(/*pair=*/false);
        if (new_id.empty()) return false;
#else
        return false;
#endif
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
#ifndef _WIN32
    if (rt->client) {
        rt->client->send_to_radio(MeshCodec::encode_disconnect());
        rt->client->stop();
    }
#endif
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
#ifndef _WIN32
        if (rt->client) {
            rt->client->send_to_radio(MeshCodec::encode_disconnect());
            rt->client->stop();
        }
#endif
        if (rt->stream) {
            rt->stream->send_to_radio(MeshCodec::encode_disconnect());
            rt->stream->stop();
        }
    }
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
    std::shared_ptr<DeviceRuntime> rt;
    {
        std::lock_guard<std::mutex> lock(devices_mu_);
        auto it = devices_.find(device_id);
        if (it == devices_.end()) return 0;
        rt = it->second;
    }
#ifndef _WIN32
    if (!rt->client && !rt->stream) return 0;
    if (rt->client && !rt->client->is_connected()) return 0;
#else
    if (!rt->stream) return 0;
#endif
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
#ifndef _WIN32
    if (rt->client && !rt->client->send_to_radio(bytes)) return 0;
#endif
    if (rt->stream && !rt->stream->send_to_radio(bytes)) return 0;

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
    m.from_node = rt->my_node_num;
    m.to_node = to_node;
    m.channel_idx = channel_idx;
    m.text = text;
    m.ts = std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
    m.packet_id = pid;
    m.ack_state = want_ack ? "pending" : "";
    int64_t rowid = db_.insert_message(m);
    if (want_ack) {
        std::lock_guard<std::mutex> lock(devices_mu_);
        rt->pending_acks[pid] = rowid;
    }
    return pid;
}

std::vector<std::string> MeshService::device_ids() const {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lock(devices_mu_);
    out.reserve(devices_.size());
    for (const auto& [id, _] : devices_) out.push_back(id);
    return out;
}

const NodeDb* MeshService::db_for(const std::string& device_id) const {
    std::lock_guard<std::mutex> lock(devices_mu_);
    auto it = devices_.find(device_id);
    if (it == devices_.end()) return nullptr;
    return it->second->db.get();
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
    std::lock_guard<std::mutex> lock(devices_mu_);
    auto it = devices_.find(device_id);
    if (it == devices_.end()) return {};
    return it->second->display_name;
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

// ---------------------------------------------------------------------------
// event handling (runs on the BLE thread of whichever device emitted it)
// ---------------------------------------------------------------------------

void MeshService::handle_event(const std::shared_ptr<DeviceRuntime>& rt, const MeshEvent& ev) {
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
        } else if constexpr (std::is_same_v<T, EvNodeUpdated>) {
            rt->db->upsert_node(e.node);
            db_.upsert_node(e.device, e.node);
        } else if constexpr (std::is_same_v<T, EvChannelUpdated>) {
            rt->db->upsert_channel(e.channel);
            db_.upsert_channel(e.device, e.channel);
        } else if constexpr (std::is_same_v<T, EvTextReceived>) {
            // Dedup: check if this message already arrived via another device.
            if (is_duplicate(e.from_node, e.packet_id)) {
                StoredMessage dup;
                dup.device = e.device;
                if (e.broadcast) {
                    dup.window_kind = "channel";
                    dup.window_target = e.channel_idx;
                } else {
                    dup.window_kind = "dm";
                    dup.window_target = e.from_node;
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
                m.window_target = e.from_node;
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
            db_.insert_message(m);
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
        } else if constexpr (std::is_same_v<T, EvRawPacket>) {
            rt->raw_packets.push_back(e);
            if (rt->raw_packets.size() > DeviceRuntime::kMaxRawPackets)
                rt->raw_packets.erase(rt->raw_packets.begin());
        } else {
            // EvConnected / EvDisconnected / EvLogLine / EvError: no DB work.
        }
    }, ev);
}

void MeshService::dispatch_to_ui(MeshEvent ev) {
    if (ui_queue_) {
        ui_queue_->push(std::move(ev));
        if (ui_wake_) ui_wake_->notify();
    }
}

} // namespace meshcli
