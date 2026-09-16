// ==============================================================================
// keva/core/server_context.hpp
//
// Purpose:
//   Declares ServerContext — the global singleton that holds all shared server
//   state accessible to every subsystem (command handlers, persistence engine,
//   event loop, expiry cron, and the future threaded I/O layer).
//
//   ServerContext aggregates:
//   - The KevaDatabase instance(s) (Phase 1: one DB; Phase 2: configurable)
//   - Server configuration parameters (port, host, maxmemory, hz, save points)
//   - Runtime statistics (total_commands_processed, connected_clients, etc.)
//   - The server start time (for UPTIME calculations in the INFO command)
//   - The SipHash secret keys (generated at startup; stored here for sharing)
//   - The RDB BGSAVE child PID (tracked for SIGCHLD handling)
//
//   Why a singleton?
//   Command handlers need access to the database but should not receive
//   the entire ServerContext via function parameters (too verbose). Making
//   ServerContext a globally accessible singleton with a static instance()
//   accessor is the pragmatic systems programming pattern used in Redis itself
//   (the global 'server' struct in Redis's server.h).
// ==============================================================================

#pragma once

#include "keva/core/db.hpp"
#include "keva/common/types.hpp"

#include <string>
#include <vector>
#include <atomic>
#include <memory>

namespace keva::core {

// --------------------------------------------------------------------------
// ServerConfig — configuration parameters (parsed from CLI args / config file)
// --------------------------------------------------------------------------
struct ServerConfig {
    std::string host          = "0.0.0.0";
    u16         port          = constants::DEFAULT_PORT;
    u32         max_clients   = constants::DEFAULT_MAX_CLIENTS;
    usize       maxmemory     = 0;        // 0 = unlimited
    int         hz            = constants::SERVER_HZ;  // Cron tick rate
    std::string loglevel      = "info";
    std::string rdb_filename  = "dump.rdb";
    bool        rdb_enabled   = true;

    // Save points: save if >= 'changes' keys changed in <= 'seconds'
    struct SavePoint {
        int seconds;
        int changes;
    };
    std::vector<SavePoint> save_points = {
        {3600, 1},    // Save after 1 hour if at least 1 key changed
        {300,  100},  // Save after 5 minutes if at least 100 keys changed
        {60,   10000} // Save after 1 minute if at least 10,000 keys changed
    };
};

// --------------------------------------------------------------------------
// ServerStats — runtime metrics (atomics for safe multi-threaded reads in INFO)
// --------------------------------------------------------------------------
struct ServerStats {
    std::atomic<u64> total_commands_processed{0};
    std::atomic<u64> total_connections_received{0};
    std::atomic<u64> keyspace_hits{0};
    std::atomic<u64> keyspace_misses{0};
    std::atomic<u64> expired_keys{0};
    std::atomic<u64> evicted_keys{0};
};

// --------------------------------------------------------------------------
// ServerContext — global server state singleton
// --------------------------------------------------------------------------
class ServerContext {
public:
    static ServerContext& instance();

    // Initialize the server context with configuration.
    // Must be called once at startup before any commands are processed.
    void init(ServerConfig config);

    // --------------------------------------------------------------------------
    // Database access
    // --------------------------------------------------------------------------
    [[nodiscard]] KevaDatabase& db(int db_idx = 0);

    // --------------------------------------------------------------------------
    // Configuration
    // --------------------------------------------------------------------------
    [[nodiscard]] const ServerConfig& config() const noexcept { return config_; }

    // --------------------------------------------------------------------------
    // Statistics
    // --------------------------------------------------------------------------
    [[nodiscard]] ServerStats& stats() noexcept { return stats_; }
    void record_command() noexcept { stats_.total_commands_processed.fetch_add(1, std::memory_order_relaxed); }
    void record_hit()     noexcept { stats_.keyspace_hits.fetch_add(1, std::memory_order_relaxed); }
    void record_miss()    noexcept { stats_.keyspace_misses.fetch_add(1, std::memory_order_relaxed); }

    // --------------------------------------------------------------------------
    // Server cron (called at hz frequency by the EventLoop timer)
    // --------------------------------------------------------------------------
    void server_cron();

    // --------------------------------------------------------------------------
    // BGSAVE child tracking (for SIGCHLD handler)
    // --------------------------------------------------------------------------
    pid_t bgsave_child_pid = -1;

    // Dirty key counter: incremented on every write; reset on BGSAVE
    std::atomic<u64> dirty_keys{0};

    // Wall clock of last successful BGSAVE
    std::atomic<i64> last_save_time{0};

    // --------------------------------------------------------------------------
    // Server start time
    // --------------------------------------------------------------------------
    i64 start_time_seconds = 0;

private:
    ServerContext() = default;

    ServerConfig config_;
    ServerStats  stats_;

    // The database instances (Phase 1: just db[0])
    std::vector<std::unique_ptr<KevaDatabase>> databases_;
};

} // namespace keva::core
