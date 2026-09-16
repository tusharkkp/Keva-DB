// ==============================================================================
// src/core/object.cpp
//
// Purpose:
//   Implements KevaObject factory methods and payload accessors, specifically
//   the encoding selection algorithm for string values.
//
//   Encoding selection algorithm for create_string():
//
//   Step 1: Try to parse the string as int64.
//     - If successful AND value is in [LLONG_MIN, LLONG_MAX]:
//       Use ENCODING_INT — store the integer directly in the ptr field.
//       Why cast to void*? On all 64-bit platforms, sizeof(void*) == sizeof(int64_t) == 8.
//       We bit-cast the integer directly into the pointer slot. Zero heap allocation.
//
//   Step 2: If len <= EMBSTR_MAX_LEN (44 bytes):
//     Use ENCODING_EMBSTR — allocate the KevaObject and a Buffer in one
//     contiguous malloc() call. Layout:
//       [KevaObject header][Buffer object][char data...]
//     Only ONE allocation (new char[]) is needed. The Buffer object is
//     placement-new'd directly after the KevaObject in the same memory block.
//     This guarantees L1 cache co-location of the object header and its data.
//
//   Step 3: Otherwise (len > 44):
//     Use ENCODING_RAW — allocate a KevaObject and a separate Buffer heap object.
//     Two allocations, but necessary for large strings.
//
//   The embstr optimization is one of the most celebrated micro-optimizations
//   in Redis's source code and a deep lesson in cache-friendly memory layout.
// ==============================================================================

#include "keva/core/object.hpp"

#include <cstring>
#include <ctime>
#include <new>       // std::launder, placement new
#include <climits>   // LLONG_MIN, LLONG_MAX
#include <bit>       // std::bit_cast

namespace keva::core {

// --------------------------------------------------------------------------
// lru_clock::current() — seconds since Unix epoch, truncated to 24 bits
// --------------------------------------------------------------------------
u32 lru_clock::current() noexcept {
    return static_cast<u32>(::time(nullptr)) & constants::LRU_CLOCK_MAX;
}

u32 lru_clock::idle_seconds(u32 obj_lru) noexcept {
    u32 now = current();
    // Handle 24-bit clock wrap-around
    if (now >= obj_lru) {
        return now - obj_lru;
    }
    // Clock wrapped: add LRU_CLOCK_MAX to account for the wrap
    return (constants::LRU_CLOCK_MAX - obj_lru) + now;
}

// --------------------------------------------------------------------------
// Helper: make a plain KevaObject with type and encoding set
// --------------------------------------------------------------------------
static KevaObject* alloc_object(ObjectType type, ObjectEncoding enc, u32 lru_clock_val) {
    auto* obj = new KevaObject{};
    obj->set_type(type);
    obj->set_encoding(enc);
    obj->set_lru(lru_clock_val);
    obj->refcount = 1;
    return obj;
}

// --------------------------------------------------------------------------
// create_string — select optimal encoding for a string value
// --------------------------------------------------------------------------
KevaObject* KevaObject::create_string(std::string_view sv, u32 lru_clock_val) {
    // Step 1: Try integer encoding (zero heap allocation path)
    i64 int_val = 0;
    Buffer tmp_buf(sv);
    if (tmp_buf.try_parse_int(int_val)) {
        auto* obj = alloc_object(ObjectType::String, ObjectEncoding::Int, lru_clock_val);
        // Store the integer directly in the pointer slot using std::bit_cast.
        // Safe on all 64-bit platforms: sizeof(int64_t) == sizeof(void*) == 8.
        obj->ptr = std::bit_cast<void*>(int_val);
        return obj;
    }

    // Step 2: Embedded string encoding (single-allocation path)
    if (sv.size() <= constants::EMBSTR_MAX_LEN) {
        // Allocate: sizeof(KevaObject) + sizeof(Buffer) + sv.size() + 1 (null sentinel)
        const usize total = sizeof(KevaObject) + sizeof(Buffer) + sv.size() + 1;
        char* block = new char[total];

        // Placement-new the KevaObject at the beginning of the block
        auto* obj = new (block) KevaObject{};
        obj->set_type(ObjectType::String);
        obj->set_encoding(ObjectEncoding::Embstr);
        obj->set_lru(lru_clock_val);
        obj->refcount = 1;

        // Placement-new the Buffer immediately after the KevaObject
        auto* buf = new (block + sizeof(KevaObject)) Buffer{sv};
        obj->ptr = buf;

        return obj;
    }

    // Step 3: Raw encoding (two separate allocations for large strings)
    auto* obj = alloc_object(ObjectType::String, ObjectEncoding::Raw, lru_clock_val);
    obj->ptr = new Buffer{sv};
    return obj;
}

// --------------------------------------------------------------------------
// create_string_from_int — directly create an INT-encoded string object
// --------------------------------------------------------------------------
KevaObject* KevaObject::create_string_from_int(i64 value, u32 lru_clock_val) {
    auto* obj = alloc_object(ObjectType::String, ObjectEncoding::Int, lru_clock_val);
    obj->ptr = std::bit_cast<void*>(value);
    return obj;
}

// --------------------------------------------------------------------------
// string_view — extract string content from any encoding as a string_view
// --------------------------------------------------------------------------
std::string_view KevaObject::string_view() const noexcept {
    switch (encoding()) {
        case ObjectEncoding::Raw:
        case ObjectEncoding::Embstr: {
            const auto* buf = static_cast<const Buffer*>(ptr);
            return buf->view();
        }
        case ObjectEncoding::Int: {
            // INT-encoded: the value is stored in ptr, not accessible as a
            // raw string_view without allocating. Callers should use integer_value()
            // and format it. Returning empty to signal this.
            return {};
        }
    }
    return {};
}

// --------------------------------------------------------------------------
// integer_value — extract int64 from ENCODING_INT object
// --------------------------------------------------------------------------
i64 KevaObject::integer_value() const noexcept {
    return std::bit_cast<i64>(ptr);
}

// --------------------------------------------------------------------------
// buffer — access the Buffer payload (RAW / EMBSTR encodings)
// --------------------------------------------------------------------------
Buffer* KevaObject::buffer() noexcept {
    return static_cast<Buffer*>(ptr);
}

const Buffer* KevaObject::buffer() const noexcept {
    return static_cast<const Buffer*>(ptr);
}

// --------------------------------------------------------------------------
// destroy — free a KevaObject accounting for all encoding types
// --------------------------------------------------------------------------
void KevaObject::destroy(KevaObject* obj) {
    if (!obj) return;

    switch (obj->encoding()) {
        case ObjectEncoding::Int:
            // No heap allocation for the payload — just delete the header
            delete obj;
            break;

        case ObjectEncoding::Embstr: {
            // Single contiguous block: [KevaObject | Buffer | char data]
            // Must call Buffer's destructor explicitly (since we used placement new),
            // then delete[] the whole block.
            auto* buf = static_cast<Buffer*>(obj->ptr);
            buf->~Buffer();
            // obj points to the start of the char[] block allocated in create_string
            delete[] reinterpret_cast<char*>(obj);
            break;
        }

        case ObjectEncoding::Raw: {
            // Two separate allocations
            delete static_cast<Buffer*>(obj->ptr);
            delete obj;
            break;
        }
    }
}

} // namespace keva::core
