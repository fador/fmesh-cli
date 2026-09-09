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
    relay_node    INTEGER,
    reply_id      INTEGER,
    emoji         INTEGER
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
CREATE TABLE IF NOT EXISTS telemetry_history(
    rowid               INTEGER PRIMARY KEY AUTOINCREMENT,
    device              TEXT NOT NULL,
    node_num            INTEGER NOT NULL,
    ts                  INTEGER NOT NULL,
    battery_level       INTEGER,
    voltage             REAL,
    channel_util        REAL,
    air_util_tx         REAL,
    uptime_seconds      INTEGER,
    temperature         REAL,
    relative_humidity   REAL,
    barometric_pressure REAL,
    gas_resistance      REAL,
    iaq                 INTEGER,
    pm25                INTEGER,
    co2                 INTEGER,
    current             REAL,
    snr                 REAL,
    hops_away           INTEGER
);
CREATE INDEX IF NOT EXISTS idx_telemetry_device_node_ts
    ON telemetry_history(device, node_num, ts);
CREATE INDEX IF NOT EXISTS idx_telemetry_node_ts
    ON telemetry_history(node_num, ts);
CREATE INDEX IF NOT EXISTS idx_telemetry_ts
    ON telemetry_history(ts);
CREATE TABLE IF NOT EXISTS device_config(
    device    TEXT NOT NULL,
    section   TEXT NOT NULL,
    key       TEXT NOT NULL,
    value     TEXT NOT NULL,
    PRIMARY KEY (device, section, key)
);
CREATE INDEX IF NOT EXISTS idx_device_config_device
    ON device_config(device);
)SQL";

