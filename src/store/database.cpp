#include "database.h"

#include "util/log.h"

#include <sqlite3.h>

#include <algorithm>

namespace meshcli {

namespace {

const char* kSchemaSql = R"SQL(
CREATE TABLE IF NOT EXISTS nodes(
    device      TEXT NOT NULL,
    node_num    INTEGER NOT NULL,
    node_id     TEXT,
    long_name   TEXT,
    short_name  TEXT,
    hw_model    TEXT,
    role        TEXT,
    battery     INTEGER,
    voltage     REAL,
    snr         REAL,
    hops_away   INTEGER,
    last_heard  INTEGER,
    PRIMARY KEY (device, node_num)
);
CREATE TABLE IF NOT EXISTS channels(
    device    TEXT NOT NULL,
    idx       INTEGER NOT NULL,
    name      TEXT,
    role      TEXT,
    has_psk   INTEGER,
    PRIMARY KEY (device, idx)
);
CREATE TABLE IF NOT EXISTS messages(
    rowid         INTEGER PRIMARY KEY AUTOINCREMENT,
    device        TEXT NOT NULL,
    window_kind   TEXT NOT NULL,
    window_target INTEGER NOT NULL,
    direction     TEXT NOT NULL,
    from_node     INTEGER,
    to_node       INTEGER,
    channel_idx   INTEGER,
    text          TEXT,
    ts            INTEGER,
    packet_id     INTEGER,
    ack_state     TEXT
);
CREATE INDEX IF NOT EXISTS idx_messages_window
    ON messages(device, window_kind, window_target, ts);
CREATE TABLE IF NOT EXISTS location_history(
    rowid       INTEGER PRIMARY KEY AUTOINCREMENT,
    device      TEXT NOT NULL,
    node_num    INTEGER NOT NULL,
    latitude    REAL NOT NULL,
    longitude   REAL NOT NULL,
    altitude    INTEGER,
    ts          INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_location_history_node
    ON location_history(device, node_num, ts);
)SQL";

int null_cb(void*, int, char**, char**) { return 0; }

} // namespace

Database::Database() = default;

Database::~Database() { close(); }

bool Database::open(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        LOG_ERROR() << "cannot open db " << path << ": " << sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    if (!exec(kSchemaSql)) {
        close();
        return false;
    }
    LOG_INFO() << "db opened: " << path;
    return true;
}

void Database::close() {
    if (db_) { checkpoint(); sqlite3_close(db_); db_ = nullptr; }
}

bool Database::exec(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), null_cb, nullptr, &err) != SQLITE_OK) {
        LOG_ERROR() << "db error: " << (err ? err : "(null)");
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

// --- nodes / channels ------------------------------------------------------

void Database::upsert_node(const std::string& device, const Node& n) {
    if (!db_) return;
    const char* sql =
        "INSERT INTO nodes(device,node_num,node_id,long_name,short_name,hw_model,role,"
        "battery,voltage,snr,hops_away,last_heard) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(device,node_num) DO UPDATE SET "
        "node_id=excluded.node_id,long_name=excluded.long_name,"
        "short_name=excluded.short_name,hw_model=excluded.hw_model,role=excluded.role,"
        "battery=excluded.battery,voltage=excluded.voltage,snr=excluded.snr,"
        "hops_away=excluded.hops_away,last_heard=excluded.last_heard";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, device.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, n.node_num);
    sqlite3_bind_text(st, 3, n.node_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, n.long_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, n.short_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 6, n.hw_model.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 7, n.role.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 8, n.battery_level.value_or(-1));
    sqlite3_bind_double(st, 9, n.voltage.value_or(0));
    sqlite3_bind_double(st, 10, n.snr.value_or(0));
    sqlite3_bind_int(st, 11, n.hops_away.value_or(-1));
    sqlite3_bind_int64(st, 12, n.last_heard.value_or(0));
    sqlite3_step(st);
    sqlite3_finalize(st);
    maybe_checkpoint();
}

void Database::upsert_channel(const std::string& device, const Channel& c) {
    if (!db_) return;
    const char* sql =
        "INSERT INTO channels(device,idx,name,role,has_psk) VALUES(?,?,?,?,?) "
        "ON CONFLICT(device,idx) DO UPDATE SET name=excluded.name,role=excluded.role,"
        "has_psk=excluded.has_psk";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, device.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, static_cast<int64_t>(c.index));
    sqlite3_bind_text(st, 3, c.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, c.role.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 5, c.has_psk ? 1 : 0);
    sqlite3_step(st);
    sqlite3_finalize(st);
    maybe_checkpoint();
}

