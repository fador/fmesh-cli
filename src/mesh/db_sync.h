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

private:
    Database& db_;
    MeshService& mesh_;

    void handle_inventory(const std::string& device, const std::string& json_str);
    void handle_request(const std::string& device, const std::string& json_str);
    void handle_data(const std::string& device, const std::string& json_str);
};

} // namespace meshcli
