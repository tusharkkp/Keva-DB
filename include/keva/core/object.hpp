// ==============================================================================
// keva/core/object.hpp
//
// Purpose:
//   Defines KevaObject — the universal value container for every key in the
//   Keva database, directly inspired by Redis's 'robj' (redis object) struct.
//
//   KevaObject serves three critical roles:
//
//   1. Type Tagging:
//      Every value stored in Keva has a TYPE (String, List, Hash, Set, ZSet).
//      The type is checked by command handlers to enforce Redis-compatible
//      type safety (e.g., you cannot run LPUSH on a string key).
//
//   2. Encoding Optimization:
//      The same logical type can be stored in multiple physical memory encodings
//      for efficiency:
//      - A string containing "12345" can be stored as an int64_t directly in
//        the ptr field (ENCODING_INT), requiring zero heap allocation.
//      - A string of <= 44 bytes is stored in an embedded allocation
//        (ENCODING_EMBSTR) where the KevaObject header and the string buffer
//        are a single contiguous malloc() call.
//      - Larger strings use a separate Buffer allocation (ENCODING_RAW).
//
//   3. LRU Clock for Eviction:
//      The lru field stores a 24-bit server clock value (seconds resolution,
//      wraps every ~194 days). This timestamp is updated on every access and
//      used by the approximated LRU eviction algorithm to select the "coldest"
//      (least recently used) key to evict when maxmemory is reached.
//      Critically, this is stored as bits inside the KevaObject header itself —
//      no separate linked list, no write to a global LRU chain on every read.
//
//   Memory layout of the header (packed to 16 bytes):
//   [type: 4 bits | encoding: 4 bits | lru: 24 bits | refcount: 32 bits]
//   [ptr: 64 bits (8 bytes)]
//   Total: 4 + 4 + 24 (packed into 32 bits) + 32 bits + 64 bits = 128 bits = 16 bytes
// ==============================================================================

#pragma once

#include "keva/common/types.hpp"
#include "keva/core/buffer.hpp"

#include <memory>
#include <string_view>

namespace keva::core {

// --------------------------------------------------------------------------
// ObjectType — the logical data type of a Keva value
// --------------------------------------------------------------------------
enum class ObjectType : u8 {
    String  = 0,   // OBJ_STRING
    List    = 1,   // OBJ_LIST    (Phase 2)
    Set     = 2,   // OBJ_SET     (Phase 2)
    ZSet    = 3,   // OBJ_ZSET    (Phase 2)
    Hash    = 4,   // OBJ_HASH    (Phase 2)
};

// --------------------------------------------------------------------------
// ObjectEncoding — the physical memory layout / encoding of a value
// --------------------------------------------------------------------------
enum class ObjectEncoding : u8 {
    Raw     = 0,   // OBJ_ENCODING_RAW   : Separate Buffer allocation on heap
    Int     = 1,   // OBJ_ENCODING_INT   : Signed 64-bit int stored in ptr field directly
    Embstr  = 2,   // OBJ_ENCODING_EMBSTR: Single-allocation embedded string (<= 44 bytes)
    // Future Phase 2 encodings:
    // Ziplist, Listpack, Skiplist, Hashtable, etc.
};

// --------------------------------------------------------------------------
// KevaObject — the universal value container
// --------------------------------------------------------------------------
struct KevaObject {
    // --- Packed 32-bit metadata word ---
    // Layout (from LSB to MSB):
    //   bits  0- 3 : ObjectType  (4 bits, values 0-15)
    //   bits  4- 7 : ObjectEncoding (4 bits, values 0-15)
    //   bits  8-31 : LRU clock (24 bits, seconds since server start mod 2^24)
    u32 meta = 0;

    // Reference count: used for shared object optimization
    // (e.g., shared integer objects for values 0-9999 like Redis does)
    u32 refcount = 1;

    // Payload pointer:
    //   - ENCODING_RAW   : points to a heap-allocated Buffer
    //   - ENCODING_EMBSTR: points to an embedded Buffer within same allocation
    //   - ENCODING_INT   : stores the int64_t value directly (cast to void*)
    void* ptr = nullptr;

    // --------------------------------------------------------------------------
    // Bit-field accessors for 'meta'
    // --------------------------------------------------------------------------
    [[nodiscard]] ObjectType     type()     const noexcept { return static_cast<ObjectType>(meta & 0xF); }
    [[nodiscard]] ObjectEncoding encoding() const noexcept { return static_cast<ObjectEncoding>((meta >> 4) & 0xF); }
    [[nodiscard]] u32            lru()      const noexcept { return (meta >> 8) & 0xFFFFFF; }

    void set_type(ObjectType t)       noexcept { meta = (meta & ~0xFu)          | (static_cast<u32>(t) & 0xF); }
    void set_encoding(ObjectEncoding e)noexcept { meta = (meta & ~0xF0u)        | ((static_cast<u32>(e) & 0xF) << 4); }
    void set_lru(u32 lru_val)         noexcept { meta = (meta & 0xFFu)          | ((lru_val & 0xFFFFFF) << 8); }

    // --------------------------------------------------------------------------
    // Factory functions — create KevaObjects with the optimal encoding
    // --------------------------------------------------------------------------

    // Create a String KevaObject.
    // - If the content parses as int64: ENCODING_INT (zero heap allocation)
    // - If len <= EMBSTR_MAX_LEN: ENCODING_EMBSTR (single allocation)
    // - Otherwise: ENCODING_RAW (separate Buffer allocation)
    static KevaObject* create_string(std::string_view sv, u32 lru_clock);
    static KevaObject* create_string_from_int(i64 value, u32 lru_clock);

    // --------------------------------------------------------------------------
    // Payload accessors (for String objects)
    // --------------------------------------------------------------------------

    // Returns the string content as a string_view (works for all string encodings)
    [[nodiscard]] std::string_view string_view() const noexcept;

    // Returns the integer value (only valid for ENCODING_INT)
    [[nodiscard]] i64 integer_value() const noexcept;

    // Returns a pointer to the Buffer (only valid for ENCODING_RAW / ENCODING_EMBSTR)
    [[nodiscard]] Buffer* buffer() noexcept;
    [[nodiscard]] const Buffer* buffer() const noexcept;

    // --------------------------------------------------------------------------
    // Destructor logic — must handle all three encodings
    // --------------------------------------------------------------------------
    static void destroy(KevaObject* obj);

    // Update the LRU clock on access
    void touch(u32 lru_clock) noexcept { set_lru(lru_clock); }
};

// --------------------------------------------------------------------------
// LRU clock utilities (used by DB and eviction engine)
// --------------------------------------------------------------------------
namespace lru_clock {
    // Returns the current LRU clock value (seconds, truncated to 24 bits)
    u32 current() noexcept;
    // Compute idle time in seconds from a stored lru value and current clock
    u32 idle_seconds(u32 obj_lru) noexcept;
}

} // namespace keva::core
