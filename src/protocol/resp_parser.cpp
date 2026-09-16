// ==============================================================================
// src/protocol/resp_parser.cpp
//
// Purpose:
//   Implements the RespParser Finite State Machine (FSM) for streaming RESP2
//   protocol parsing, as declared in keva/protocol/resp_parser.hpp.
//
//   The parser is recursive-descent:
//   - parse() -> parse_value() dispatches on the first byte:
//     '+' -> parse_simple() -> SimpleString
//     '-' -> parse_simple() -> Error
//     ':' -> parse_simple() -> Integer
//     '$' -> parse_bulk_string() -> BulkString or Nil
//     '*' -> parse_array() -> Array (calls parse_value() for each element)
//
//   Streaming contract:
//   - If at any point we do not have enough bytes to complete a token, we
//     return ParseResult::Incomplete immediately. The caller keeps the
//     unconsumed bytes in the connection buffer and retries parsing after
//     the next read() delivers more data.
//   - We only advance the 'pos' cursor when we have confirmed all required
//     bytes are present. This means partial parses never corrupt state.
//
//   Example trace for "*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n":
//   1. parse_value() sees '*', calls parse_array()
//   2. parse_array() reads count=2
//   3. Element 0: parse_value() sees '$', calls parse_bulk_string() -> "GET"
//   4. Element 1: parse_value() sees '$', calls parse_bulk_string() -> "key"
//   5. Returns Complete, out_consumed = total bytes consumed
// ==============================================================================

#include "keva/protocol/resp_parser.hpp"

#include <charconv>   // std::from_chars: locale-independent integer parsing
#include <cstring>

