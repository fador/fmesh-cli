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
    temperature REAL,
    relative_humidity REAL,
    barometric_pressure REAL,
    channel_util REAL,
    air_util_tx REAL,
    uptime_seconds INTEGER,
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
    ack_state     TEXT,
    rx_snr        REAL,
    rx_rssi       INTEGER,
    hop_start     INTEGER,
    hop_limit     INTEGER,
    relay_node    INTEGER
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
CREATE INDEX IF NOT EXISTS idx_location_device_node
    ON location_history(device, node_num, ts);
)SQL";

const char* kSelectMessagesPrefix =
    "SELECT rowid,device,window_kind,window_target,direction,from_node,to_node,"
    "channel_idx,text,ts,packet_id,ack_state,rx_snr,rx_rssi,hop_start,hop_limit,relay_node "
    "FROM messages ";

StoredMessage read_message_row(sqlite3_stmt* st) {
    StoredMessage m;
    m.rowid = sqlite3_column_int64(st, 0);
    if (auto* p = sqlite3_column_text(st, 1)) m.device = reinterpret_cast<const char*>(p);
    if (auto* p = sqlite3_column_text(st, 2)) m.window_kind = reinterpret_cast<const char*>(p);
    m.window_target = static_cast<uint32_t>(sqlite3_column_int64(st, 3));
    if (auto* p = sqlite3_column_text(st, 4)) m.direction = reinterpret_cast<const char*>(p);
    m.from_node = static_cast<uint32_t>(sqlite3_column_int64(st, 5));
    m.to_node = static_cast<uint32_t>(sqlite3_column_int64(st, 6));
    m.channel_idx = static_cast<uint32_t>(sqlite3_column_int64(st, 7));
    if (auto* p = sqlite3_column_text(st, 8)) m.text = reinterpret_cast<const char*>(p);
    m.ts = static_cast<uint64_t>(sqlite3_column_int64(st, 9));
    m.packet_id = static_cast<uint32_t>(sqlite3_column_int64(st, 10));
    if (auto* p = sqlite3_column_text(st, 11)) m.ack_state = reinterpret_cast<const char*>(p);
    m.rx_snr = static_cast<float>(sqlite3_column_double(st, 12));
    m.rx_rssi = static_cast<int32_t>(sqlite3_column_int(st, 13));
    m.hop_start = static_cast<uint32_t>(sqlite3_column_int64(st, 14));
    m.hop_limit = static_cast<uint32_t>(sqlite3_column_int64(st, 15));
    m.relay_node = static_cast<uint32_t>(sqlite3_column_int64(st, 16));
    return m;
}

int null_cb(void*, int, char**, char**) { return 0; }

} // namespace

Database::Database() = default;

Database::~Database() { close(); }

