// ==============================================================================
// src/core/buffer.cpp
//
// Purpose:
//   Implements the Buffer binary-safe dynamic byte array.
//
//   Key implementation details:
//
//   Power-of-two growth strategy:
//   When more capacity is needed, grow() rounds up to the next power of two.
//   This ensures at most O(log N) reallocations as data is appended N times,
//   giving O(1) amortized cost per append — the same guarantee as std::vector.
//   The power-of-two size also aligns well with jemalloc/glibc malloc size
//   classes, reducing internal fragmentation in the heap allocator.
//
//   Null sentinel byte:
//   We always allocate cap_ + 1 bytes and keep data_[size_] = '\0'. This means
//   the buffer can be safely passed to C APIs expecting a null-terminated string
//   in the common case where the content happens to be text (without paying any
//   performance penalty for binary data since size_ tracks real length).
//
//   try_parse_int():
//   Uses std::from_chars for locale-independent, zero-allocation parsing. This
//   is how we detect "can this string value be stored as an int64_t encoding"
//   (the OBJ_ENCODING_INT path in KevaObject, which saves heap allocations for
//   numeric strings like counters).
// ==============================================================================

#include "keva/core/buffer.hpp"

#include <charconv>
#include <cstring>
#include <stdexcept>
#include <bit>        // std::bit_ceil (C++20 power-of-two rounding)

namespace keva::core {

// --------------------------------------------------------------------------
// Constructors
// --------------------------------------------------------------------------
Buffer::Buffer(std::string_view sv) {
    if (!sv.empty()) {
        grow(sv.size());
        std::memcpy(data_, sv.data(), sv.size());
        size_ = sv.size();
        data_[size_] = '\0';
    }
}

Buffer::Buffer(const void* raw_data, usize len) {
    if (len > 0) {
        grow(len);
        std::memcpy(data_, raw_data, len);
        size_ = len;
        data_[size_] = '\0';
    }
}

Buffer::Buffer(const Buffer& other) {
    if (other.size_ > 0) {
        grow(other.size_);
        std::memcpy(data_, other.data_, other.size_);
        size_ = other.size_;
        data_[size_] = '\0';
    }
}

Buffer& Buffer::operator=(const Buffer& other) {
    if (this != &other) {
        clear();
        if (other.size_ > 0) {
            grow(other.size_);
            std::memcpy(data_, other.data_, other.size_);
            size_ = other.size_;
            data_[size_] = '\0';
        }
    }
    return *this;
}

Buffer::Buffer(Buffer&& other) noexcept
    : data_(other.data_), size_(other.size_), cap_(other.cap_)
{
    other.data_ = nullptr;
    other.size_ = 0;
    other.cap_  = 0;
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        delete[] data_;
        data_ = other.data_;
        size_ = other.size_;
        cap_  = other.cap_;
        other.data_ = nullptr;
        other.size_ = 0;
        other.cap_  = 0;
    }
    return *this;
}

Buffer::~Buffer() {
    delete[] data_;
}

// --------------------------------------------------------------------------
// grow — allocate new capacity rounded up to next power of two
// --------------------------------------------------------------------------
void Buffer::grow(usize required) {
    if (required <= cap_) return;

    // std::bit_ceil: rounds up to next power of two (C++20)
    // Minimum of 16 bytes to avoid tiny first allocations
    usize new_cap = std::bit_ceil(required < 16 ? 16UZ : required);

    char* new_data = new char[new_cap + 1];  // +1 for null sentinel
    if (data_ && size_ > 0) {
        std::memcpy(new_data, data_, size_);
    }
    delete[] data_;
    data_ = new_data;
    cap_  = new_cap;
    data_[size_] = '\0';
}

// --------------------------------------------------------------------------
// append
// --------------------------------------------------------------------------
void Buffer::append(std::string_view sv) {
    if (sv.empty()) return;
    grow(size_ + sv.size());
    std::memcpy(data_ + size_, sv.data(), sv.size());
    size_ += sv.size();
    data_[size_] = '\0';
}

void Buffer::append(const void* raw_data, usize len) {
    if (len == 0) return;
    grow(size_ + len);
    std::memcpy(data_ + size_, raw_data, len);
    size_ += len;
    data_[size_] = '\0';
}

// --------------------------------------------------------------------------
// resize
// --------------------------------------------------------------------------
void Buffer::resize(usize new_size) {
    if (new_size > cap_) grow(new_size);
    if (new_size > size_) {
        std::memset(data_ + size_, 0, new_size - size_);
    }
    size_ = new_size;
    data_[size_] = '\0';
}

// --------------------------------------------------------------------------
// reserve
// --------------------------------------------------------------------------
void Buffer::reserve(usize capacity) {
    grow(capacity);
}

// --------------------------------------------------------------------------
// clear
// --------------------------------------------------------------------------
void Buffer::clear() noexcept {
    size_ = 0;
    if (data_) data_[0] = '\0';
}

// --------------------------------------------------------------------------
// Comparison operators
// --------------------------------------------------------------------------
bool Buffer::operator==(const Buffer& other) const noexcept {
    if (size_ != other.size_) return false;
    return std::memcmp(data_, other.data_, size_) == 0;
}

bool Buffer::operator==(std::string_view sv) const noexcept {
    if (size_ != sv.size()) return false;
    return std::memcmp(data_, sv.data(), size_) == 0;
}

// --------------------------------------------------------------------------
// try_parse_int — attempt to interpret the buffer as a 64-bit integer
// --------------------------------------------------------------------------
bool Buffer::try_parse_int(i64& out) const noexcept {
    if (size_ == 0 || size_ > 20) return false;  // i64 max is 19 digits + sign
    const auto [end, ec] = std::from_chars(data_, data_ + size_, out);
    // Must consume the entire buffer (no trailing garbage)
    return ec == std::errc{} && end == data_ + size_;
}

} // namespace keva::core
