// ==============================================================================
// keva/common/logger.hpp
//
// Purpose:
//   Declares the Keva structured logging interface. The logger writes
//   timestamped, level-tagged lines to stdout (for Docker/container log
//   aggregation) and optionally to a log file.
//
//   Design decisions:
//   - Four log levels: DEBUG < INFO < WARN < ERROR. Filtered at runtime by
//     the --loglevel CLI flag passed to keva-server.
//   - Format: "[ISO-8601-timestamp] [LEVEL] message"
//     Example: "[2026-09-15T18:00:00Z] [INFO] Keva server listening on 0.0.0.0:6379"
//   - The logger is a global singleton (thread-safe writes via a lightweight
//     spinlock) because log calls must be callable from any subsystem without
//     injecting a Logger dependency everywhere.
//   - Uses printf-style variadic formatting via snprintf for zero heap
//     allocation in the hot path (no std::format or fmtlib required).
// ==============================================================================

#pragma once

#include <string_view>
#include <cstdarg>

namespace keva {

// --------------------------------------------------------------------------
// LogLevel — runtime-configurable verbosity threshold
// --------------------------------------------------------------------------
enum class LogLevel : int {
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3,
};

// --------------------------------------------------------------------------
// Logger — global singleton structured logger
// --------------------------------------------------------------------------
class Logger {
public:
    // Retrieve the global logger singleton instance
    static Logger& instance();

    // Set the minimum log level at runtime (e.g., from --loglevel CLI flag)
    void set_level(LogLevel level) noexcept;
    [[nodiscard]] LogLevel level() const noexcept;

    // --------------------------------------------------------------------------
    // Core logging methods (printf-style format strings)
    // --------------------------------------------------------------------------
    void debug(const char* fmt, ...) const;
    void info (const char* fmt, ...) const;
    void warn (const char* fmt, ...) const;
    void error(const char* fmt, ...) const;

private:
    Logger() = default;

    // Internal write: formats the message, prepends timestamp + level tag,
    // and writes atomically to stdout
    void write(LogLevel level, const char* fmt, std::va_list args) const;

    LogLevel level_ = LogLevel::Info;
};

// --------------------------------------------------------------------------
// Convenience module-level free functions (delegates to Logger::instance())
// --------------------------------------------------------------------------
namespace log {
    void debug(const char* fmt, ...);
    void info (const char* fmt, ...);
    void warn (const char* fmt, ...);
    void error(const char* fmt, ...);
}

} // namespace keva
