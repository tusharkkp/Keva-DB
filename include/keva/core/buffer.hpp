// ==============================================================================
// keva/core/buffer.hpp
//
// Purpose:
//   Declares the Buffer class — Keva's binary-safe dynamic byte array, inspired
//   by Redis's SDS (Simple Dynamic String) library.
//
//   Why not std::string?
//   std::string uses a null terminator ('\0') as its logical end-of-string
//   sentinel. If a client stores binary data (e.g., a compressed gzip payload,
//   a serialized protobuf, or an image) that contains embedded '\0' bytes,
//   std::string's C-string interoperability functions (c_str(), find(), etc.)
//   will silently truncate the data at the first null byte. This is a critical
//   correctness bug for a general-purpose key-value database.
//
//   Our Buffer class:
//   - Tracks length explicitly (no reliance on null terminator).
//   - Can store any byte sequence including '\0'.
//   - Uses power-of-two capacity growth (doubling strategy) to achieve
//     O(1) amortized appends — same as std::vector<char>.
//   - Provides O(1) length queries (unlike strlen which is O(N)).
//   - Keeps the underlying data null-terminated as a convenience (so it can
//     be passed to legacy C APIs when the content is known to be text),
//     but the null terminator is NOT counted in size_.
//
//   Memory layout (SDS-inspired):
//     [ data_[0] ... data_[size_-1] | '\0' (sentinel) | (unused capacity) ]
//     ^                              ^
//     data()                         size_ (explicit, not null-dependent)
// ==============================================================================

#pragma once

#include "keva/common/types.hpp"

#include <string_view>
#include <cstring>

namespace keva::core {

// --------------------------------------------------------------------------
// Buffer — binary-safe dynamic byte array
// --------------------------------------------------------------------------
class Buffer {
public:
    // Default-construct an empty buffer (no heap allocation)
    Buffer() = default;

    // Construct from a C-string (copies len bytes, does NOT depend on '\0')
    explicit Buffer(std::string_view sv);

    // Construct from raw bytes + length
    Buffer(const void* data, usize len);

    // Copy and move semantics
    Buffer(const Buffer& other);
    Buffer& operator=(const Buffer& other);
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;

    ~Buffer();

    // --------------------------------------------------------------------------
    // Mutation
    // --------------------------------------------------------------------------

    // Append bytes from a string_view
    void append(std::string_view sv);

    // Append raw bytes
    void append(const void* data, usize len);

    // Resize to exactly 'new_size' bytes (zero-fills newly added bytes)
    void resize(usize new_size);

    // Reserve capacity without changing size (avoids reallocations in tight loops)
    void reserve(usize capacity);

    // Clear contents (sets size=0 but retains allocated capacity)
    void clear() noexcept;

    // --------------------------------------------------------------------------
    // Observers
    // --------------------------------------------------------------------------
    [[nodiscard]] const char* data()      const noexcept { return data_; }
    [[nodiscard]] char*       data()            noexcept { return data_; }
    [[nodiscard]] usize       size()      const noexcept { return size_; }
    [[nodiscard]] usize       capacity()  const noexcept { return cap_; }
    [[nodiscard]] bool        empty()     const noexcept { return size_ == 0; }

    // Returns a non-owning view of the buffer contents (does not include '\0')
    [[nodiscard]] std::string_view view() const noexcept { return {data_, size_}; }

    // Comparison
    bool operator==(const Buffer& other) const noexcept;
    bool operator==(std::string_view sv) const noexcept;

    // Try to parse the buffer content as a 64-bit integer.
    // Returns true and populates 'out' if the entire buffer is a valid integer.
    [[nodiscard]] bool try_parse_int(i64& out) const noexcept;

private:
    // Grow capacity to at least 'required' bytes (power-of-two rounding)
    void grow(usize required);

    char*  data_ = nullptr;
    usize  size_ = 0;    // Number of valid bytes (NOT including null sentinel)
    usize  cap_  = 0;    // Allocated capacity (NOT including null sentinel byte)
};

} // namespace keva::core