void Database::load_nodes(const std::string& device, NodeDb& db) {
    if (!db_) return;
    const char* sql = "SELECT node_num,node_id,long_name,short_name,hw_model,role,"
                      "battery,voltage,snr,hops_away,last_heard FROM nodes WHERE device=?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, device.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        Node n;
        n.node_num = static_cast<uint32_t>(sqlite3_column_int64(st, 0));
        if (auto* p = sqlite3_column_text(st, 1)) n.node_id = reinterpret_cast<const char*>(p);
        if (auto* p = sqlite3_column_text(st, 2)) n.long_name = reinterpret_cast<const char*>(p);
        if (auto* p = sqlite3_column_text(st, 3)) n.short_name = reinterpret_cast<const char*>(p);
        if (auto* p = sqlite3_column_text(st, 4)) n.hw_model = reinterpret_cast<const char*>(p);
        if (auto* p = sqlite3_column_text(st, 5)) n.role = reinterpret_cast<const char*>(p);
        int b = sqlite3_column_int(st, 6);
        if (b >= 0) n.battery_level = static_cast<uint8_t>(b);
        if (sqlite3_column_type(st, 7) != SQLITE_NULL)
            n.voltage = static_cast<float>(sqlite3_column_double(st, 7));
        if (sqlite3_column_type(st, 8) != SQLITE_NULL)
            n.snr = static_cast<float>(sqlite3_column_double(st, 8));
        int h = sqlite3_column_int(st, 9);
        if (h >= 0) n.hops_away = static_cast<uint32_t>(h);
        n.last_heard = static_cast<uint64_t>(sqlite3_column_int64(st, 10));
        db.upsert_node(std::move(n));
    }
    sqlite3_finalize(st);
}

void Database::load_channels(const std::string& device, NodeDb& db) {
    if (!db_) return;
    const char* sql = "SELECT idx,name,role,has_psk FROM channels WHERE device=?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, device.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        Channel c;
        c.index = static_cast<uint32_t>(sqlite3_column_int64(st, 0));
        if (auto* p = sqlite3_column_text(st, 1)) c.name = reinterpret_cast<const char*>(p);
        if (auto* p = sqlite3_column_text(st, 2)) c.role = reinterpret_cast<const char*>(p);
        c.has_psk = sqlite3_column_int(st, 3) != 0;
        db.upsert_channel(std::move(c));
    }
    sqlite3_finalize(st);
}

// --- offline history loading -----------------------------------------------

std::vector<std::string> Database::get_all_devices() {
    std::vector<std::string> out;
    if (!db_) return out;
    const char* sql = "SELECT DISTINCT device FROM messages UNION SELECT DISTINCT device FROM nodes UNION SELECT DISTINCT device FROM channels";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (auto* p = sqlite3_column_text(st, 0)) out.push_back(reinterpret_cast<const char*>(p));
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<WindowKey> Database::get_all_windows(const std::string& device) {
    std::vector<WindowKey> out;
    if (!db_) return out;
    const char* sql = "SELECT DISTINCT window_kind, window_target FROM messages WHERE device=?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_text(st, 1, device.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        WindowKey w;
        w.device = device;
        if (auto* p = sqlite3_column_text(st, 0)) w.kind = reinterpret_cast<const char*>(p);
        w.target = static_cast<uint32_t>(sqlite3_column_int64(st, 1));
        out.push_back(std::move(w));
    }
    sqlite3_finalize(st);
    return out;
}

// --- messages --------------------------------------------------------------

int64_t Database::insert_message(const StoredMessage& m) {
    if (!db_) return 0;
    const char* sql =
        "INSERT INTO messages(device,window_kind,window_target,direction,from_node,to_node,"
        "channel_idx,text,ts,packet_id,ack_state) VALUES(?,?,?,?,?,?,?,?,?,?,?)";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, m.device.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, m.window_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, m.window_target);
    sqlite3_bind_text(st, 4, m.direction.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, m.from_node);
    sqlite3_bind_int64(st, 6, m.to_node);
    sqlite3_bind_int64(st, 7, m.channel_idx);
    sqlite3_bind_text(st, 8, m.text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 9, m.ts);
    sqlite3_bind_int64(st, 10, m.packet_id);
    sqlite3_bind_text(st, 11, m.ack_state.c_str(), -1, SQLITE_TRANSIENT);
    int64_t rowid = 0;
    if (sqlite3_step(st) == SQLITE_DONE) rowid = sqlite3_last_insert_rowid(db_);
    sqlite3_finalize(st);
    maybe_checkpoint();
    return rowid;
}

