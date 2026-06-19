#pragma once

#include "store/database.h"
#include "mesh/mesh_service.h"
#include <memory>
#include <string>

namespace meshcli {

class DbSyncManager {
public:
    DbSyncManager(Database& db, MeshService& mesh);

    // Call this from the UI event loop when an EvDbSyncPayload is received.
    void handle_sync_payload(const std::string& device, const std::string& payload);

    // Call this to initiate a sync (sends SyncInventory).
    void initiate_sync();

    // Call this whenever a new message is received or a local update occurs.
    void push_message(const StoredMessage& msg);
    void push_location(const Database::LocationRow& loc);
    
    // Broadcast local physical devices to remote streams
    void push_devices(const std::vector<std::pair<std::string, uint32_t>>& local_devices);
    
    // Send a raw packet to a specific physical radio
    void send_raw_to_device(const std::string& target_original_id, const std::string& bytes);

private:
    Database& db_;
    MeshService& mesh_;

    void handle_inventory(const std::string& device, const std::string& json_str);
    void handle_request(const std::string& device, const std::string& json_str);
    void handle_data(const std::string& device, const std::string& json_str);
    void handle_devices(const std::string& device, const std::string& json_str);
    void handle_send_raw(const std::string& json_str);
};

} // namespace meshcli
