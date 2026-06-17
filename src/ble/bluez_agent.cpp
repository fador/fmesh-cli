#include "bluez_agent.h"

#include "util/log.h"

namespace meshcli {

BluezAgent::BluezAgent(std::string pin, std::function<void(std::string)> log)
    : pin_(std::move(pin)), log_(std::move(log)) {}

BluezAgent::~BluezAgent() { unregister(); }

void BluezAgent::register_on(sdbus::IConnection& conn, const std::string& object_path) {
    path_ = object_path;
    conn_ = &conn;
    object_ = sdbus::createObject(conn, path_);

    object_->registerMethod("Release").onInterface("org.bluez.Agent1").implementedAs([this] { release(); });
    object_->registerMethod("RequestPinCode").onInterface("org.bluez.Agent1")
        .implementedAs([this](std::string device) { return request_pin_code(device); });
    object_->registerMethod("DisplayPinCode").onInterface("org.bluez.Agent1")
        .implementedAs([this](std::string device, std::string pincode) { display_pin_code(device, pincode); });
    object_->registerMethod("RequestPasskey").onInterface("org.bluez.Agent1")
        .implementedAs([this](std::string device) -> uint32_t { return request_passkey(device); });
    object_->registerMethod("DisplayPasskey").onInterface("org.bluez.Agent1")
        .implementedAs([this](std::string device, uint32_t passkey, uint16_t entered) { display_passkey(device, passkey, entered); });
    object_->registerMethod("RequestConfirmation").onInterface("org.bluez.Agent1")
        .implementedAs([this](std::string device, uint32_t passkey) { request_confirmation(device, passkey); });
    object_->registerMethod("AuthorizeService").onInterface("org.bluez.Agent1")
        .implementedAs([this](std::string device, std::string uuid) { authorize_service(device, uuid); });
    object_->registerMethod("Cancel").onInterface("org.bluez.Agent1").implementedAs([this] { cancel(); });
    object_->finishRegistration();

    // RegisterAgent(path, capability)
    auto agent_mgr = sdbus::createProxy(*conn_, "org.bluez", "/org/bluez");
    try {
        agent_mgr->callMethod("RegisterAgent")
            .onInterface("org.bluez.AgentManager1")
            .withArguments(sdbus::ObjectPath(path_), std::string("KeyboardDisplay"));
        agent_mgr->callMethod("RequestDefaultAgent")
            .onInterface("org.bluez.AgentManager1")
            .withArguments(sdbus::ObjectPath(path_));
    } catch (const sdbus::Error& e) {
        LOG_ERROR() << "Agent registration failed: " << e.what();
        throw;
    }
    emit_log("Pairing agent registered (capability=KeyboardDisplay)");
}

void BluezAgent::unregister() {
    if (!conn_ || path_.empty()) return;
    auto agent_mgr = sdbus::createProxy(*conn_, "org.bluez", "/org/bluez");
    try {
        agent_mgr->callMethod("UnregisterAgent")
            .onInterface("org.bluez.AgentManager1")
            .withArguments(sdbus::ObjectPath(path_));
    } catch (const sdbus::Error& e) {
        LOG_WARN() << "UnregisterAgent failed: " << e.what();
    }
    object_.reset();
    conn_ = nullptr;
    path_.clear();
}

// --- Agent1 method bodies --------------------------------------------------

void BluezAgent::release() {
    emit_log("agent released");
    object_.reset();  // agent is gone
}

std::string BluezAgent::request_pin_code(const std::string& device) {
    LOG_INFO() << "PIN requested for " << device << " -> supplying " << pin_;
    emit_log("supplying PIN for " + device);
    return pin_;
}

void BluezAgent::display_pin_code(const std::string& device, const std::string& pincode) {
    LOG_INFO() << "device " << device << " displaying PIN " << pincode;
    emit_log("device displays PIN " + pincode);
}

uint32_t BluezAgent::request_passkey(const std::string& device) {
    LOG_INFO() << "Passkey requested for " << device << " -> supplying " << pin_;
    try { return static_cast<uint32_t>(std::stoul(pin_)); }
    catch (...) { return 0; }
}

void BluezAgent::display_passkey(const std::string& device, uint32_t passkey, uint16_t entered) {
    LOG_INFO() << "device " << device << " passkey " << passkey << " (" << entered << "/6 entered)";
}

void BluezAgent::request_confirmation(const std::string& device, uint32_t passkey) {
    // Auto-confirm: this is a pairing agent for a known device with a known PIN.
    LOG_INFO() << "auto-confirming pairing for " << device << " (passkey=" << passkey << ")";
    emit_log("auto-confirming pair for " + device);
    // Returning normally == confirmed.
}

void BluezAgent::authorize_service(const std::string& device, const std::string& uuid) {
    LOG_DEBUG() << "authorizing service " << uuid << " on " << device;
    // Returning normally == authorized.
}

void BluezAgent::cancel() {
    LOG_WARN() << "pairing cancelled by device";
    emit_log("pairing cancelled");
}

} // namespace meshcli
