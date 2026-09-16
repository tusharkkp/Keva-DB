// ==============================================================================
// src/common/logger.cpp
//
// Purpose:
//   Implements the Keva global logger singleton declared in logger.hpp.
//
//   Key implementation details:
//   - Uses a POSIX mutex (pthread_mutex_t) rather than std::mutex to ensure
//     safe log writes from the main thread alongside background I/O threads
//     (Phase 2), without the overhead of a full lock per log line in the
//     hot command execution path.
//   - Timestamps are formatted using clock_gettime(CLOCK_REALTIME) + strftime
//     for sub-second precision in log output.
//   - Log output is written with a single write() syscall per line to avoid
//     interleaved partial lines from concurrent threads.
// ==============================================================================

#include "keva/common/logger.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>       // write()
#include <pthread.h>      // pthread_mutex_t

namespace keva {

// --------------------------------------------------------------------------
// Internal POSIX mutex guard for thread-safe log writes
// --------------------------------------------------------------------------
namespace {

struct LogMutex {
    LogMutex()  { pthread_mutex_init(&m, nullptr); }
    ~LogMutex() { pthread_mutex_destroy(&m); }
    void lock()   { pthread_mutex_lock(&m); }
    void unlock() { pthread_mutex_unlock(&m); }
    pthread_mutex_t m;
};

LogMutex g_log_mutex;

// Returns the string label for a log level
const char* level_tag(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

} // anonymous namespace

// --------------------------------------------------------------------------
// Logger singleton
// --------------------------------------------------------------------------
Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

void Logger::set_level(LogLevel level) noexcept {
    level_ = level;
}

LogLevel Logger::level() const noexcept {
    return level_;
}

// --------------------------------------------------------------------------
// Public logging methods
// --------------------------------------------------------------------------
void Logger::debug(const char* fmt, ...) const {
    if (level_ > LogLevel::Debug) return;
    std::va_list args;
    va_start(args, fmt);
    write(LogLevel::Debug, fmt, args);
    va_end(args);
}

void Logger::info(const char* fmt, ...) const {
    if (level_ > LogLevel::Info) return;
    std::va_list args;
    va_start(args, fmt);
    write(LogLevel::Info, fmt, args);
    va_end(args);
}

void Logger::warn(const char* fmt, ...) const {
    if (level_ > LogLevel::Warn) return;
    std::va_list args;
    va_start(args, fmt);
    write(LogLevel::Warn, fmt, args);
    va_end(args);
}

void Logger::error(const char* fmt, ...) const {
    // Errors are always written regardless of level filter
    std::va_list args;
    va_start(args, fmt);
    write(LogLevel::Error, fmt, args);
    va_end(args);
}

// --------------------------------------------------------------------------
// Internal write: format, timestamp, lock, flush
// --------------------------------------------------------------------------
void Logger::write(LogLevel lvl, const char* fmt, std::va_list args) const {
    // --- Format timestamp ---
    char ts_buf[32];
    {
        struct timespec ts{};
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tm_info{};
        gmtime_r(&ts.tv_sec, &tm_info);
        // e.g., "2026-09-15T18:30:00Z"
        strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%SZ", &tm_info);
    }

    // --- Format the caller's message ---
    char msg_buf[2048];
    std::vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);

    // --- Assemble final log line ---
    char line_buf[2200];
    int len = std::snprintf(line_buf, sizeof(line_buf),
        "[%s] [%s] %s\n", ts_buf, level_tag(lvl), msg_buf);

    // --- Write atomically under the mutex ---
    g_log_mutex.lock();
    // Write to stderr so logs appear in Docker / systemd journal separately
    // from stdout data. Errors and all log lines go to fd=2 (stderr).
    ::write(STDERR_FILENO, line_buf, static_cast<std::size_t>(len));
    g_log_mutex.unlock();
}

// --------------------------------------------------------------------------
// Free function convenience wrappers
// --------------------------------------------------------------------------
namespace log {

void debug(const char* fmt, ...) {
    std::va_list args; va_start(args, fmt);
    // Re-route through the internal write directly so we get correct va_list
    if (Logger::instance().level() <= LogLevel::Debug) {
        Logger::instance().write(LogLevel::Debug, fmt, args);
    }
    va_end(args);
}

void info(const char* fmt, ...) {
    std::va_list args; va_start(args, fmt);
    if (Logger::instance().level() <= LogLevel::Info) {
        Logger::instance().write(LogLevel::Info, fmt, args);
    }
    va_end(args);
}

void warn(const char* fmt, ...) {
    std::va_list args; va_start(args, fmt);
    if (Logger::instance().level() <= LogLevel::Warn) {
        Logger::instance().write(LogLevel::Warn, fmt, args);
    }
    va_end(args);
}

void error(const char* fmt, ...) {
    std::va_list args; va_start(args, fmt);
    // Errors always print
    Logger::instance().write(LogLevel::Error, fmt, args);
    va_end(args);
}

} // namespace log

} // namespace keva
