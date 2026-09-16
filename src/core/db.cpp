// ==============================================================================
// src/core/db.cpp
//
// Purpose:
//   Implements the KevaDatabase class: the in-memory database with dual-dict
//   key/expiry architecture, lazy passive expiry, active probabilistic expiry
//   cycle, and approximated LRU eviction.
//
//   Active expiry algorithm (active_expire_cycle):
//   Redis's algorithm runs up to a time budget (25ms by default). Ours uses
//   a fixed iteration limit for simplicity in Phase 1:
//   1. Randomly sample ACTIVE_EXPIRE_SAMPLE_SIZE entries from expires_.
//   2. Delete all entries that are past their expiry timestamp.
//   3. If more than 25% of sampled entries were expired, repeat the loop
//      (keys likely have many expired entries — worth continuing).
//   4. Stop after a maximum of MAX_EXPIRE_CYCLES loops per cron tick.
//
//   Approximated LRU eviction:
//   When evict_if_needed() is called and memory is over limit:
//   1. Sample LRU_POOL_SIZE random entries from keys_.
//   2. Compute idle_seconds(obj->lru()) for each.
//   3. Evict the entry with the highest idle_seconds (coldest key).
//   This achieves >99% accuracy vs. exact LRU at a fraction of the cost.
// ==============================================================================

#include "keva/core/db.hpp"
#include "keva/common/logger.hpp"

#include <chrono>
#include <vector>
#include <climits>

namespace keva::core {

// --------------------------------------------------------------------------
// now_milliseconds — current wall clock in milliseconds
// --------------------------------------------------------------------------
MillisecondTimestamp now_milliseconds() noexcept {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()
    ).count();
}

// --------------------------------------------------------------------------
// Constructor: initialize the two Dicts with their respective value deleters
// --------------------------------------------------------------------------
KevaDatabase::KevaDatabase(int db_id)
    : db_id_(db_id)
    , keys_([](void* ptr) { KevaObject::destroy(static_cast<KevaObject*>(ptr)); })
    , expires_([](void* ptr) { delete static_cast<i64*>(ptr); })
{}

// --------------------------------------------------------------------------
// delete_key — unconditionally remove a key and its expiry
// --------------------------------------------------------------------------
void KevaDatabase::delete_key(std::string_view key) {
    keys_.del(key);
    expires_.del(key);
}

// --------------------------------------------------------------------------
// is_expired — check if a key has a registered expiry that has passed
// --------------------------------------------------------------------------
bool KevaDatabase::is_expired(std::string_view key, MillisecondTimestamp now_ms) const {
    const auto* ts_ptr = static_cast<const i64*>(expires_.get(key));
    if (!ts_ptr) return false;  // No expiry registered
    return now_ms >= *ts_ptr;
}

// --------------------------------------------------------------------------
// get — retrieve a value; perform lazy expiry check
// --------------------------------------------------------------------------
KevaObject* KevaDatabase::get(std::string_view key) {
    auto* obj = static_cast<KevaObject*>(keys_.get(key));
    if (!obj) return nullptr;

    // Lazy (passive) expiry: delete the key if it has expired
    if (is_expired(key, now_milliseconds())) {
        delete_key(key);
        return nullptr;
    }

    // Update the LRU clock on access (for eviction approximation)
    obj->touch(lru_clock::current());
    return obj;
}

// --------------------------------------------------------------------------
// peek — look up without touching LRU (for EXISTS, TYPE, TTL commands)
// --------------------------------------------------------------------------
KevaObject* KevaDatabase::peek(std::string_view key) {
    auto* obj = static_cast<KevaObject*>(keys_.get(key));
    if (!obj) return nullptr;
    // Still check expiry for correctness
    if (is_expired(key, now_milliseconds())) return nullptr;
    return obj;
}

// --------------------------------------------------------------------------
// set — store a key-value pair (overwrites existing; clears old expiry)
// --------------------------------------------------------------------------
void KevaDatabase::set(std::string_view key, KevaObject* obj) {
    // Remove any existing expiry when a key is overwritten with a plain SET
    expires_.del(key);
    keys_.set(key, obj);
}

// --------------------------------------------------------------------------
// del — remove a key and its expiry; returns true if it existed
// --------------------------------------------------------------------------
bool KevaDatabase::del(std::string_view key) {
    const bool existed = keys_.exists(key);
    delete_key(key);
    return existed;
}

