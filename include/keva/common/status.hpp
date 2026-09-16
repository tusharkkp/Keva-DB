// ==============================================================================
// keva/common/status.hpp
//
// Purpose:
//   Defines a lightweight, value-type Status object used as the canonical
//   return type for all Keva subsystem operations (networking, parsing,
//   storage engine, persistence).
//
//   Design philosophy:
//   - Operations that can fail do NOT throw exceptions. Instead, they return
//     a Status value that the caller must inspect. This aligns with modern
//     systems programming practice (similar to leveldb::Status, absl::Status,
//     or Rust's Result<T, E>) where exceptions have unpredictable latency
//     implications in a hot database code path.
//   - The Status object is trivially copyable and extremely cheap to pass
//     by value (~2 words in the common success case when the message is empty).
//
//   Usage:
//     Status s = some_operation();
//     if (!s.ok()) {
//         logger.error("Failed: {}", s.message());
//     }
// ==============================================================================

#pragma once

#include <string>
#include <string_view>

namespace keva {

// --------------------------------------------------------------------------
// StatusCode — machine-readable error categories
// --------------------------------------------------------------------------
enum class StatusCode : int {
    Ok              = 0,   // Operation succeeded
    Err             = 1,   // Generic unclassified error
    IOError         = 2,   // OS-level I/O error (socket read/write, file I/O)
    ProtocolError   = 3,   // Malformed RESP2 protocol data from client
    NotFound        = 4,   // Key does not exist in the database
    WrongType       = 5,   // Command applied to wrong object type
    OutOfMemory     = 6,   // Server reached maxmemory limit
    InvalidArgs     = 7,   // Command called with wrong number / type of arguments
    Timeout         = 8,   // Operation timed out
    Overflow        = 9,   // Integer overflow (INCR/DECR beyond int64 range)
};

// --------------------------------------------------------------------------
// Status — value-type result carrier
// --------------------------------------------------------------------------
class Status {
public:
    // Construct a success status (most common path, no heap allocation)
    Status() : code_(StatusCode::Ok) {}

    // Construct an error status with a human-readable message
    explicit Status(StatusCode code, std::string_view message)
        : code_(code), message_(message) {}

    // --------------------------------------------------------------------------
    // Named constructors for ergonomic call sites
    // --------------------------------------------------------------------------
    static Status success()                         { return Status{}; }
    static Status err(std::string_view msg)         { return Status{StatusCode::Err, msg}; }
    static Status io_error(std::string_view msg)    { return Status{StatusCode::IOError, msg}; }
    static Status proto_error(std::string_view msg) { return Status{StatusCode::ProtocolError, msg}; }
    static Status not_found(std::string_view msg = "key not found")
                                                    { return Status{StatusCode::NotFound, msg}; }
    static Status wrong_type(std::string_view msg = "WRONGTYPE Operation against a key holding the wrong kind of value")
                                                    { return Status{StatusCode::WrongType, msg}; }
    static Status oom(std::string_view msg = "OOM command not allowed when used memory > 'maxmemory'")
                                                    { return Status{StatusCode::OutOfMemory, msg}; }
    static Status invalid_args(std::string_view msg){ return Status{StatusCode::InvalidArgs, msg}; }
    static Status overflow(std::string_view msg = "ERR increment or decrement would overflow")
                                                    { return Status{StatusCode::Overflow, msg}; }

    // --------------------------------------------------------------------------
    // Observers
    // --------------------------------------------------------------------------
    [[nodiscard]] bool ok()           const noexcept { return code_ == StatusCode::Ok; }
    [[nodiscard]] bool is_not_found() const noexcept { return code_ == StatusCode::NotFound; }
    [[nodiscard]] bool is_wrong_type()const noexcept { return code_ == StatusCode::WrongType; }
    [[nodiscard]] bool is_oom()       const noexcept { return code_ == StatusCode::OutOfMemory; }
    [[nodiscard]] StatusCode code()   const noexcept { return code_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

    // Implicit bool conversion for concise error checking: if (status) { ... }
    explicit operator bool() const noexcept { return ok(); }

private:
    StatusCode  code_;
    std::string message_;
};

} // namespace keva
