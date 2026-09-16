// ==============================================================================
// keva/protocol/resp_types.hpp
//
// Purpose:
//   Defines the data types used to represent parsed RESP2 protocol messages
//   as in-memory C++ structures (the Abstract Syntax Tree of the protocol).
//
//   The RESP2 wire format uses a single byte prefix to identify the type:
//     '+' Simple String   : +OK\r\n
//     '-' Error           : -ERR unknown command\r\n
//     ':' Integer         : :1000\r\n
//     '$' Bulk String     : $5\r\nhello\r\n  (or $-1\r\n for Nil)
//     '*' Array           : *2\r\n$3\r\nGET\r\n$3\r\nkey\r\n
//
//   Why RespValue as a C++ type?
//   After the RESP2 parser reads raw bytes off the network, it produces
//   RespValue objects that the command dispatcher can inspect to extract
//   the command name and its arguments. This clean separation means:
//   - The parser does not need to know about "SET" or "GET" commands.
//   - The command handlers do not need to know about raw bytes or \r\n framing.
//
//   Design: We use a tagged union approach (RespType enum + std::string for
//   string payloads + std::vector for arrays) rather than std::variant to
//   keep the code readable for learners and avoid template metaprogramming.
// ==============================================================================

#pragma once

#include "keva/common/types.hpp"

#include <string>
#include <vector>
#include <string_view>

namespace keva::protocol {

// --------------------------------------------------------------------------
// RespType — the RESP2 data type tag
// --------------------------------------------------------------------------
enum class RespType : u8 {
    SimpleString,  // '+' prefix
    Error,         // '-' prefix
    Integer,       // ':' prefix
    BulkString,    // '$' prefix (may be Nil if length == -1)
    Array,         // '*' prefix (may be Nil if count == -1)
    Nil,           // Represents a null bulk string ($-1\r\n) or null array (*-1\r\n)
};

// --------------------------------------------------------------------------
// RespValue — a parsed RESP2 value (recursive for arrays)
// --------------------------------------------------------------------------
struct RespValue {
    RespType type = RespType::Nil;

    // Payload for SimpleString, Error, BulkString
    std::string str;

    // Payload for Integer
    i64 integer = 0;

    // Payload for Array (recursive: each element is a RespValue)
    std::vector<RespValue> array;

    // --------------------------------------------------------------------------
    // Factories for constructing RespValue objects ergonomically
    // --------------------------------------------------------------------------
    static RespValue simple_string(std::string s) {
        RespValue v;
        v.type = RespType::SimpleString;
        v.str  = std::move(s);
        return v;
    }

    static RespValue error(std::string s) {
        RespValue v;
        v.type = RespType::Error;
        v.str  = std::move(s);
        return v;
    }

    static RespValue integer(i64 n) {
        RespValue v;
        v.type    = RespType::Integer;
        v.integer = n;
        return v;
    }

    static RespValue bulk_string(std::string s) {
        RespValue v;
        v.type = RespType::BulkString;
        v.str  = std::move(s);
        return v;
    }

    static RespValue nil() {
        RespValue v;
        v.type = RespType::Nil;
        return v;
    }

    static RespValue array(std::vector<RespValue> elements) {
        RespValue v;
        v.type  = RespType::Array;
        v.array = std::move(elements);
        return v;
    }

    // --------------------------------------------------------------------------
    // Observers
    // --------------------------------------------------------------------------
    [[nodiscard]] bool is_nil()          const noexcept { return type == RespType::Nil; }
    [[nodiscard]] bool is_array()        const noexcept { return type == RespType::Array; }
    [[nodiscard]] bool is_bulk_string()  const noexcept { return type == RespType::BulkString; }
    [[nodiscard]] bool is_integer()      const noexcept { return type == RespType::Integer; }
    [[nodiscard]] bool is_error()        const noexcept { return type == RespType::Error; }
    [[nodiscard]] bool is_simple_string()const noexcept { return type == RespType::SimpleString; }
};

// --------------------------------------------------------------------------
// ParseResult — outcome of a single parse attempt
// --------------------------------------------------------------------------
enum class ParseResult {
    Complete,    // A full RESP2 message was successfully parsed
    Incomplete,  // Not enough bytes yet; need more data from the network
    Error,       // Malformed protocol: connection should be closed
};

} // namespace keva::protocol
