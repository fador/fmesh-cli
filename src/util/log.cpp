#include "log.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sys/stat.h>
#include <sys/types.h>

namespace fs = std::filesystem;
using namespace meshcli;

namespace {

std::mutex g_log_mutex;

std::string default_log_path() {
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    std::string dir = std::string(home) + "/.local/share/mesh-cli";
    ::mkdir(dir.c_str(), 0755);
    return dir + "/mesh-cli.log";
}

const char* level_str(LogLevel l) {
    switch (l) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "?";
}

} // namespace

namespace meshcli {

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

void Logger::init(std::string path, bool console, LogLevel level) {
    if (path.empty()) path = default_log_path();
    path_ = std::move(path);
    console_ = console;
    level_ = level;
    // Touch the file so we know it's writable.
    if (std::ofstream f(path_, std::ios::app); f) {
        f << "---- mesh-cli log ----\n";
    }
}

void Logger::write(LogLevel level, std::string_view msg) {
    if (!enabled(level)) return;
    std::lock_guard<std::mutex> lock(g_log_mutex);

    using namespace std::chrono;
    auto now = system_clock::now();
    auto t = system_clock::to_time_t(now);
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm{};
    ::localtime_r(&t, &tm);

    char ts[32];
    std::snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d.%03lld",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<long long>(ms.count()));

    if (!path_.empty()) {
        if (std::ofstream f(path_, std::ios::app); f) {
            f << ts << " [" << level_str(level) << "] " << msg << '\n';
            f.flush();
        }
    }
    if (console_) {
        std::fprintf(stderr, "%s [%s] %.*s\n", ts, level_str(level),
                     static_cast<int>(msg.size()), msg.data());
    }
}

LogStream::LogStream(LogLevel level, const char* file, int line)
    : level_(level) {
    // Trim file to basename.
    const char* base = file;
    for (const char* p = file; *p; ++p) {
        if (*p == '/') base = p + 1;
    }
    buf_ << "[" << base << ":" << line << "] ";
}

LogStream::~LogStream() {
    Logger::instance().write(level_, buf_.str());
}

} // namespace meshcli