bool Database::open(const std::string& path) {
    if (db_) close();
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
    // Migration: add new telemetry columns to existing nodes tables if absent
    sqlite3_exec(db_, "ALTER TABLE nodes ADD COLUMN temperature REAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE nodes ADD COLUMN relative_humidity REAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE nodes ADD COLUMN barometric_pressure REAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE nodes ADD COLUMN channel_util REAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE nodes ADD COLUMN air_util_tx REAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE nodes ADD COLUMN uptime_seconds INTEGER;", nullptr, nullptr, nullptr);
    // Migration: add signal and hop columns to existing messages tables if absent
    sqlite3_exec(db_, "ALTER TABLE messages ADD COLUMN rx_snr REAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE messages ADD COLUMN rx_rssi INTEGER;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE messages ADD COLUMN hop_start INTEGER;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE messages ADD COLUMN hop_limit INTEGER;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE messages ADD COLUMN relay_node INTEGER;", nullptr, nullptr, nullptr);

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
        "battery,voltage,snr,hops_away,last_heard,temperature,relative_humidity,barometric_pressure,"
        "channel_util,air_util_tx,uptime_seconds) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(device,node_num) DO UPDATE SET "
        "node_id=excluded.node_id,long_name=excluded.long_name,"
        "short_name=excluded.short_name,hw_model=excluded.hw_model,role=excluded.role,"
        "battery=excluded.battery,voltage=excluded.voltage,snr=excluded.snr,"
        "hops_away=excluded.hops_away,last_heard=excluded.last_heard,"
        "temperature=excluded.temperature,relative_humidity=excluded.relative_humidity,"
        "barometric_pressure=excluded.barometric_pressure,channel_util=excluded.channel_util,"
        "air_util_tx=excluded.air_util_tx,uptime_seconds=excluded.uptime_seconds";
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
    if (n.voltage) sqlite3_bind_double(st, 9, *n.voltage);
    else sqlite3_bind_null(st, 9);
    if (n.snr) sqlite3_bind_double(st, 10, *n.snr);
    else sqlite3_bind_null(st, 10);
    sqlite3_bind_int(st, 11, n.hops_away.value_or(-1));
    sqlite3_bind_int64(st, 12, n.last_heard.value_or(0));
    if (n.temperature) sqlite3_bind_double(st, 13, *n.temperature);
    else sqlite3_bind_null(st, 13);
    if (n.relative_humidity) sqlite3_bind_double(st, 14, *n.relative_humidity);
    else sqlite3_bind_null(st, 14);
    if (n.barometric_pressure) sqlite3_bind_double(st, 15, *n.barometric_pressure);
    else sqlite3_bind_null(st, 15);
    if (n.channel_util) sqlite3_bind_double(st, 16, *n.channel_util);
    else sqlite3_bind_null(st, 16);
    if (n.air_util_tx) sqlite3_bind_double(st, 17, *n.air_util_tx);
    else sqlite3_bind_null(st, 17);
    if (n.uptime_seconds) sqlite3_bind_int64(st, 18, *n.uptime_seconds);
    else sqlite3_bind_null(st, 18);
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
                      "battery,voltage,snr,hops_away,last_heard,"
                      "temperature,relative_humidity,barometric_pressure,"
                      "channel_util,air_util_tx,uptime_seconds FROM nodes WHERE device=?";
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
        if (sqlite3_column_type(st, 11) != SQLITE_NULL)
            n.temperature = static_cast<float>(sqlite3_column_double(st, 11));
        if (sqlite3_column_type(st, 12) != SQLITE_NULL)
            n.relative_humidity = static_cast<float>(sqlite3_column_double(st, 12));
        if (sqlite3_column_type(st, 13) != SQLITE_NULL)
            n.barometric_pressure = static_cast<float>(sqlite3_column_double(st, 13));
        if (sqlite3_column_type(st, 14) != SQLITE_NULL)
            n.channel_util = static_cast<float>(sqlite3_column_double(st, 14));
        if (sqlite3_column_type(st, 15) != SQLITE_NULL)
            n.air_util_tx = static_cast<float>(sqlite3_column_double(st, 15));
        if (sqlite3_column_type(st, 16) != SQLITE_NULL)
            n.uptime_seconds = static_cast<uint32_t>(sqlite3_column_int64(st, 16));
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
    const char* sql = "SELECT DISTINCT window_kind, window_target FROM messages "
                      "WHERE (device=? OR ?='' OR device='' OR device LIKE '%' || ? OR ? LIKE '%' || device)";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_text(st, 1, device.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, device.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, device.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, device.c_str(), -1, SQLITE_TRANSIENT);
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
        "channel_idx,text,ts,packet_id,ack_state,rx_snr,rx_rssi,hop_start,hop_limit,relay_node) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
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
    sqlite3_bind_double(st, 12, m.rx_snr);
    sqlite3_bind_int(st, 13, m.rx_rssi);
    sqlite3_bind_int64(st, 14, m.hop_start);
    sqlite3_bind_int64(st, 15, m.hop_limit);
    sqlite3_bind_int64(st, 16, m.relay_node);
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
    std::string sql = std::string(kSelectMessagesPrefix) +
        "WHERE (device=? OR ?='' OR device='' OR device LIKE '%' || ? OR ? LIKE '%' || device) "
        "AND window_kind=? AND window_target=? "
        "ORDER BY ts DESC LIMIT ?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_text(st, 1, w.device.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, w.device.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, w.device.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, w.device.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, w.kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 6, w.target);
    sqlite3_bind_int(st, 7, limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        out.push_back(read_message_row(st));
    }
    sqlite3_finalize(st);
    std::reverse(out.begin(), out.end());
    return out;
}

