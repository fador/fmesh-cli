#pragma once

#include <sstream>
#include <string>
#include <string_view>

namespace meshcli {

enum class LogLevel : int {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
};

// Minimal logger: writes to a rotating file + optional stderr (when TUI is not
// running). Thread-safe. The log path defaults to
// ~/.local/share/mesh-cli/mesh-cli.log but may be overridden via init().
class Logger {
public:
    static Logger& instance();

    // Initialise the global logger. If `path` is empty, a default is used.
    // If `console` is true, also duplicate to stderr (useful pre-TUI).
    void init(std::string path, bool console, LogLevel level = LogLevel::Info);

    void write(LogLevel level, std::string_view msg);
    void set_console(bool on) { console_ = on; }
    void set_level(LogLevel l) { level_ = l; }

    [[nodiscard]] bool enabled(LogLevel l) const { return static_cast<int>(l) >= static_cast<int>(level_); }

private:
    Logger() = default;
    std::string path_;
    bool console_ = true;
    LogLevel level_ = LogLevel::Info;
    // Rotate when log file exceeds this size (default 5 MB).
    static constexpr size_t kMaxLogSize = 5 * 1024 * 1024;
    void maybe_rotate();};

class LogStream {
public:
    LogStream(LogLevel level, const char* file, int line);
    ~LogStream();

    template <typename T>
    LogStream& operator<<(const T& v) {
        buf_ << v;
        return *this;
    }

private:
    LogLevel level_;
    std::ostringstream buf_;
};

#define MESHCLI_LOG(level) ::meshcli::LogStream(level, __FILE__, __LINE__)
#define LOG_TRACE() MESHCLI_LOG(::meshcli::LogLevel::Trace)
#define LOG_DEBUG() MESHCLI_LOG(::meshcli::LogLevel::Debug)
#define LOG_INFO()  MESHCLI_LOG(::meshcli::LogLevel::Info)
#define LOG_WARN()  MESHCLI_LOG(::meshcli::LogLevel::Warn)
#define LOG_ERROR() MESHCLI_LOG(::meshcli::LogLevel::Error)

} // namespace meshcli
