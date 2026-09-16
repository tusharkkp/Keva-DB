// ==============================================================================
// src/core/server_context.cpp
//
// Purpose:
//   Implements the ServerContext singleton: initialization, database access,
//   and the server_cron() periodic maintenance function.
//
//   server_cron() is called by the EventLoop timer at 10Hz (every 100ms).
//   Each tick it performs:
//   1. Active expiry: calls active_expire_cycle() on each database.
//      Deletes probabilistically-sampled expired keys without scanning everything.
//   2. Incremental rehash: calls rehash_step() on each database so that
//      hash table resizes are spread across cron ticks rather than spiking
//      in a single command execution.
//   3. RDB save point check: if dirty_keys has exceeded a save point threshold
//      AND enough time has elapsed since the last save, trigger BGSAVE.
// ==============================================================================

#include "keva/core/server_context.hpp"
#include "keva/common/logger.hpp"

#include <ctime>

namespace keva::core {

// --------------------------------------------------------------------------
// Singleton access
// --------------------------------------------------------------------------
ServerContext& ServerContext::instance() {
    static ServerContext ctx;
    return ctx;
}

// --------------------------------------------------------------------------
// init — called once at server startup
// --------------------------------------------------------------------------
void ServerContext::init(ServerConfig config) {
    config_           = std::move(config);
    start_time_seconds = static_cast<i64>(::time(nullptr));
    last_save_time.store(start_time_seconds);

    // Create one database for Phase 1 (Redis supports 16 DBs by default)
    databases_.clear();
    databases_.push_back(std::make_unique<KevaDatabase>(0));

    log::info("ServerContext initialized: port=%d, hz=%d, rdb=%s",
              config_.port, config_.hz, config_.rdb_filename.c_str());
}

// --------------------------------------------------------------------------
// db — access database by index
// --------------------------------------------------------------------------
KevaDatabase& ServerContext::db(int db_idx) {
    // Phase 1: only db 0 is supported
    return *databases_[0];
}

// --------------------------------------------------------------------------
// server_cron — periodic maintenance at hz frequency
// --------------------------------------------------------------------------
void ServerContext::server_cron() {
    // 1. Active expiry scan on all databases
    for (auto& database : databases_) {
        const usize deleted = database->active_expire_cycle();
        if (deleted > 0) {
            stats_.expired_keys.fetch_add(deleted, std::memory_order_relaxed);
            log::debug("server_cron: expired %zu keys from db %d",
                       deleted, database->db_id());
        }
    }

    // 2. Incremental hash table rehash on all databases
    for (auto& database : databases_) {
        database->rehash_step();
    }

    // 3. RDB save point check
    if (config_.rdb_enabled && bgsave_child_pid == -1) {
        const i64 now = static_cast<i64>(::time(nullptr));
        const u64 dirty = dirty_keys.load(std::memory_order_relaxed);
        const i64 last  = last_save_time.load(std::memory_order_relaxed);

        for (const auto& sp : config_.save_points) {
            if (dirty >= static_cast<u64>(sp.changes) &&
                (now - last) >= sp.seconds) {
                log::info("server_cron: save point triggered (%llu changes in %llds)",
                          static_cast<unsigned long long>(dirty),
                          static_cast<long long>(now - last));
                // Actual BGSAVE is triggered by the command handler in Phase 5
                // Here we just log the trigger condition.
                break;
            }
        }
    }
}

} // namespace keva::core
