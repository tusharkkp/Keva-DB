// ==============================================================================
// src/core/dict.cpp
//
// Purpose:
//   Implements the Dict dual-table progressive incremental hash table.
//
//   SipHash-1-2 implementation:
//   SipHash (designed by Jean-Philippe Aumasson and Daniel J. Bernstein) is a
//   fast, keyed hash function resistant to hash-flooding denial-of-service
//   attacks. The "1-2" variant runs 1 compression round per message block and
//   2 finalization rounds — providing strong pseudorandomness with minimal
//   CPU cost. The 128-bit key (k0, k1) is generated from /dev/urandom at
//   server startup and stored per-Dict instance, making the hash outputs
//   unpredictable to external attackers even if they can observe hash outputs.
//
//   Progressive rehash algorithm (rehash_step):
//   1. Skip empty buckets in ht_[0] (they have nothing to migrate).
//      Track empty bucket skips with a limit to avoid O(N) scanning.
//   2. Take all entries in the current ht_[0].buckets[rehashidx_] chain.
//   3. Recompute each entry's bucket index in ht_[1] (using ht_[1].sizemask).
//   4. Prepend the entry to ht_[1].buckets[new_idx] chain.
//   5. Set ht_[0].buckets[rehashidx_] = nullptr, decrement ht_[0].used.
//   6. Increment rehashidx_.
//   7. When ht_[0].used == 0: swap tables, deallocate old ht_[0], reset.
//
//   Reads and writes during rehash:
//   - get():  searches ht_[0] first, then ht_[1] (key might be already migrated).
//   - set():  always inserts into ht_[1] (ensures newly inserted keys don't need migration).
//   - del():  searches and removes from both tables.
// ==============================================================================

#include "keva/core/dict.hpp"
#include "keva/common/logger.hpp"

#include <cstring>
#include <cstdlib>
#include <bit>
#include <random>
#include <vector>