static std::string normalize_db_device(const std::string& raw) {
    if (raw.empty()) return {};
    auto pos = raw.rfind("/dev_");
    if (pos != std::string::npos) {
        std::string s = raw.substr(pos + 5);
        std::string mac;
        for (char c : s) {
            if (c == '_') mac += ':';
            else mac += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return mac;
    }
    if (raw.rfind("dev_", 0) == 0 && raw.size() == 21) {
        std::string s = raw.substr(4);
        std::string mac;
        for (char c : s) {
            if (c == '_') mac += ':';
            else mac += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return mac;
    }
    bool is_mac = true;
    int colons = 0;
    std::string norm_mac;
    for (char c : raw) {
        if (c == ':' || c == '-' || c == '_') {
            norm_mac += ':';
            colons++;
        } else if (std::isxdigit(static_cast<unsigned char>(c))) {
            norm_mac += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        } else {
            is_mac = false;
            break;
        }
    }
    if (is_mac && colons == 5 && norm_mac.size() == 17) {
        return norm_mac;
    }
    return raw;
}

const char* kSelectMessagesPrefix =
    "SELECT rowid,device,window_kind,window_target,direction,from_node,to_node,"
    "channel_idx,text,ts,packet_id,ack_state,rx_snr,rx_rssi,hop_start,hop_limit,relay_node,"
    "reply_id,emoji "
    "FROM messages ";

const char* kSelectTelemetryPrefix =
    "SELECT rowid,device,node_num,ts,battery_level,voltage,channel_util,air_util_tx,uptime_seconds,"
    "temperature,relative_humidity,barometric_pressure,gas_resistance,iaq,pm25,co2,current,snr,hops_away "
    "FROM telemetry_history ";

StoredMessage read_message_row(sqlite3_stmt* st) {
    StoredMessage m;
    m.rowid = sqlite3_column_int64(st, 0);
    if (auto* p = sqlite3_column_text(st, 1)) m.device = reinterpret_cast<const char*>(p);
    m.window_kind = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
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
    m.reply_id = static_cast<uint32_t>(sqlite3_column_int64(st, 17));
    m.emoji = static_cast<uint32_t>(sqlite3_column_int64(st, 18));
    return m;
}

Database::TelemetryRow read_telemetry_row(sqlite3_stmt* st) {
    Database::TelemetryRow r;
    r.rowid = sqlite3_column_int64(st, 0);
    if (auto* p = sqlite3_column_text(st, 1)) r.device = reinterpret_cast<const char*>(p);
    r.node_num = static_cast<uint32_t>(sqlite3_column_int64(st, 2));
    r.ts = static_cast<uint64_t>(sqlite3_column_int64(st, 3));
    if (sqlite3_column_type(st, 4) != SQLITE_NULL) r.battery_level = static_cast<uint8_t>(sqlite3_column_int(st, 4));
    if (sqlite3_column_type(st, 5) != SQLITE_NULL) r.voltage = static_cast<float>(sqlite3_column_double(st, 5));
    if (sqlite3_column_type(st, 6) != SQLITE_NULL) r.channel_util = static_cast<float>(sqlite3_column_double(st, 6));
    if (sqlite3_column_type(st, 7) != SQLITE_NULL) r.air_util_tx = static_cast<float>(sqlite3_column_double(st, 7));
    if (sqlite3_column_type(st, 8) != SQLITE_NULL) r.uptime_seconds = static_cast<uint32_t>(sqlite3_column_int64(st, 8));
    if (sqlite3_column_type(st, 9) != SQLITE_NULL) r.temperature = static_cast<float>(sqlite3_column_double(st, 9));
    if (sqlite3_column_type(st, 10) != SQLITE_NULL) r.relative_humidity = static_cast<float>(sqlite3_column_double(st, 10));
    if (sqlite3_column_type(st, 11) != SQLITE_NULL) r.barometric_pressure = static_cast<float>(sqlite3_column_double(st, 11));
    if (sqlite3_column_type(st, 12) != SQLITE_NULL) r.gas_resistance = static_cast<float>(sqlite3_column_double(st, 12));
    if (sqlite3_column_type(st, 13) != SQLITE_NULL) r.iaq = static_cast<uint32_t>(sqlite3_column_int64(st, 13));
    if (sqlite3_column_type(st, 14) != SQLITE_NULL) r.pm25 = static_cast<uint32_t>(sqlite3_column_int64(st, 14));
    if (sqlite3_column_type(st, 15) != SQLITE_NULL) r.co2 = static_cast<uint32_t>(sqlite3_column_int64(st, 15));
    if (sqlite3_column_type(st, 16) != SQLITE_NULL) r.current = static_cast<float>(sqlite3_column_double(st, 16));
    if (sqlite3_column_type(st, 17) != SQLITE_NULL) r.snr = static_cast<float>(sqlite3_column_double(st, 17));
    if (sqlite3_column_type(st, 18) != SQLITE_NULL) r.hops_away = static_cast<uint32_t>(sqlite3_column_int64(st, 18));
    return r;
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
    sqlite3_exec(db_, "ALTER TABLE messages ADD COLUMN reply_id INTEGER;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE messages ADD COLUMN emoji INTEGER;", nullptr, nullptr, nullptr);

    // Migration: normalize historical device identifiers (e.g. /org/bluez/.../dev_XX_XX... or dev_XX_XX...)
    const std::vector<std::string> simple_tables = {
        "messages", "location_history", "telemetry_history", "packet_log"
    };
    for (const auto& tbl : simple_tables) {
        std::string q = "SELECT DISTINCT device FROM " + tbl + " WHERE device LIKE '%dev_%';";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, q.c_str(), -1, &st, nullptr) == SQLITE_OK) {
            std::vector<std::pair<std::string, std::string>> renames;
            while (sqlite3_step(st) == SQLITE_ROW) {
                if (auto* p = sqlite3_column_text(st, 0)) {
                    std::string orig = reinterpret_cast<const char*>(p);
                    std::string norm = normalize_db_device(orig);
                    if (!norm.empty() && norm != orig) {
                        renames.push_back({orig, norm});
                    }
                }
            }
            sqlite3_finalize(st);
            for (const auto& [orig, norm] : renames) {
                std::string upd = "UPDATE " + tbl + " SET device = ? WHERE device = ?;";
                sqlite3_stmt* ust = nullptr;
                if (sqlite3_prepare_v2(db_, upd.c_str(), -1, &ust, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(ust, 1, norm.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(ust, 2, orig.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_step(ust);
                    sqlite3_finalize(ust);
                }
            }
        }
    }
    const std::vector<std::string> pk_tables = {
        "channels", "nodes", "device_config"
    };
    for (const auto& tbl : pk_tables) {
        std::string q = "SELECT DISTINCT device FROM " + tbl + " WHERE device LIKE '%dev_%';";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, q.c_str(), -1, &st, nullptr) == SQLITE_OK) {
            std::vector<std::pair<std::string, std::string>> renames;
            while (sqlite3_step(st) == SQLITE_ROW) {
                if (auto* p = sqlite3_column_text(st, 0)) {
                    std::string orig = reinterpret_cast<const char*>(p);
                    std::string norm = normalize_db_device(orig);
                    if (!norm.empty() && norm != orig) {
                        renames.push_back({orig, norm});
                    }
                }
            }
            sqlite3_finalize(st);
            for (const auto& [orig, norm] : renames) {
                std::string upd = "UPDATE OR IGNORE " + tbl + " SET device = ? WHERE device = ?;";
                sqlite3_stmt* ust = nullptr;
                if (sqlite3_prepare_v2(db_, upd.c_str(), -1, &ust, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(ust, 1, norm.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(ust, 2, orig.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_step(ust);
                    sqlite3_finalize(ust);
                }
                std::string del = "DELETE FROM " + tbl + " WHERE device = ?;";
                if (sqlite3_prepare_v2(db_, del.c_str(), -1, &ust, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(ust, 1, orig.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_step(ust);
                    sqlite3_finalize(ust);
                }
            }
        }
    }
    // Migration: ensure telemetry_history exists
    sqlite3_exec(db_, "CREATE TABLE IF NOT EXISTS telemetry_history("
                       "rowid INTEGER PRIMARY KEY AUTOINCREMENT,"
                       "device TEXT NOT NULL,"
                       "node_num INTEGER NOT NULL,"
                       "ts INTEGER NOT NULL,"
                       "battery_level INTEGER,"
                       "voltage REAL,"
                       "channel_util REAL,"
                       "air_util_tx REAL,"
                       "uptime_seconds INTEGER,"
                       "temperature REAL,"
                       "relative_humidity REAL,"
                       "barometric_pressure REAL,"
                       "gas_resistance REAL,"
                       "iaq INTEGER,"
                       "pm25 INTEGER,"
                       "co2 INTEGER,"
                       "current REAL,"
                       "snr REAL,"
                       "hops_away INTEGER);", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "CREATE INDEX IF NOT EXISTS idx_telemetry_device_node_ts ON telemetry_history(device, node_num, ts);", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "CREATE INDEX IF NOT EXISTS idx_telemetry_node_ts ON telemetry_history(node_num, ts);", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "CREATE INDEX IF NOT EXISTS idx_telemetry_ts ON telemetry_history(ts);", nullptr, nullptr, nullptr);

    // Migration: ensure packet_log exists
    sqlite3_exec(db_, "CREATE TABLE IF NOT EXISTS packet_log("
                       "rowid INTEGER PRIMARY KEY AUTOINCREMENT,"
                       "device TEXT NOT NULL,"
                       "from_node INTEGER NOT NULL,"
                       "to_node INTEGER NOT NULL,"
                       "port_name TEXT NOT NULL,"
                       "channel_idx INTEGER DEFAULT 0,"
                       "rx_snr REAL,"
                       "rx_rssi INTEGER,"
                       "hop_limit INTEGER,"
                       "hop_start INTEGER,"
                       "broadcast INTEGER DEFAULT 0,"
                       "summary TEXT,"
                       "ts INTEGER NOT NULL);", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "CREATE INDEX IF NOT EXISTS idx_packet_log_ts ON packet_log(ts);", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "CREATE INDEX IF NOT EXISTS idx_packet_log_from_node_ts ON packet_log(from_node, ts);", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "CREATE INDEX IF NOT EXISTS idx_packet_log_port_ts ON packet_log(port_name, ts);", nullptr, nullptr, nullptr);

    // Initial backfill if packet_log is empty but history tables contain data
    sqlite3_stmt* check_st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM packet_log;", -1, &check_st, nullptr) == SQLITE_OK) {
        if (sqlite3_step(check_st) == SQLITE_ROW && sqlite3_column_int64(check_st, 0) == 0) {
            sqlite3_exec(db_, "INSERT INTO packet_log(device, from_node, to_node, port_name, channel_idx, rx_snr, rx_rssi, hop_start, hop_limit, broadcast, summary, ts) "
                              "SELECT device, from_node, to_node, 'TEXT_MESSAGE_APP', channel_idx, rx_snr, rx_rssi, hop_start, hop_limit, "
                              "CASE WHEN to_node = 4294967295 THEN 1 ELSE 0 END, text, ts FROM messages WHERE from_node IS NOT NULL AND from_node != 0;", nullptr, nullptr, nullptr);
            sqlite3_exec(db_, "INSERT INTO packet_log(device, from_node, to_node, port_name, channel_idx, rx_snr, rx_rssi, hop_start, hop_limit, broadcast, summary, ts) "
                              "SELECT device, node_num, 4294967295, 'TELEMETRY_APP', 0, snr, 0, 0, 0, 1, 'Telemetry Update', ts FROM telemetry_history WHERE node_num != 0;", nullptr, nullptr, nullptr);
            sqlite3_exec(db_, "INSERT INTO packet_log(device, from_node, to_node, port_name, channel_idx, rx_snr, rx_rssi, hop_start, hop_limit, broadcast, summary, ts) "
                              "SELECT device, node_num, 4294967295, 'POSITION_APP', 0, 0, 0, 0, 0, 1, 'GPS Position', ts FROM location_history WHERE node_num != 0;", nullptr, nullptr, nullptr);
        }
        sqlite3_finalize(check_st);
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
        if (auto* p = sqlite3_column_text(st, 0)) {
            std::string d = reinterpret_cast<const char*>(p);
            if (!d.empty()) out.push_back(std::move(d));
        }
    }
    sqlite3_finalize(st);
    return out;
}

std::optional<Node> Database::get_node_any_device(uint32_t node_num) {
    if (!db_) return std::nullopt;
    const char* sql = "SELECT node_num,node_id,long_name,short_name,hw_model,role,"
                      "battery,voltage,snr,hops_away,last_heard,"
                      "temperature,relative_humidity,barometric_pressure,"
                      "channel_util,air_util_tx,uptime_seconds FROM nodes "
                      "WHERE node_num=? AND (long_name != '' OR short_name != '') "
                      "ORDER BY last_heard DESC LIMIT 1";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_int64(st, 1, node_num);
    std::optional<Node> result;
    if (sqlite3_step(st) == SQLITE_ROW) {
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
        result = std::move(n);
    }
    sqlite3_finalize(st);
    return result;
}

void Database::upsert_device_config(const std::string& device, const std::string& section,
                                   const std::string& key, const std::string& value) {
    if (!db_ || device.empty() || key.empty()) return;
    const char* sql = "INSERT INTO device_config(device, section, key, value) "
                      "VALUES(?, ?, ?, ?) "
                      "ON CONFLICT(device, section, key) DO UPDATE SET value=excluded.value";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, device.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, section.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    maybe_checkpoint();
}

std::vector<Database::DeviceConfigItem> Database::load_device_config(const std::string& device) {
    std::vector<DeviceConfigItem> out;
    if (!db_ || device.empty()) return out;
    const char* sql = "SELECT section, key, value FROM device_config WHERE device=? ORDER BY section, key";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_text(st, 1, device.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        DeviceConfigItem item;
        if (auto* p = sqlite3_column_text(st, 0)) item.section = reinterpret_cast<const char*>(p);
        if (auto* p = sqlite3_column_text(st, 1)) item.key = reinterpret_cast<const char*>(p);
        if (auto* p = sqlite3_column_text(st, 2)) item.value = reinterpret_cast<const char*>(p);
        out.push_back(std::move(item));
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<WindowKey> Database::get_all_windows(const std::string& device) {
    std::vector<WindowKey> out;
    if (!db_) return out;
    std::string norm_d = normalize_db_device(device);
    std::string under_d = norm_d;
    std::replace(under_d.begin(), under_d.end(), ':', '_');

    const char* sql = "SELECT DISTINCT window_kind, window_target FROM messages "
                      "WHERE (device=? OR ?='' OR device='' OR device=? OR device=? "
                      "OR device LIKE '%' || ? || '%' OR device LIKE '%' || ? || '%' OR ? LIKE '%' || device)";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_text(st, 1, device.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, device.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, norm_d.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, under_d.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, norm_d.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 6, under_d.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 7, device.c_str(), -1, SQLITE_TRANSIENT);
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
        "channel_idx,text,ts,packet_id,ack_state,rx_snr,rx_rssi,hop_start,hop_limit,relay_node,reply_id,emoji) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
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
    sqlite3_bind_int64(st, 17, m.reply_id);
    sqlite3_bind_int64(st, 18, m.emoji);
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
    std::string norm_d = normalize_db_device(w.device);
    std::string under_d = norm_d;
    std::replace(under_d.begin(), under_d.end(), ':', '_');

    std::string sql = std::string(kSelectMessagesPrefix) +
        "WHERE (device=? OR ?='' OR device='' OR device=? OR device=? "
        "OR device LIKE '%' || ? || '%' OR device LIKE '%' || ? || '%' OR ? LIKE '%' || device) "
        "AND window_kind=? AND window_target=? "
        "ORDER BY ts DESC LIMIT ?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_text(st, 1, w.device.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, w.device.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, norm_d.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, under_d.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, norm_d.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 6, under_d.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 7, w.device.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 8, w.kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 9, w.target);
    sqlite3_bind_int(st, 10, limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        out.push_back(read_message_row(st));
    }
    sqlite3_finalize(st);
    std::reverse(out.begin(), out.end());
    if (out.empty() && !w.device.empty()) {
        WindowKey any_dev = w;
        any_dev.device = "";
        return recent_messages(any_dev, limit);
    }
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
    std::string norm_d = normalize_db_device(w.device);
    std::string under_d = norm_d;
    std::replace(under_d.begin(), under_d.end(), ':', '_');

    std::string sql = std::string(kSelectMessagesPrefix) +
        "WHERE (device=? OR ?='' OR device='' OR device=? OR device=? "
        "OR device LIKE '%' || ? || '%' OR device LIKE '%' || ? || '%' OR ? LIKE '%' || device) "
        "AND window_kind=? AND window_target=? "
        "ORDER BY ts DESC LIMIT ? OFFSET ?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_text(st, 1, w.device.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, w.device.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, norm_d.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, under_d.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, norm_d.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 6, under_d.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 7, w.device.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 8, w.kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 9, w.target);
    sqlite3_bind_int(st, 10, limit);
    sqlite3_bind_int(st, 11, offset);
    while (sqlite3_step(st) == SQLITE_ROW) {
        out.push_back(read_message_row(st));
    }
    sqlite3_finalize(st);
    std::reverse(out.begin(), out.end());
    if (out.empty() && !w.device.empty()) {
        WindowKey any_dev = w;
        any_dev.device = "";
        return get_messages_paginated(any_dev, limit, offset);
    }
    return out;
}

// --- telemetry history ------------------------------------------------------

bool Database::insert_telemetry(const TelemetryRow& r) {
    if (!db_) return false;

    // Check if a row already exists for (device, node_num, ts)
    const char* check_sql = "SELECT rowid, battery_level, voltage, channel_util, air_util_tx, uptime_seconds, "
                            "temperature, relative_humidity, barometric_pressure, gas_resistance, iaq, pm25, co2, current, snr, hops_away "
                            "FROM telemetry_history WHERE device=? AND node_num=? AND ts=?";
    sqlite3_stmt* check_st = nullptr;
    int64_t existing_rowid = 0;
    TelemetryRow merged = r;
    if (sqlite3_prepare_v2(db_, check_sql, -1, &check_st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(check_st, 1, r.device.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(check_st, 2, r.node_num);
        sqlite3_bind_int64(check_st, 3, r.ts);
        if (sqlite3_step(check_st) == SQLITE_ROW) {
            existing_rowid = sqlite3_column_int64(check_st, 0);
            if (!merged.battery_level.has_value() && sqlite3_column_type(check_st, 1) != SQLITE_NULL)
                merged.battery_level = static_cast<uint8_t>(sqlite3_column_int(check_st, 1));
            if (!merged.voltage.has_value() && sqlite3_column_type(check_st, 2) != SQLITE_NULL)
                merged.voltage = static_cast<float>(sqlite3_column_double(check_st, 2));
            if (!merged.channel_util.has_value() && sqlite3_column_type(check_st, 3) != SQLITE_NULL)
                merged.channel_util = static_cast<float>(sqlite3_column_double(check_st, 3));
            if (!merged.air_util_tx.has_value() && sqlite3_column_type(check_st, 4) != SQLITE_NULL)
                merged.air_util_tx = static_cast<float>(sqlite3_column_double(check_st, 4));
            if (!merged.uptime_seconds.has_value() && sqlite3_column_type(check_st, 5) != SQLITE_NULL)
                merged.uptime_seconds = static_cast<uint32_t>(sqlite3_column_int64(check_st, 5));
            if (!merged.temperature.has_value() && sqlite3_column_type(check_st, 6) != SQLITE_NULL)
                merged.temperature = static_cast<float>(sqlite3_column_double(check_st, 6));
            if (!merged.relative_humidity.has_value() && sqlite3_column_type(check_st, 7) != SQLITE_NULL)
                merged.relative_humidity = static_cast<float>(sqlite3_column_double(check_st, 7));
            if (!merged.barometric_pressure.has_value() && sqlite3_column_type(check_st, 8) != SQLITE_NULL)
                merged.barometric_pressure = static_cast<float>(sqlite3_column_double(check_st, 8));
            if (!merged.gas_resistance.has_value() && sqlite3_column_type(check_st, 9) != SQLITE_NULL)
                merged.gas_resistance = static_cast<float>(sqlite3_column_double(check_st, 9));
            if (!merged.iaq.has_value() && sqlite3_column_type(check_st, 10) != SQLITE_NULL)
                merged.iaq = static_cast<uint32_t>(sqlite3_column_int64(check_st, 10));
            if (!merged.pm25.has_value() && sqlite3_column_type(check_st, 11) != SQLITE_NULL)
                merged.pm25 = static_cast<uint32_t>(sqlite3_column_int64(check_st, 11));
            if (!merged.co2.has_value() && sqlite3_column_type(check_st, 12) != SQLITE_NULL)
                merged.co2 = static_cast<uint32_t>(sqlite3_column_int64(check_st, 12));
            if (!merged.current.has_value() && sqlite3_column_type(check_st, 13) != SQLITE_NULL)
                merged.current = static_cast<float>(sqlite3_column_double(check_st, 13));
            if (!merged.snr.has_value() && sqlite3_column_type(check_st, 14) != SQLITE_NULL)
                merged.snr = static_cast<float>(sqlite3_column_double(check_st, 14));
            if (!merged.hops_away.has_value() && sqlite3_column_type(check_st, 15) != SQLITE_NULL)
                merged.hops_away = static_cast<uint32_t>(sqlite3_column_int64(check_st, 15));
        }
        sqlite3_finalize(check_st);
    }

    if (existing_rowid > 0) {
        const char* sql = "UPDATE telemetry_history SET "
                          "battery_level=?, voltage=?, channel_util=?, air_util_tx=?, uptime_seconds=?, "
                          "temperature=?, relative_humidity=?, barometric_pressure=?, gas_resistance=?, "
                          "iaq=?, pm25=?, co2=?, current=?, snr=?, hops_away=? WHERE rowid=?";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
        int idx = 1;
        if (merged.battery_level) sqlite3_bind_int(st, idx++, *merged.battery_level); else sqlite3_bind_null(st, idx++);
        if (merged.voltage) sqlite3_bind_double(st, idx++, *merged.voltage); else sqlite3_bind_null(st, idx++);
        if (merged.channel_util) sqlite3_bind_double(st, idx++, *merged.channel_util); else sqlite3_bind_null(st, idx++);
        if (merged.air_util_tx) sqlite3_bind_double(st, idx++, *merged.air_util_tx); else sqlite3_bind_null(st, idx++);
        if (merged.uptime_seconds) sqlite3_bind_int64(st, idx++, *merged.uptime_seconds); else sqlite3_bind_null(st, idx++);
        if (merged.temperature) sqlite3_bind_double(st, idx++, *merged.temperature); else sqlite3_bind_null(st, idx++);
        if (merged.relative_humidity) sqlite3_bind_double(st, idx++, *merged.relative_humidity); else sqlite3_bind_null(st, idx++);
        if (merged.barometric_pressure) sqlite3_bind_double(st, idx++, *merged.barometric_pressure); else sqlite3_bind_null(st, idx++);
        if (merged.gas_resistance) sqlite3_bind_double(st, idx++, *merged.gas_resistance); else sqlite3_bind_null(st, idx++);
        if (merged.iaq) sqlite3_bind_int64(st, idx++, *merged.iaq); else sqlite3_bind_null(st, idx++);
        if (merged.pm25) sqlite3_bind_int64(st, idx++, *merged.pm25); else sqlite3_bind_null(st, idx++);
        if (merged.co2) sqlite3_bind_int64(st, idx++, *merged.co2); else sqlite3_bind_null(st, idx++);
        if (merged.current) sqlite3_bind_double(st, idx++, *merged.current); else sqlite3_bind_null(st, idx++);
        if (merged.snr) sqlite3_bind_double(st, idx++, *merged.snr); else sqlite3_bind_null(st, idx++);
        if (merged.hops_away) sqlite3_bind_int64(st, idx++, *merged.hops_away); else sqlite3_bind_null(st, idx++);
        sqlite3_bind_int64(st, idx++, existing_rowid);
        sqlite3_step(st);
        sqlite3_finalize(st);
        maybe_checkpoint();
        return true;
    }

    const char* sql = "INSERT INTO telemetry_history(device,node_num,ts,battery_level,voltage,channel_util,"
                      "air_util_tx,uptime_seconds,temperature,relative_humidity,barometric_pressure,"
                      "gas_resistance,iaq,pm25,co2,current,snr,hops_away) "
                      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, merged.device.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, merged.node_num);
    sqlite3_bind_int64(st, 3, merged.ts);
    int idx = 4;
    if (merged.battery_level) sqlite3_bind_int(st, idx++, *merged.battery_level); else sqlite3_bind_null(st, idx++);
    if (merged.voltage) sqlite3_bind_double(st, idx++, *merged.voltage); else sqlite3_bind_null(st, idx++);
    if (merged.channel_util) sqlite3_bind_double(st, idx++, *merged.channel_util); else sqlite3_bind_null(st, idx++);
    if (merged.air_util_tx) sqlite3_bind_double(st, idx++, *merged.air_util_tx); else sqlite3_bind_null(st, idx++);
    if (merged.uptime_seconds) sqlite3_bind_int64(st, idx++, *merged.uptime_seconds); else sqlite3_bind_null(st, idx++);
    if (merged.temperature) sqlite3_bind_double(st, idx++, *merged.temperature); else sqlite3_bind_null(st, idx++);
    if (merged.relative_humidity) sqlite3_bind_double(st, idx++, *merged.relative_humidity); else sqlite3_bind_null(st, idx++);
    if (merged.barometric_pressure) sqlite3_bind_double(st, idx++, *merged.barometric_pressure); else sqlite3_bind_null(st, idx++);
    if (merged.gas_resistance) sqlite3_bind_double(st, idx++, *merged.gas_resistance); else sqlite3_bind_null(st, idx++);
    if (merged.iaq) sqlite3_bind_int64(st, idx++, *merged.iaq); else sqlite3_bind_null(st, idx++);
    if (merged.pm25) sqlite3_bind_int64(st, idx++, *merged.pm25); else sqlite3_bind_null(st, idx++);
    if (merged.co2) sqlite3_bind_int64(st, idx++, *merged.co2); else sqlite3_bind_null(st, idx++);
    if (merged.current) sqlite3_bind_double(st, idx++, *merged.current); else sqlite3_bind_null(st, idx++);
    if (merged.snr) sqlite3_bind_double(st, idx++, *merged.snr); else sqlite3_bind_null(st, idx++);
    if (merged.hops_away) sqlite3_bind_int64(st, idx++, *merged.hops_away); else sqlite3_bind_null(st, idx++);
    sqlite3_step(st);
    sqlite3_finalize(st);
    maybe_checkpoint();
    return true;
}

uint64_t Database::max_telemetry_ts() {
    if (!db_) return 0;
    const char* sql = "SELECT MAX(ts) FROM telemetry_history";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
    uint64_t ret = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        ret = static_cast<uint64_t>(sqlite3_column_int64(st, 0));
    }
    sqlite3_finalize(st);
    return ret;
}

std::vector<Database::TelemetryRow> Database::get_telemetry_after_ts(uint64_t ts, int limit) {
    std::vector<TelemetryRow> out;
    if (!db_) return out;
    std::string sql = std::string(kSelectTelemetryPrefix) + "WHERE ts > ? ORDER BY ts ASC LIMIT ?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_int64(st, 1, ts);
    sqlite3_bind_int(st, 2, limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        out.push_back(read_telemetry_row(st));
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<Database::TelemetryRow> Database::get_node_telemetry(uint32_t node_num, uint64_t since_ts, uint64_t before_ts, int limit) {
    std::vector<TelemetryRow> out;
    if (!db_) return out;
    std::string sql = std::string(kSelectTelemetryPrefix) +
                      "WHERE node_num = ? AND ts >= ? " +
                      (before_ts > 0 ? "AND ts <= ? " : "") +
                      "ORDER BY ts ASC LIMIT ?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return out;
    int idx = 1;
    sqlite3_bind_int64(st, idx++, node_num);
    sqlite3_bind_int64(st, idx++, since_ts);
    if (before_ts > 0) sqlite3_bind_int64(st, idx++, before_ts);
    sqlite3_bind_int(st, idx++, limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        out.push_back(read_telemetry_row(st));
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<Database::TelemetryRow> Database::get_recent_telemetry(uint64_t since_ts, int limit) {
    std::vector<TelemetryRow> out;
    if (!db_) return out;
    std::string sql = std::string(kSelectTelemetryPrefix) +
                      "WHERE ts >= ? ORDER BY ts ASC LIMIT ?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_int64(st, 1, since_ts);
    sqlite3_bind_int(st, 2, limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        out.push_back(read_telemetry_row(st));
    }
    sqlite3_finalize(st);
    return out;
}

bool Database::insert_packet_log(const PacketLogRow& row) {
    if (!db_) return false;
    const char* sql = "INSERT INTO packet_log(device, from_node, to_node, port_name, channel_idx, rx_snr, rx_rssi, hop_limit, hop_start, broadcast, summary, ts) "
                      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?);";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, row.device.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, row.from_node);
    sqlite3_bind_int64(st, 3, row.to_node);
    sqlite3_bind_text(st, 4, row.port_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, row.channel_idx);
    sqlite3_bind_double(st, 6, row.rx_snr);
    sqlite3_bind_int(st, 7, row.rx_rssi);
    sqlite3_bind_int64(st, 8, row.hop_limit);
    sqlite3_bind_int64(st, 9, row.hop_start);
    sqlite3_bind_int(st, 10, row.broadcast ? 1 : 0);
    sqlite3_bind_text(st, 11, row.summary.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 12, row.ts);

    bool ok = (sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    if (ok) maybe_checkpoint();
    return ok;
}

Database::GlobalStats Database::get_stats(uint64_t since_ts, uint32_t for_node) {
    GlobalStats res;
    if (!db_) return res;

    // 1. Overall aggregations
    std::string q1 = "SELECT COUNT(*), COUNT(DISTINCT from_node), "
                     "SUM(CASE WHEN broadcast != 0 THEN 1 ELSE 0 END), "
                     "SUM(CASE WHEN broadcast = 0 THEN 1 ELSE 0 END), "
                     "AVG(CASE WHEN rx_snr != 0 THEN rx_snr ELSE NULL END) "
                     "FROM packet_log WHERE ts >= ? ";
    if (for_node != 0) q1 += "AND from_node = ? ";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, q1.c_str(), -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, since_ts);
        if (for_node != 0) sqlite3_bind_int64(st, 2, for_node);
        if (sqlite3_step(st) == SQLITE_ROW) {
            res.total_packets = static_cast<uint64_t>(sqlite3_column_int64(st, 0));
            res.active_nodes = static_cast<uint64_t>(sqlite3_column_int64(st, 1));
            res.broadcast_count = static_cast<uint64_t>(sqlite3_column_int64(st, 2));
            res.unicast_count = static_cast<uint64_t>(sqlite3_column_int64(st, 3));
            if (sqlite3_column_type(st, 4) != SQLITE_NULL) {
                res.avg_snr = static_cast<float>(sqlite3_column_double(st, 4));
            }
        }
        sqlite3_finalize(st);
    }

    // 2. Port distribution
    std::string q2 = "SELECT port_name, COUNT(*) FROM packet_log WHERE ts >= ? ";
    if (for_node != 0) q2 += "AND from_node = ? ";
    q2 += "GROUP BY port_name ORDER BY COUNT(*) DESC;";

    if (sqlite3_prepare_v2(db_, q2.c_str(), -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, since_ts);
        if (for_node != 0) sqlite3_bind_int64(st, 2, for_node);
        while (sqlite3_step(st) == SQLITE_ROW) {
            std::string port = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
            uint64_t cnt = static_cast<uint64_t>(sqlite3_column_int64(st, 1));
            res.port_distribution[port] = cnt;
            if (port == "TEXT_MESSAGE_APP") res.msg_count = cnt;
            else if (port == "TELEMETRY_APP") res.telemetry_count = cnt;
            else if (port == "POSITION_APP") res.position_count = cnt;
            else if (port == "ROUTING_APP") res.ack_count = cnt;
            else if (port == "TRACEROUTE_APP") res.traceroute_count = cnt;
            else if (port == "NODEINFO_APP") res.nodeinfo_count = cnt;
            else res.other_count += cnt;
        }
        sqlite3_finalize(st);
    }

    // 3. Channel distribution
    std::string q3 = "SELECT channel_idx, COUNT(*) FROM packet_log WHERE ts >= ? ";
    if (for_node != 0) q3 += "AND from_node = ? ";
    q3 += "GROUP BY channel_idx ORDER BY channel_idx ASC;";

    if (sqlite3_prepare_v2(db_, q3.c_str(), -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, since_ts);
        if (for_node != 0) sqlite3_bind_int64(st, 2, for_node);
        while (sqlite3_step(st) == SQLITE_ROW) {
            uint32_t ch = static_cast<uint32_t>(sqlite3_column_int64(st, 0));
            uint64_t cnt = static_cast<uint64_t>(sqlite3_column_int64(st, 1));
            res.channel_distribution[ch] = cnt;
        }
        sqlite3_finalize(st);
    }

    // 4. Timeline buckets
    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    uint64_t duration = (since_ts > 0 && now > since_ts) ? (now - since_ts) : 864000;
    uint64_t bucket_size = 3600;
    if (duration <= 3600) bucket_size = 120;           // 1h -> 2 min buckets
    else if (duration <= 21600) bucket_size = 600;     // 6h -> 10 min buckets
    else if (duration <= 86400) bucket_size = 1800;    // 1d -> 30 min buckets
    else if (duration <= 604800) bucket_size = 7200;   // 7d -> 2 hour buckets
    else bucket_size = 86400;                          // all time -> 1 day buckets

    std::string q4 = "SELECT (ts / ?) * ? AS b_ts, COUNT(*), "
                     "SUM(CASE WHEN port_name = 'TEXT_MESSAGE_APP' THEN 1 ELSE 0 END), "
                     "SUM(CASE WHEN port_name = 'TELEMETRY_APP' THEN 1 ELSE 0 END), "
                     "SUM(CASE WHEN port_name = 'POSITION_APP' THEN 1 ELSE 0 END), "
                     "SUM(CASE WHEN port_name NOT IN ('TEXT_MESSAGE_APP','TELEMETRY_APP','POSITION_APP') THEN 1 ELSE 0 END) "
                     "FROM packet_log WHERE ts >= ? ";
    if (for_node != 0) q4 += "AND from_node = ? ";
    q4 += "GROUP BY b_ts ORDER BY b_ts ASC;";

    if (sqlite3_prepare_v2(db_, q4.c_str(), -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, bucket_size);
        sqlite3_bind_int64(st, 2, bucket_size);
        sqlite3_bind_int64(st, 3, since_ts);
        if (for_node != 0) sqlite3_bind_int64(st, 4, for_node);
        while (sqlite3_step(st) == SQLITE_ROW) {
            GlobalStats::TimelineBucket b;
            b.ts = static_cast<uint64_t>(sqlite3_column_int64(st, 0));
            b.count = static_cast<uint64_t>(sqlite3_column_int64(st, 1));
            b.msg_count = static_cast<uint64_t>(sqlite3_column_int64(st, 2));
            b.telemetry_count = static_cast<uint64_t>(sqlite3_column_int64(st, 3));
            b.pos_count = static_cast<uint64_t>(sqlite3_column_int64(st, 4));
            b.other_count = static_cast<uint64_t>(sqlite3_column_int64(st, 5));
            res.timeline.push_back(b);
        }
        sqlite3_finalize(st);
    }

    // 5. Per-node statistics breakdown
    std::string q5 = "SELECT from_node, COUNT(*), "
                     "SUM(CASE WHEN port_name = 'TEXT_MESSAGE_APP' THEN 1 ELSE 0 END), "
                     "SUM(CASE WHEN port_name = 'TELEMETRY_APP' THEN 1 ELSE 0 END), "
                     "SUM(CASE WHEN port_name = 'POSITION_APP' THEN 1 ELSE 0 END), "
                     "SUM(CASE WHEN port_name = 'ROUTING_APP' THEN 1 ELSE 0 END), "
                     "AVG(CASE WHEN rx_snr != 0 THEN rx_snr ELSE NULL END), "
                     "MAX(ts) "
                     "FROM packet_log WHERE ts >= ? ";
    if (for_node != 0) q5 += "AND from_node = ? ";
    q5 += "GROUP BY from_node ORDER BY COUNT(*) DESC;";

    if (sqlite3_prepare_v2(db_, q5.c_str(), -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, since_ts);
        if (for_node != 0) sqlite3_bind_int64(st, 2, for_node);
        while (sqlite3_step(st) == SQLITE_ROW) {
            GlobalStats::NodeStat ns;
            ns.node_num = static_cast<uint32_t>(sqlite3_column_int64(st, 0));
            ns.packet_count = static_cast<uint64_t>(sqlite3_column_int64(st, 1));
            ns.msg_count = static_cast<uint64_t>(sqlite3_column_int64(st, 2));
            ns.telemetry_count = static_cast<uint64_t>(sqlite3_column_int64(st, 3));
            ns.pos_count = static_cast<uint64_t>(sqlite3_column_int64(st, 4));
            ns.ack_count = static_cast<uint64_t>(sqlite3_column_int64(st, 5));
            if (sqlite3_column_type(st, 6) != SQLITE_NULL) {
                ns.avg_snr = static_cast<float>(sqlite3_column_double(st, 6));
            }
            ns.last_seen = static_cast<uint64_t>(sqlite3_column_int64(st, 7));
            res.per_node_stats.push_back(ns);
        }
        sqlite3_finalize(st);
    }

    return res;
}

void Database::maybe_checkpoint() {
    if (++write_count_ >= 100) {
        write_count_ = 0;
        checkpoint();
    }
}

} // namespace meshcli