std::optional<StoredMessage> Database::find_by_packet_id(uint32_t packet_id) {
    if (!db_) return std::nullopt;
    std::string sql = std::string(kSelectMessagesPrefix) +
        "WHERE packet_id=? ORDER BY rowid DESC LIMIT 1";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_int64(st, 1, packet_id);
    std::optional<StoredMessage> out;
    if (sqlite3_step(st) == SQLITE_ROW) {
        out = read_message_row(st);
    }
    sqlite3_finalize(st);
    return out;
}

void Database::checkpoint() {
    if (!db_) return;
    sqlite3_exec(db_, "PRAGMA wal_checkpoint(TRUNCATE);", nullptr, nullptr, nullptr);
}

// --- location history ------------------------------------------------------

bool Database::insert_location(const std::string& device, uint32_t node_num, double lat, double lon, int altitude, uint64_t ts) {
    if (!db_) return false;
    
    // Check if it already exists
    const char* check_sql = "SELECT 1 FROM location_history WHERE device=? AND node_num=? AND ts=?";
    sqlite3_stmt* check_st = nullptr;
    if (sqlite3_prepare_v2(db_, check_sql, -1, &check_st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(check_st, 1, device.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(check_st, 2, node_num);
        sqlite3_bind_int64(check_st, 3, ts);
        bool exists = (sqlite3_step(check_st) == SQLITE_ROW);
        sqlite3_finalize(check_st);
        if (exists) return false;
    }

    const char* sql = "INSERT INTO location_history(device,node_num,latitude,longitude,altitude,ts) VALUES(?,?,?,?,?,?)";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, device.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, node_num);
    sqlite3_bind_double(st, 3, lat);
    sqlite3_bind_double(st, 4, lon);
    sqlite3_bind_int(st, 5, altitude);
    sqlite3_bind_int64(st, 6, ts);
    sqlite3_step(st);
    sqlite3_finalize(st);
    maybe_checkpoint();
    return true;
}

int64_t Database::max_message_rowid() {
    if (!db_) return 0;
    const char* sql = "SELECT MAX(rowid) FROM messages";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
    int64_t ret = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        ret = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return ret;
}

std::vector<StoredMessage> Database::get_messages_after(int64_t rowid, int limit) {
    std::vector<StoredMessage> out;
    if (!db_) return out;
    std::string sql = std::string(kSelectMessagesPrefix) +
        "WHERE rowid > ? ORDER BY rowid ASC LIMIT ?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_int64(st, 1, rowid);
    sqlite3_bind_int(st, 2, limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        out.push_back(read_message_row(st));
    }
    sqlite3_finalize(st);
    return out;
}

uint64_t Database::max_message_ts() {
    if (!db_) return 0;
    const char* sql = "SELECT MAX(ts) FROM messages";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
    uint64_t ret = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        ret = static_cast<uint64_t>(sqlite3_column_int64(st, 0));
    }
    sqlite3_finalize(st);
    return ret;
}

std::vector<StoredMessage> Database::get_messages_after_ts(uint64_t ts, int limit) {
    std::vector<StoredMessage> out;
    if (!db_) return out;
    std::string sql = std::string(kSelectMessagesPrefix) +
        "WHERE ts > ? ORDER BY ts ASC LIMIT ?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_int64(st, 1, ts);
    sqlite3_bind_int(st, 2, limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        out.push_back(read_message_row(st));
    }
    sqlite3_finalize(st);
    return out;
}


uint64_t Database::max_location_ts() {
    if (!db_) return 0;
    const char* sql = "SELECT MAX(ts) FROM location_history";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
    uint64_t ret = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        ret = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return ret;
}