namespace keva::core {

// --------------------------------------------------------------------------
// SipHash-1-2 implementation (self-contained, no external library)
// Reference: https://131002.net/siphash/
// --------------------------------------------------------------------------
namespace {

#define ROTL64(x, r) (((x) << (r)) | ((x) >> (64 - (r))))

#define SIPROUND(v0,v1,v2,v3)         \
    v0 += v1; v1 = ROTL64(v1,13); v1 ^= v0; v0 = ROTL64(v0,32); \
    v2 += v3; v3 = ROTL64(v3,16); v3 ^= v2;                     \
    v0 += v3; v3 = ROTL64(v3,21); v3 ^= v0;                     \
    v2 += v1; v1 = ROTL64(v1,17); v1 ^= v2; v2 = ROTL64(v2,32);

u64 siphash_1_2(const void* data, usize len, u64 k0, u64 k1) {
    u64 v0 = k0 ^ 0x736f6d6570736575ULL;
    u64 v1 = k1 ^ 0x646f72616e646f6dULL;
    u64 v2 = k0 ^ 0x6c7967656e657261ULL;
    u64 v3 = k1 ^ 0x7465646279746573ULL;

    const u8* in = static_cast<const u8*>(data);
    const u8* end = in + (len & ~7ULL);

    while (in != end) {
        u64 m;
        std::memcpy(&m, in, 8);
        v3 ^= m;
        SIPROUND(v0, v1, v2, v3);
        v0 ^= m;
        in += 8;
    }

    // Process remaining bytes
    usize remaining = len & 7;
    u64 last = static_cast<u64>(len & 0xFF) << 56;
    switch (remaining) {
        case 7: last |= static_cast<u64>(in[6]) << 48; [[fallthrough]];
        case 6: last |= static_cast<u64>(in[5]) << 40; [[fallthrough]];
        case 5: last |= static_cast<u64>(in[4]) << 32; [[fallthrough]];
        case 4: last |= static_cast<u64>(in[3]) << 24; [[fallthrough]];
        case 3: last |= static_cast<u64>(in[2]) << 16; [[fallthrough]];
        case 2: last |= static_cast<u64>(in[1]) << 8;  [[fallthrough]];
        case 1: last |= static_cast<u64>(in[0]);        [[fallthrough]];
        default: break;
    }

    v3 ^= last;
    SIPROUND(v0, v1, v2, v3);
    v0 ^= last;

    // Finalization (2 rounds)
    v2 ^= 0xFF;
    SIPROUND(v0, v1, v2, v3);
    SIPROUND(v0, v1, v2, v3);

    return v0 ^ v1 ^ v2 ^ v3;
}

#undef ROTL64
#undef SIPROUND

} // anonymous namespace

// --------------------------------------------------------------------------
// DictHashTable::allocate / deallocate
// --------------------------------------------------------------------------
void DictHashTable::allocate(usize n_buckets) {
    // n_buckets MUST be a power of two (caller ensures this)
    buckets  = new DictEntry*[n_buckets]();  // zero-initialized by ()
    size     = n_buckets;
    sizemask = n_buckets - 1;
    used     = 0;
}

void DictHashTable::deallocate() {
    delete[] buckets;
    buckets  = nullptr;
    size     = 0;
    sizemask = 0;
    used     = 0;
}

// --------------------------------------------------------------------------
// Dict constructor / destructor
// --------------------------------------------------------------------------
Dict::Dict(ValueDeleter deleter)
    : rehashidx_(-1)
    , value_deleter_(std::move(deleter))
{
    // Initialize with a small bucket array (4 buckets = 1 cache line)
    ht_[0].allocate(4);
    // ht_[1] starts null (only allocated when rehash begins)
}

Dict::~Dict() {
    // Free all entries in both tables
    for (int t = 0; t < 2; ++t) {
        if (ht_[t].null()) continue;
        for (usize i = 0; i < ht_[t].size; ++i) {
            DictEntry* entry = ht_[t].buckets[i];
            while (entry) {
                DictEntry* next = entry->next;
                if (value_deleter_ && entry->value) {
                    value_deleter_(entry->value);
                }
                delete entry;
                entry = next;
            }
        }
        ht_[t].deallocate();
    }
}

// --------------------------------------------------------------------------
// seed_hash — set the SipHash secret key
// --------------------------------------------------------------------------
void Dict::seed_hash(u64 k0, u64 k1) noexcept {
    hash_key_[0] = k0;
    hash_key_[1] = k1;
}

// --------------------------------------------------------------------------
// hash — compute SipHash of a key
// --------------------------------------------------------------------------
u64 Dict::hash(std::string_view key) const noexcept {
    return siphash_1_2(key.data(), key.size(), hash_key_[0], hash_key_[1]);
}

// --------------------------------------------------------------------------
// find_in — search one hash table for a key
// --------------------------------------------------------------------------
DictEntry* Dict::find_in(int ht_idx, std::string_view key, u64 hash_val) const {
    if (ht_[ht_idx].null() || ht_[ht_idx].used == 0) return nullptr;
    const usize idx = hash_val & ht_[ht_idx].sizemask;
    DictEntry* e = ht_[ht_idx].buckets[idx];
    while (e) {
        if (e->hash_val == hash_val && e->key == key) return e;
        e = e->next;
    }
    return nullptr;
}

// --------------------------------------------------------------------------
// maybe_expand — trigger rehash if load factor exceeds threshold
// --------------------------------------------------------------------------
void Dict::maybe_expand() {
    if (is_rehashing()) return;  // Already rehashing
    if (ht_[0].null()) return;

    const float lf = load_factor();
    if (lf > constants::DICT_EXPAND_RATIO) {
        // New size: smallest power-of-two >= 2 * current_used
        const usize new_size = std::bit_ceil(ht_[0].used * 2);
        ht_[1].allocate(new_size);
        rehashidx_ = 0;
        log::debug("Dict expanding: size=%zu -> %zu (load=%.2f)",
                   ht_[0].size, new_size, lf);
    }
}

// --------------------------------------------------------------------------
// maybe_shrink — shrink if load factor drops below threshold (after mass DELs)
// --------------------------------------------------------------------------
void Dict::maybe_shrink() {
    if (is_rehashing()) return;
    if (ht_[0].null() || ht_[0].size <= 4) return;  // Don't shrink below minimum

    const float lf = load_factor();
    if (lf < constants::DICT_SHRINK_RATIO) {
        const usize new_size = std::bit_ceil(ht_[0].used < 4 ? 4UZ : ht_[0].used);
        ht_[1].allocate(new_size);
        rehashidx_ = 0;
        log::debug("Dict shrinking: size=%zu -> %zu (load=%.2f)",
                   ht_[0].size, new_size, lf);
    }
}

// --------------------------------------------------------------------------
// load_factor
// --------------------------------------------------------------------------
float Dict::load_factor() const noexcept {
    if (ht_[0].size == 0) return 0.0f;
    return static_cast<float>(ht_[0].used) / static_cast<float>(ht_[0].size);
}

// --------------------------------------------------------------------------
// rehash_step — migrate 'steps' buckets from ht[0] to ht[1]
// --------------------------------------------------------------------------
bool Dict::rehash_step(int steps) {
    if (!is_rehashing()) return true;  // Nothing to do

    // Safety limit: don't scan more than 10x steps empty buckets
    int empty_limit = steps * 10;

    while (steps-- > 0 && ht_[0].used > 0) {
        // Skip empty buckets
        while (ht_[0].buckets[static_cast<usize>(rehashidx_)] == nullptr) {
            ++rehashidx_;
            if (--empty_limit == 0) return false;
        }

        // Migrate all entries in this bucket chain
        DictEntry* entry = ht_[0].buckets[static_cast<usize>(rehashidx_)];
        while (entry) {
            DictEntry* next = entry->next;

            // Compute new bucket index in ht_[1]
            const usize new_idx = entry->hash_val & ht_[1].sizemask;

            // Prepend to ht_[1] chain (O(1))
            entry->next = ht_[1].buckets[new_idx];
            ht_[1].buckets[new_idx] = entry;
            ht_[1].used++;
            ht_[0].used--;

            entry = next;
        }
        ht_[0].buckets[static_cast<usize>(rehashidx_)] = nullptr;
        ++rehashidx_;
    }

    // Check if ht_[0] is fully migrated
    if (ht_[0].used == 0) {
        ht_[0].deallocate();
        ht_[0] = ht_[1];
        ht_[1] = DictHashTable{};  // Reset ht_[1] to null state
        rehashidx_ = -1;
        log::debug("Dict rehash complete: size=%zu", ht_[0].size);
        return true;
    }

    return false;
}

// --------------------------------------------------------------------------
// set — insert or update a key-value pair
// --------------------------------------------------------------------------
bool Dict::set(std::string_view key, void* value) {
    // Always migrate one batch before each mutation (amortized rehash)
    if (is_rehashing()) rehash_step(1);

    const u64 h = hash(key);

    // During rehash, check if key already exists in ht_[0]
    if (is_rehashing()) {
        DictEntry* existing = find_in(0, key, h);
        if (existing) {
            if (value_deleter_ && existing->value) value_deleter_(existing->value);
            existing->value = value;
            return false;
        }
    }

    // Choose the target table: ht_[1] during rehash, ht_[0] otherwise
    const int target = is_rehashing() ? 1 : 0;

    // Check for existing entry in the target table
    DictEntry* existing = find_in(target, key, h);
    if (existing) {
        if (value_deleter_ && existing->value) value_deleter_(existing->value);
        existing->value = value;
        return false;
    }

    // Create a new entry and prepend to the bucket chain (O(1))
    const usize idx = h & ht_[target].sizemask;
    auto* entry = new DictEntry{key, value, h};
    entry->next = ht_[target].buckets[idx];
    ht_[target].buckets[idx] = entry;
    ht_[target].used++;

    maybe_expand();
    return true;  // New entry created
}

// --------------------------------------------------------------------------
// get — look up a key; returns nullptr if not found
// --------------------------------------------------------------------------
void* Dict::get(std::string_view key) {
    if (is_rehashing()) rehash_step(1);

    const u64 h = hash(key);

    // Search ht_[0] first, then ht_[1] (in case key was already migrated)
    for (int t = 0; t < 2; ++t) {
        DictEntry* e = find_in(t, key, h);
        if (e) return e->value;
        if (!is_rehashing()) break;  // Only check ht_[1] during rehash
    }
    return nullptr;
}

const void* Dict::get(std::string_view key) const {
    const u64 h = hash(key);
    for (int t = 0; t < 2; ++t) {
        DictEntry* e = find_in(t, key, h);
        if (e) return e->value;
        if (!is_rehashing()) break;
    }
    return nullptr;
}

// --------------------------------------------------------------------------
// del — remove a key; returns true if it existed
// --------------------------------------------------------------------------
bool Dict::del(std::string_view key) {
    if (is_rehashing()) rehash_step(1);

    const u64 h = hash(key);

    for (int t = 0; t < 2; ++t) {
        if (ht_[t].null() || ht_[t].used == 0) {
            if (!is_rehashing()) break;
            continue;
        }
        const usize idx = h & ht_[t].sizemask;
        DictEntry* prev  = nullptr;
        DictEntry* entry = ht_[t].buckets[idx];

        while (entry) {
            if (entry->hash_val == h && entry->key == key) {
                // Unlink from chain
                if (prev) {
                    prev->next = entry->next;
                } else {
                    ht_[t].buckets[idx] = entry->next;
                }
                if (value_deleter_ && entry->value) {
                    value_deleter_(entry->value);
                }
                delete entry;
                ht_[t].used--;
                maybe_shrink();
                return true;
            }
            prev  = entry;
            entry = entry->next;
        }

        if (!is_rehashing()) break;
    }
    return false;
}

// --------------------------------------------------------------------------
// exists — check if a key is present
// --------------------------------------------------------------------------
bool Dict::exists(std::string_view key) const {
    return get(key) != nullptr;
}

// --------------------------------------------------------------------------
// for_each — iterate all entries (both tables during rehash)
// --------------------------------------------------------------------------
void Dict::for_each(const std::function<void(std::string_view, void*)>& visitor) const {
    for (int t = 0; t < 2; ++t) {
        if (ht_[t].null()) continue;
        for (usize i = 0; i < ht_[t].size; ++i) {
            DictEntry* entry = ht_[t].buckets[i];
            while (entry) {
                visitor(entry->key.view(), entry->value);
                entry = entry->next;
            }
        }
        if (!is_rehashing()) break;
    }
}

// --------------------------------------------------------------------------
// random_sample — pick up to 'count' random entries for LRU eviction
// --------------------------------------------------------------------------
usize Dict::random_sample(usize count, std::vector<DictEntry*>& out) {
    if (size() == 0) return 0;

    // Use a simple LCG random to pick random bucket positions
    static thread_local std::mt19937_64 rng{std::random_device{}()};

    usize found = 0;
    usize attempts = 0;
    const usize max_attempts = count * 20;

    while (found < count && attempts < max_attempts) {
        ++attempts;
        // Pick a random table to sample from (prefer ht_[0] by 2:1)
        int t = 0;
        if (is_rehashing() && (rng() % 3 == 0)) t = 1;
        if (ht_[t].null() || ht_[t].used == 0) continue;

        const usize idx = rng() % ht_[t].size;
        DictEntry* entry = ht_[t].buckets[idx];
        if (!entry) continue;

        // Walk the chain and collect entries
        while (entry && found < count) {
            out.push_back(entry);
            ++found;
            entry = entry->next;
        }
    }
    return found;
}

} // namespace keva::core