void Database::update_ack_state(int64_t rowid, const std::string& ack_state) {
    if (!db_ || !rowid) return;
    const char* sql = "UPDATE messages SET ack_state=? WHERE rowid=?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, ack_state.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, rowid);
    sqlite3_step(st);
    sqlite3_finalize(st);
    maybe_checkpoint();
}

std::vector<StoredMessage> Database::recent_messages(const WindowKey& w, int limit) {
    std::vector<StoredMessage> out;
    if (!db_) return out;
    const char* sql =
        "SELECT rowid,device,window_kind,window_target,direction,from_node,to_node,"
        "channel_idx,text,ts,packet_id,ack_state FROM messages "
        "WHERE device=? AND window_kind=? AND window_target=? "
        "ORDER BY ts DESC LIMIT ?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_text(st, 1, w.device.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, w.kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, w.target);
    sqlite3_bind_int(st, 4, limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        StoredMessage m;
        m.rowid = sqlite3_column_int64(st, 0);
        m.device = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        m.window_kind = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
        m.window_target = static_cast<uint32_t>(sqlite3_column_int64(st, 3));
        m.direction = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
        m.from_node = static_cast<uint32_t>(sqlite3_column_int64(st, 5));
        m.to_node = static_cast<uint32_t>(sqlite3_column_int64(st, 6));
        m.channel_idx = static_cast<uint32_t>(sqlite3_column_int64(st, 7));
        if (auto* p = sqlite3_column_text(st, 8)) m.text = reinterpret_cast<const char*>(p);
        m.ts = static_cast<uint64_t>(sqlite3_column_int64(st, 9));
        m.packet_id = static_cast<uint32_t>(sqlite3_column_int64(st, 10));
        if (auto* p = sqlite3_column_text(st, 11)) m.ack_state = reinterpret_cast<const char*>(p);
        out.push_back(std::move(m));
    }
    sqlite3_finalize(st);
    std::reverse(out.begin(), out.end());
    return out;
}

std::optional<StoredMessage> Database::find_by_packet_id(uint32_t packet_id) {
    if (!db_) return std::nullopt;
    const char* sql = "SELECT rowid,device,window_kind,window_target,direction,"
                      "from_node,to_node,channel_idx,text,ts,packet_id,ack_state "
                      "FROM messages WHERE packet_id=? ORDER BY rowid DESC LIMIT 1";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_int64(st, 1, packet_id);
    std::optional<StoredMessage> out;
    if (sqlite3_step(st) == SQLITE_ROW) {
        StoredMessage m;
        m.rowid = sqlite3_column_int64(st, 0);
        m.device = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        m.window_kind = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
        m.window_target = static_cast<uint32_t>(sqlite3_column_int64(st, 3));
        m.direction = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
        m.from_node = static_cast<uint32_t>(sqlite3_column_int64(st, 5));
        m.to_node = static_cast<uint32_t>(sqlite3_column_int64(st, 6));
        m.channel_idx = static_cast<uint32_t>(sqlite3_column_int64(st, 7));
        if (auto* p = sqlite3_column_text(st, 8)) m.text = reinterpret_cast<const char*>(p);
        m.ts = static_cast<uint64_t>(sqlite3_column_int64(st, 9));
        m.packet_id = static_cast<uint32_t>(sqlite3_column_int64(st, 10));
        if (auto* p = sqlite3_column_text(st, 11)) m.ack_state = reinterpret_cast<const char*>(p);
        out = std::move(m);
    }
    sqlite3_finalize(st);
    return out;
}

void Database::checkpoint() {
    if (!db_) return;
    sqlite3_exec(db_, "PRAGMA wal_checkpoint(TRUNCATE);", nullptr, nullptr, nullptr);
}

// --- location history ------------------------------------------------------

void Database::insert_location(const std::string& device, uint32_t node_num, double lat, double lon, int altitude, uint64_t ts) {
    if (!db_) return;
    const char* sql = "INSERT INTO location_history(device,node_num,latitude,longitude,altitude,ts) VALUES(?,?,?,?,?,?)";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, device.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, node_num);
    sqlite3_bind_double(st, 3, lat);
    sqlite3_bind_double(st, 4, lon);
    sqlite3_bind_int(st, 5, altitude);
    sqlite3_bind_int64(st, 6, ts);
    sqlite3_step(st);
    sqlite3_finalize(st);
    maybe_checkpoint();
}

void Database::maybe_checkpoint() {
    if (++write_count_ >= 100) {
        write_count_ = 0;
        checkpoint();
    }
}

} // namespace meshcli
