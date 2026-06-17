#pragma once

#include <sdbus-c++/sdbus-c++.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace meshcli {

// A minimal org.bluez.Agent1 implementation. It auto-supplies the configured
// PIN for RequestPinCode/RequestPasskey, auto-confirms RequestConfirmation,
// and auto-authorises every service. Used by the `pair` flow to make pairing
// non-interactive. Runs on whichever sdbus-c++ connection owns it.
class BluezAgent {
public:
    // `pin` is the 6-digit PIN/passkey (e.g. "123456"). `log` is optional.
    BluezAgent(std::string pin, std::function<void(std::string)> log = {});
    ~BluezAgent();

    // Register this agent on `conn` at `object_path` and make it the default
    // agent. Called RegisterAgent(path, capability="KeyboardDisplay") then
    // RequestDefaultAgent.
    void register_on(sdbus::IConnection& conn, const std::string& object_path);

    // Deregister and release. Safe to call even if not registered.
    void unregister();

private:
    std::string pin_;
    std::function<void(std::string)> log_;
    std::unique_ptr<sdbus::IObject> object_;
    std::string path_;
    sdbus::IConnection* conn_ = nullptr;

    void emit_log(const std::string& s) { if (log_) log_(s); }

    // Agent1 method implementations.
    void release();
    std::string request_pin_code(const std::string& device);
    void display_pin_code(const std::string& device, const std::string& pincode);
    uint32_t request_passkey(const std::string& device);
    void display_passkey(const std::string& device, uint32_t passkey, uint16_t entered);
    void request_confirmation(const std::string& device, uint32_t passkey);
    void authorize_service(const std::string& device, const std::string& uuid);
    void cancel();
};

} // namespace meshcli
