// ==============================================================================
// keva/protocol/resp_parser.hpp
//
// Purpose:
//   Declares the RespParser class — a streaming, zero-copy Finite State Machine
//   (FSM) that parses incoming bytes from a TCP connection into typed RespValue
//   objects without any intermediate heap allocations during tokenization.
//
//   Why a streaming parser?
//   TCP is a streaming byte protocol with no message boundaries. A single
//   RESP2 command like:
//       *3\r\n$3\r\nSET\r\n$4\r\nname\r\n$5\r\nAlice\r\n
//   may arrive in fragments over multiple read() syscalls:
//       Read 1: "*3\r\n$3\r\n"
//       Read 2: "SET\r\n$4\r\nna"
//       Read 3: "me\r\n$5\r\nAlice\r\n"
//
//   A streaming parser consumes bytes as they arrive and saves its state
//   between calls using the State enum, so it can resume parsing exactly
//   where it left off when more bytes become available.
//
//   Why "zero-copy"?
//   The parser works on std::string_view slices of the Connection's read
//   buffer directly. It does not copy individual bytes to a temporary token
//   buffer — it reads the length prefix (e.g. `$5`) and directly extracts
//   a string_view of exactly 5 bytes from the buffer. Only when we need to
//   store the parsed string in a RespValue (which lives beyond the buffer's
//   lifetime) do we construct a std::string copy.
// ==============================================================================

#pragma once

#include "keva/protocol/resp_types.hpp"
#include "keva/common/status.hpp"

#include <string_view>
#include <string>
#include <vector>

namespace keva::protocol {

// --------------------------------------------------------------------------
// RespParser — streaming FSM for RESP2 protocol parsing
// --------------------------------------------------------------------------
class RespParser {
public:
    RespParser() = default;

    // --------------------------------------------------------------------------
    // parse() — attempt to parse one complete RESP2 message from 'input'.
    //
    // Parameters:
    //   input         : A view of the unconsumed bytes in the connection's
    //                   read buffer (Connection::read_view()).
    //   out_value     : Populated with the parsed RespValue on ParseResult::Complete.
    //   out_consumed  : Number of bytes consumed from 'input'. The caller must
    //                   call Connection::consume(out_consumed) after this returns.
    //
    // Returns:
    //   ParseResult::Complete   — full message parsed; out_value is ready.
    //   ParseResult::Incomplete — not enough bytes; call again with more data.
    //   ParseResult::Error      — malformed RESP2; close the connection.
    // --------------------------------------------------------------------------
    ParseResult parse(std::string_view input, RespValue& out_value, usize& out_consumed);

    // Reset all parser state (called on connection close)
    void reset();

private:
    // Scan 'input' for a '\r\n' line terminator.
    // Returns the position of '\r', or std::string_view::npos if not found.
    static usize find_crlf(std::string_view input) noexcept;

    // Parse a line-terminated integer ("+OK\r\n", ":42\r\n", etc.)
    // Advances 'pos' past the \r\n delimiter.
    // Returns false if the line is malformed.
    static bool parse_line(std::string_view input, usize& pos, std::string& out_line);

    // Parse a RESP simple type: Simple String, Error, or Integer (no nested parsing)
    ParseResult parse_simple(std::string_view input, RespValue& out, usize& pos);

    // Parse a bulk string ($<len>\r\n<data>\r\n)
    ParseResult parse_bulk_string(std::string_view input, RespValue& out, usize& pos);

    // Parse an array (*<count>\r\n<element1>...<elementN>)
    // Calls parse_value() recursively for each element.
    ParseResult parse_array(std::string_view input, RespValue& out, usize& pos);

    // Top-level recursive parser: dispatches based on the first byte prefix
    ParseResult parse_value(std::string_view input, RespValue& out, usize& pos);
};

} // namespace keva::protocol
