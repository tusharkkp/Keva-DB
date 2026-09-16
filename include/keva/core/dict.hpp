// ==============================================================================
// keva/core/dict.hpp
//
// Purpose:
//   Declares keva::core::Dict — the custom, dual-table progressive incremental
//   hash table that forms the primary key-space index of the Keva database.
//   This is the most important data structure in the entire Keva codebase,
//   directly inspired by Redis's legendary 'dict.c'.
//
//   Why a custom hash table instead of std::unordered_map?
//   std::unordered_map uses node-based separate chaining (each entry is a
//   separately heap-allocated node). When the load factor threshold is exceeded,
//   std::unordered_map rehashes ALL entries in a single call — this can cause
//   a multi-hundred-millisecond "stop-the-world" freeze for large tables.
//   For a database serving 100,000+ QPS, this is catastrophic.
//
//   Progressive Incremental Rehashing:
//   Instead of rehashing all N keys at once, our Dict maintains TWO hash tables:
//   - ht[0]: the "live" table (handles all reads and writes during rehash)
//   - ht[1]: the "new" larger table being built
//
//   When the load factor of ht[0] exceeds DICT_EXPAND_RATIO (1.0), we:
//   1. Allocate ht[1] at twice the capacity.
//   2. Set rehashidx = 0 (the current bucket being migrated).
//   3. Every operation (GET, SET, DEL) migrates N buckets from ht[0] to ht[1].
//   4. The 10Hz server cron also migrates buckets proactively.
//   5. When ht[0] is empty (all buckets migrated), swap: ht[0] = ht[1], reset.
//
//   This amortizes the rehash cost across thousands of requests with zero
//   user-visible latency spikes.
//
//   Hash function: SipHash-1-2 (a fast, cryptographically secure PRF)
//   Using a predictable hash function (like a simple modulo or FNV) allows
//   adversarial clients to deliberately craft keys that all hash to the same
//   bucket, creating O(N) lookup chains (HashDoS attack). SipHash is keyed
//   by a random 128-bit secret generated at server startup, making such
//   attacks computationally infeasible.
// ==============================================================================

#pragma once

#include "keva/common/types.hpp"
#include "keva/core/buffer.hpp"

#include <functional>
#include <string_view>
#include <optional>

namespace keva::core {

// Forward declaration: DictEntry is the linked list node in each bucket
struct DictEntry;

// --------------------------------------------------------------------------
// DictHashTable — one of the two hash tables inside Dict
// --------------------------------------------------------------------------
struct DictHashTable {
    DictEntry** buckets = nullptr;  // Pointer to array of bucket-head pointers
    usize       size     = 0;       // Number of buckets (always a power of two)
    usize       sizemask = 0;       // size - 1 (for fast modulo: hash & sizemask)
    usize       used     = 0;       // Number of entries currently stored

    void allocate(usize n_buckets);   // Allocate bucket array (zero-initialized)
    void deallocate();                // Free bucket array
    [[nodiscard]] bool empty() const noexcept { return used == 0; }
    [[nodiscard]] bool null()  const noexcept { return buckets == nullptr; }
};

// --------------------------------------------------------------------------
// DictEntry — a singly-linked list node within a hash bucket
// --------------------------------------------------------------------------
struct DictEntry {
    Buffer      key;        // Binary-safe key (our custom Buffer type)
    void*       value;      // Pointer to a KevaObject (or any void* payload)
    u64         hash_val;   // Cached hash (avoids rehashing during migration)
    DictEntry*  next;       // Next entry in the same bucket's chain

    explicit DictEntry(std::string_view k, void* v, u64 h)
        : key(k), value(v), hash_val(h), next(nullptr) {}
};

// --------------------------------------------------------------------------
// Dict — dual-table progressive incremental hash table
// --------------------------------------------------------------------------
class Dict {
public:
    // 'value_deleter' is called on the value pointer when an entry is removed.
    // This decouples memory management of values from the Dict itself.
    using ValueDeleter = std::function<void(void*)>;

    explicit Dict(ValueDeleter deleter = nullptr);
    ~Dict();

    // Non-copyable (owns heap-allocated bucket arrays and entries)
    Dict(const Dict&) = delete;
    Dict& operator=(const Dict&) = delete;

    // --------------------------------------------------------------------------
    // Core Operations
    // --------------------------------------------------------------------------

    // Insert or overwrite a key. Returns true if a new entry was created,
    // false if an existing entry was updated.
    bool set(std::string_view key, void* value);

    // Look up a key. Returns the value pointer, or nullptr if not found.
    [[nodiscard]] void* get(std::string_view key);
    [[nodiscard]] const void* get(std::string_view key) const;

    // Remove a key. Returns true if the key existed and was deleted.
    bool del(std::string_view key);

    // Returns true if the key exists
    [[nodiscard]] bool exists(std::string_view key) const;

    // --------------------------------------------------------------------------
    // Rehashing
    // --------------------------------------------------------------------------

    // Returns true if a rehash is currently in progress
    [[nodiscard]] bool is_rehashing() const noexcept { return rehashidx_ >= 0; }

    // Migrate up to 'steps' buckets from ht[0] to ht[1].
    // Called per-command and from the server cron.
    // Returns true if rehashing is complete.
    bool rehash_step(int steps = 1);

    // --------------------------------------------------------------------------
    // Statistics
    // --------------------------------------------------------------------------
    [[nodiscard]] usize size() const noexcept { return ht_[0].used + ht_[1].used; }
    [[nodiscard]] bool  empty() const noexcept { return size() == 0; }

    // Remove all entries (invokes value_deleter_ on each), reset to initial state
    void clear();

    // Load factor of the primary table (used / buckets)
    [[nodiscard]] float load_factor() const noexcept;

    // --------------------------------------------------------------------------
    // Iteration (for BGSAVE, KEYS *, DBSIZE, active expiry scan)
    // --------------------------------------------------------------------------
    // Visit every live entry. 'visitor' receives (key_view, value_ptr).
    // Visiting during an active rehash iterates both ht[0] and ht[1].
    void for_each(const std::function<void(std::string_view, void*)>& visitor) const;

    // Sample up to 'count' random entries (for approximated LRU eviction).
    // Populates 'out' with value pointers. Returns the actual number sampled.
    usize random_sample(usize count, std::vector<DictEntry*>& out);

    // --------------------------------------------------------------------------
    // Hash function (SipHash-1-2, keyed per-instance)
    // --------------------------------------------------------------------------
    [[nodiscard]] u64 hash(std::string_view key) const noexcept;

    // Initialize the SipHash secret key (called once at server start)
    void seed_hash(u64 k0, u64 k1) noexcept;

private:
    // Try to expand ht[0] if load factor exceeds DICT_EXPAND_RATIO
    void maybe_expand();

    // Try to shrink ht[0] if load factor drops below DICT_SHRINK_RATIO
    void maybe_shrink();

    // Find the DictEntry* for a key in ht[i] (or nullptr if not found)
    DictEntry* find_in(int ht_idx, std::string_view key, u64 hash_val) const;

    // Returns the bucket index for a given hash in ht[i]
    usize bucket_idx(int ht_idx, u64 hash_val) const noexcept {
        return hash_val & ht_[ht_idx].sizemask;
    }

    DictHashTable ht_[2];       // ht_[0] = live table, ht_[1] = rehash target
    i64           rehashidx_;   // -1 = not rehashing; >= 0 = next bucket to migrate

    ValueDeleter  value_deleter_;

    // SipHash secret key components (randomized at startup)
    u64 hash_key_[2] = {0, 0};
};

} // namespace keva::core
