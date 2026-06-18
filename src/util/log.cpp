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
#include <thread>

#ifdef _WIN32
#include <direct.h>
#endif

namespace fs = std::filesystem;
using namespace meshcli;

namespace {
std::mutex g_log_mutex;

std::string default_log_path() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) home = "/tmp";
    std::string dir = std::string(home) + "/.local/share/fmesh-cli";
    // mkdir is best-effort; logger init will also try create_directories.
#ifdef _WIN32
    ::_mkdir(dir.c_str());
#else
    ::mkdir(dir.c_str(), 0755);
#endif
    try {
        std::filesystem::create_directories(dir);
    } catch (...) {}
    return dir + "/fmesh-cli.log";
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
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (path.empty()) path = default_log_path();
    // Ensure the parent directory exists.
    try {
        fs::path p(path);
        auto parent = p.parent_path();
        if (!parent.empty() && !fs::exists(parent)) {
            std::error_code ec;
            fs::create_directories(parent, ec);
        }
    } catch (...) {}
    path_ = std::move(path);
    console_ = console;
    level_ = level;
    // Touch the file so we know it's writable.
    if (std::ofstream f(path_, std::ios::app); f) {
        f << "---- fmesh-cli log ----\n";
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
#ifdef _WIN32
    ::localtime_s(&tm, &t);
#else
    ::localtime_r(&t, &tm);
#endif

    char ts[48];
    std::snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<int>(ms.count()));

    if (!path_.empty()) {
        maybe_rotate();
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

void Logger::maybe_rotate() {
    if (path_.empty()) return;
    try {
        std::error_code ec;
        auto sz = std::filesystem::file_size(path_, ec);
        if (ec || sz < kMaxLogSize) return;
        // Rotate: rename current log to .1, remove old .1 if present.
        auto rotated = path_ + ".1";
        std::filesystem::remove(rotated, ec);
        std::filesystem::rename(path_, rotated, ec);
    } catch (...) {}
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
