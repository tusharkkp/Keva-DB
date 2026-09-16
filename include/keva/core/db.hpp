// ==============================================================================
// keva/core/db.hpp
//
// Purpose:
//   Declares the KevaDatabaseobjected — the in-memory database instance that
//   holds all key-value pairs, their expiry timestamps, and the eviction engine.
//
//   Every key in Keva maps to a KevaObject stored via two parallel structures:
//   1. keys_   : Dict mapping key_string -> KevaObject*  (the primary key space)
//   2. expires_: Dict mapping key_string -> int64_t* (millisecond expiry timestamps)
//
//   Why a separate 'expires_' dictionary?
//   This is a critical Redis design decision: only keys WITH an expiry occupy
//   space in the expires_ dict. Keys created with plain SET (no TTL) pay zero
//   bytes of expiry overhead. This avoids a 64-bit timestamp field inside
//   every single key-value entry.
//
//   Expiry subsystem:
//   - Passive (lazy) expiration: every get() call checks if the key has
//     expired and deletes it on-the-fly before returning.
//   - Active (cron) expiration: the 10Hz server cron calls
//     active_expire_cycle() which randomly samples ACTIVE_EXPIRE_SAMPLE_SIZE
//     keys from the expires_ dict, deletes expired ones, and repeats if
//     more than 25% were expired (up to a CPU time budget).
//
//   Memory eviction:
//   - When the server's used_memory approaches maxmemory, evict_if_needed()
//     samples LRU_POOL_SIZE random keys from keys_, computes their idle time
//     using the 24-bit lru_clock in KevaObject, and evicts the coldest one.
// ==============================================================================

#pragma once

#include "keva/core/dict.hpp"
#include "keva/core/object.hpp"
#include "keva/common/types.hpp"
#include "keva/common/status.hpp"

#include <string_view>
#include <string>

namespace keva::core {

// --------------------------------------------------------------------------
// KevaDatabaseobjected — a single logical database (Keva supports db 0 by default)
// --------------------------------------------------------------------------
class KevaDatabase {
public:
    explicit KevaDatabase(int db_id = 0);
    ~KevaDatabase() = default;

    // --------------------------------------------------------------------------
    // Key-Value Operations
    // --------------------------------------------------------------------------

    // Retrieve a value by key. Performs lazy expiry check.
    // Returns nullptr if the key does not exist or has expired.
    [[nodiscard]] KevaObject* get(std::string_view key);

    // Look up a key without touching its LRU clock (for EXISTS, TYPE commands)
    [[nodiscard]] KevaObject* peek(std::string_view key) const;

    // Insert or replace a key with a new KevaObject.
    // If an existing key had an expiry, it is cleared.
    void set(std::string_view key, KevaObject* obj);

    // Remove a key (and its expiry entry if any). Returns true if it existed.
    bool del(std::string_view key);

    // Returns true if the key exists and has not expired
    [[nodiscard]] bool exists(std::string_view key);

    // Returns the number of keys in the database
    [[nodiscard]] usize dbsize() const noexcept { return keys_.size(); }

    // --------------------------------------------------------------------------
    // TTL / Expiry Operations
    // --------------------------------------------------------------------------

    // Set an absolute expiry timestamp in milliseconds for a key
    void set_expire(std::string_view key, MillisecondTimestamp expire_ms);

    // Remove the expiry of a key (PERSIST command)
    void remove_expire(std::string_view key);

    // Returns the remaining TTL in milliseconds, or -1 if no expiry, -2 if key missing
    [[nodiscard]] i64 pttl(std::string_view key);

    // Returns true if the key is expired at the given timestamp
    [[nodiscard]] bool is_expired(std::string_view key, MillisecondTimestamp now_ms) const;

    // --------------------------------------------------------------------------
    // Expiry & Eviction Engine
    // --------------------------------------------------------------------------

    // Called by the server cron (10Hz) to actively scan and delete expired keys.
    // Returns the number of keys deleted.
    usize active_expire_cycle();

    // Called before each SET to check if we need to evict a key to stay under maxmemory.
    // Evicts the approximated-LRU "coldest" key if needed.
    Status evict_if_needed(usize maxmemory);

    // Perform one incremental rehash step on both keys_ and expires_ dicts
    void rehash_step();

    // --------------------------------------------------------------------------
    // Iteration (for RDB persistence, KEYS *, DBSIZE)
    // --------------------------------------------------------------------------
    void for_each(const std::function<void(std::string_view, KevaObject*)>& visitor) const;

    // Flush all keys and expiry records (FLUSHDB)
    void flush();

    [[nodiscard]] int db_id() const noexcept { return db_id_; }

private:
    // Delete a key unconditionally (no locking; called from within the class)
    void delete_key(std::string_view key);

    int db_id_;

    Dict keys_;     // key_name -> KevaObject*
    Dict expires_;  // key_name -> int64_t* (millisecond expiry timestamp)
};

// --------------------------------------------------------------------------
// Current wall clock in milliseconds (used for expiry comparison)
// --------------------------------------------------------------------------
MillisecondTimestamp now_milliseconds() noexcept;

} // namespace keva::core