std::vector<Database::LocationRow> Database::get_locations_after(uint64_t ts, int limit) {
    std::vector<LocationRow> out;
    if (!db_) return out;
    const char* sql = "SELECT device,node_num,latitude,longitude,altitude,ts FROM location_history WHERE ts > ? ORDER BY ts ASC LIMIT ?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_int64(st, 1, ts);
    sqlite3_bind_int(st, 2, limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        LocationRow row;
        if (auto* p = sqlite3_column_text(st, 0)) row.device = reinterpret_cast<const char*>(p);
        row.node_num = static_cast<uint32_t>(sqlite3_column_int64(st, 1));
        row.latitude = sqlite3_column_double(st, 2);
        row.longitude = sqlite3_column_double(st, 3);
        row.altitude = sqlite3_column_int(st, 4);
        row.ts = static_cast<uint64_t>(sqlite3_column_int64(st, 5));
        out.push_back(std::move(row));
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<Database::LocationRow> Database::get_node_locations(uint32_t node_num, uint64_t since_ts, int limit) {
    std::vector<LocationRow> out;
    if (!db_) return out;
    const char* sql = "SELECT device,node_num,latitude,longitude,altitude,ts FROM location_history "
                      "WHERE node_num = ? AND ts >= ? ORDER BY ts ASC LIMIT ?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_int64(st, 1, node_num);
    sqlite3_bind_int64(st, 2, since_ts);
    sqlite3_bind_int(st, 3, limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        LocationRow row;
        if (auto* p = sqlite3_column_text(st, 0)) row.device = reinterpret_cast<const char*>(p);
        row.node_num = static_cast<uint32_t>(sqlite3_column_int64(st, 1));
        row.latitude = sqlite3_column_double(st, 2);
        row.longitude = sqlite3_column_double(st, 3);
        row.altitude = sqlite3_column_int(st, 4);
        row.ts = static_cast<uint64_t>(sqlite3_column_int64(st, 5));
        out.push_back(std::move(row));
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<Database::LocationRow> Database::get_recent_node_locations(uint64_t since_ts, int limit) {
    std::vector<LocationRow> out;
    if (!db_) return out;
    const char* sql = "SELECT device,node_num,latitude,longitude,altitude,ts FROM location_history "
                      "WHERE ts >= ? ORDER BY ts ASC LIMIT ?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_int64(st, 1, since_ts);
    sqlite3_bind_int(st, 2, limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        LocationRow row;
        if (auto* p = sqlite3_column_text(st, 0)) row.device = reinterpret_cast<const char*>(p);
        row.node_num = static_cast<uint32_t>(sqlite3_column_int64(st, 1));
        row.latitude = sqlite3_column_double(st, 2);
        row.longitude = sqlite3_column_double(st, 3);
        row.altitude = sqlite3_column_int(st, 4);
        row.ts = static_cast<uint64_t>(sqlite3_column_int64(st, 5));
        out.push_back(std::move(row));
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<StoredMessage> Database::get_messages_paginated(const WindowKey& w, int limit, int offset) {
    std::vector<StoredMessage> out;
    if (!db_) return out;
    std::string sql = std::string(kSelectMessagesPrefix) +
        "WHERE (device=? OR ?='' OR device='' OR device LIKE '%' || ? OR ? LIKE '%' || device) "
        "AND window_kind=? AND window_target=? "
        "ORDER BY ts DESC LIMIT ? OFFSET ?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_text(st, 1, w.device.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, w.device.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, w.device.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, w.device.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, w.kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 6, w.target);
    sqlite3_bind_int(st, 7, limit);
    sqlite3_bind_int(st, 8, offset);
    while (sqlite3_step(st) == SQLITE_ROW) {
        out.push_back(read_message_row(st));
    }
    sqlite3_finalize(st);
    std::reverse(out.begin(), out.end());
    return out;
}

void Database::maybe_checkpoint() {
    if (++write_count_ >= 100) {
        write_count_ = 0;
        checkpoint();
    }
}

} // namespace meshcli