namespace keva::protocol {

// --------------------------------------------------------------------------
// Helper: find_crlf — locate \r\n in a string_view
// --------------------------------------------------------------------------
usize RespParser::find_crlf(std::string_view input) noexcept {
    for (usize i = 0; i + 1 < input.size(); ++i) {
        if (input[i] == '\r' && input[i + 1] == '\n') {
            return i;
        }
    }
    return std::string_view::npos;
}

// --------------------------------------------------------------------------
// Helper: parse_line — extract a line (up to \r\n) and advance pos
// --------------------------------------------------------------------------
bool RespParser::parse_line(std::string_view input, usize& pos, std::string& out_line) {
    const std::string_view remaining = input.substr(pos);
    const usize crlf = find_crlf(remaining);
    if (crlf == std::string_view::npos) {
        return false;  // \r\n not yet received; Incomplete
    }
    out_line = std::string(remaining.substr(0, crlf));
    pos += crlf + 2;  // Advance past the line content + '\r' + '\n'
    return true;
}

// --------------------------------------------------------------------------
// parse_simple — parse +Simple String, -Error, or :Integer
// --------------------------------------------------------------------------
ParseResult RespParser::parse_simple(std::string_view input, RespValue& out, usize& pos) {
    if (pos >= input.size()) return ParseResult::Incomplete;

    const char prefix = input[pos];
    pos++;  // Consume the type prefix byte

    std::string line;
    if (!parse_line(input, pos, line)) {
        return ParseResult::Incomplete;
    }

    if (prefix == '+') {
        out = RespValue::simple_string(std::move(line));
    } else if (prefix == '-') {
        out = RespValue::error(std::move(line));
    } else if (prefix == ':') {
        i64 value = 0;
        const auto [end_ptr, ec] = std::from_chars(line.data(), line.data() + line.size(), value);
        if (ec != std::errc{}) {
            return ParseResult::Error;  // Malformed integer
        }
        out = RespValue::integer(value);
    } else {
        return ParseResult::Error;
    }

    return ParseResult::Complete;
}

// --------------------------------------------------------------------------
// parse_bulk_string — parse $<len>\r\n<data>\r\n
// --------------------------------------------------------------------------
ParseResult RespParser::parse_bulk_string(std::string_view input, RespValue& out, usize& pos) {
    if (pos >= input.size()) return ParseResult::Incomplete;
    pos++;  // Consume '$' prefix

    // Read the length line (e.g., "5" from "$5\r\n")
    std::string len_str;
    if (!parse_line(input, pos, len_str)) return ParseResult::Incomplete;

    i64 length = 0;
    const auto [end_ptr, ec] = std::from_chars(
        len_str.data(), len_str.data() + len_str.size(), length);
    if (ec != std::errc{}) return ParseResult::Error;

    // Null bulk string: $-1\r\n
    if (length == -1) {
        out = RespValue::nil();
        return ParseResult::Complete;
    }

    if (length < 0) return ParseResult::Error;  // Invalid negative length

    // Check we have enough bytes for the payload + trailing \r\n
    const usize payload_end = pos + static_cast<usize>(length) + 2;  // +2 for \r\n
    if (payload_end > input.size()) {
        return ParseResult::Incomplete;
    }

    // Extract the bulk string payload (this is the only std::string copy — necessary
    // because the RespValue must outlive the connection read buffer)
    std::string payload(input.substr(pos, static_cast<usize>(length)));

    // Verify the trailing \r\n
    if (input[pos + static_cast<usize>(length)]     != '\r' ||
        input[pos + static_cast<usize>(length) + 1] != '\n') {
        return ParseResult::Error;
    }

    pos += static_cast<usize>(length) + 2;  // Consume payload + \r\n
    out = RespValue::bulk_string(std::move(payload));
    return ParseResult::Complete;
}

// --------------------------------------------------------------------------
// parse_array — parse *<count>\r\n<element1>...<elementN>
// --------------------------------------------------------------------------
ParseResult RespParser::parse_array(std::string_view input, RespValue& out, usize& pos) {
    if (pos >= input.size()) return ParseResult::Incomplete;
    pos++;  // Consume '*' prefix

    // Read the count line (e.g., "3" from "*3\r\n")
    std::string count_str;
    if (!parse_line(input, pos, count_str)) return ParseResult::Incomplete;

    i64 count = 0;
    const auto [end_ptr, ec] = std::from_chars(
        count_str.data(), count_str.data() + count_str.size(), count);
    if (ec != std::errc{}) return ParseResult::Error;

    // Null array: *-1\r\n
    if (count == -1) {
        out = RespValue::nil();
        return ParseResult::Complete;
    }

    if (count < 0) return ParseResult::Error;

    std::vector<RespValue> elements;
    elements.reserve(static_cast<usize>(count));

    for (i64 i = 0; i < count; ++i) {
        RespValue elem;
        ParseResult r = parse_value(input, elem, pos);
        if (r != ParseResult::Complete) {
            return r;  // Propagate Incomplete or Error from child elements
        }
        elements.push_back(std::move(elem));
    }

    out = RespValue::array(std::move(elements));
    return ParseResult::Complete;
}

// --------------------------------------------------------------------------
// parse_value — top-level recursive dispatcher (reads the type prefix byte)
// --------------------------------------------------------------------------
ParseResult RespParser::parse_value(std::string_view input, RespValue& out, usize& pos) {
    if (pos >= input.size()) return ParseResult::Incomplete;

    const char prefix = input[pos];

    switch (prefix) {
        case '+':
        case '-':
        case ':':
            return parse_simple(input, out, pos);
        case '$':
            return parse_bulk_string(input, out, pos);
        case '*':
            return parse_array(input, out, pos);
        default:
            return ParseResult::Error;  // Unknown RESP2 type byte
    }
}

// --------------------------------------------------------------------------
// parse — public entry point
// --------------------------------------------------------------------------
ParseResult RespParser::parse(std::string_view input, RespValue& out_value, usize& out_consumed) {
    usize pos = 0;
    const ParseResult result = parse_value(input, out_value, pos);
    out_consumed = (result == ParseResult::Complete) ? pos : 0;
    return result;
}

// --------------------------------------------------------------------------
// reset — clear any intermediate state (currently stateless between calls)
// --------------------------------------------------------------------------
void RespParser::reset() {
    // The current implementation is fully stateless: all state is held on
    // the call stack during recursive parsing. This method is provided as
    // a hook for future extensions (e.g., storing partial parse state across
    // calls without needing to retain the full buffer).
}

} // namespace keva::protocol