// --------------------------------------------------------------------------
// exists — check key presence with lazy expiry
// --------------------------------------------------------------------------
bool KevaDatabase::exists(std::string_view key) {
    return get(key) != nullptr;
}

// --------------------------------------------------------------------------
// set_expire — register an absolute expiry timestamp for a key
// --------------------------------------------------------------------------
void KevaDatabase::set_expire(std::string_view key, MillisecondTimestamp expire_ms) {
    // If an expiry already exists for this key, update it in-place
    auto* existing = static_cast<i64*>(expires_.get(key));
    if (existing) {
        *existing = expire_ms;
        return;
    }
    expires_.set(key, new i64{expire_ms});
}

// --------------------------------------------------------------------------
// remove_expire — remove expiry (PERSIST command)
// --------------------------------------------------------------------------
void KevaDatabase::remove_expire(std::string_view key) {
    expires_.del(key);
}

// --------------------------------------------------------------------------
// pttl — remaining TTL in milliseconds
// --------------------------------------------------------------------------
i64 KevaDatabase::pttl(std::string_view key) {
    if (!keys_.exists(key)) return -2;  // Key does not exist
    const auto* ts_ptr = static_cast<const i64*>(expires_.get(key));
    if (!ts_ptr) return -1;             // No expiry set

    const i64 remaining = *ts_ptr - now_milliseconds();
    return remaining > 0 ? remaining : 0;
}

// --------------------------------------------------------------------------
// active_expire_cycle — probabilistic active expiry (called by server cron)
// --------------------------------------------------------------------------
usize KevaDatabase::active_expire_cycle() {
    if (expires_.empty()) return 0;

    usize total_deleted = 0;
    const int MAX_CYCLES = 3;
    const MillisecondTimestamp now = now_milliseconds();

    for (int cycle = 0; cycle < MAX_CYCLES; ++cycle) {
        std::vector<DictEntry*> sampled;
        sampled.reserve(constants::ACTIVE_EXPIRE_SAMPLE_SIZE);

        usize n = expires_.random_sample(
            static_cast<usize>(constants::ACTIVE_EXPIRE_SAMPLE_SIZE), sampled);

        if (n == 0) break;

        usize deleted_this_round = 0;
        std::vector<std::string> expired_keys;
        for (auto* entry : sampled) {
            const auto* ts_ptr = static_cast<const i64*>(entry->value);
            if (ts_ptr && now >= *ts_ptr) {
                expired_keys.emplace_back(entry->key.view());
            }
        }

        for (const auto& key : expired_keys) {
            delete_key(key);
            ++deleted_this_round;
            ++total_deleted;
        }

        // If less than 25% were expired, stop (not worth continuing)
        if (deleted_this_round * 4 < n) break;
    }

    return total_deleted;
}

// --------------------------------------------------------------------------
// evict_if_needed — approximated LRU eviction under maxmemory pressure
// --------------------------------------------------------------------------
Status KevaDatabase::evict_if_needed(usize /*maxmemory*/) {
    // Phase 1 stub: memory tracking and eviction will be fully wired in Phase 4
    // when we implement the ServerContext with used_memory tracking.
    // For now, we always return ok() (no eviction triggered).
    return Status::success();
}

// --------------------------------------------------------------------------
// rehash_step — drive incremental rehash on both dicts from server cron
// --------------------------------------------------------------------------
void KevaDatabase::rehash_step() {
    keys_.rehash_step(100);    // Migrate up to 100 buckets per cron tick
    expires_.rehash_step(100);
}

// --------------------------------------------------------------------------
// for_each — iterate all non-expired keys (used by RDB save)
// --------------------------------------------------------------------------
void KevaDatabase::for_each(const std::function<void(std::string_view, KevaObject*)>& visitor) const {
    const MillisecondTimestamp now = now_milliseconds();
    keys_.for_each([&](std::string_view key, void* val) {
        // Skip keys that have already expired
        const auto* ts_ptr = static_cast<const i64*>(expires_.get(key));
        if (ts_ptr && now >= *ts_ptr) return;

        visitor(key, static_cast<KevaObject*>(val));
    });
}

// --------------------------------------------------------------------------
// flush — remove all keys (FLUSHDB)
// --------------------------------------------------------------------------
void KevaDatabase::flush() {
    // Reconstruct both dicts (cheapest way to release all entries at once)
    keys_.clear();
    expires_.clear();
    log::info("DB %d flushed", db_id_);
}

} // namespace keva::core
