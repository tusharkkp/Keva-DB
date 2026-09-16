// ==============================================================================
// src/protocol/resp_encoder.cpp
//
// Purpose:
//   Implements the RespEncoder — stateless RESP2 wire format serializer.
//   Writes encoded bytes directly into a Connection's write buffer to avoid
//   intermediate heap allocations.
//
//   Stack-buffer integer formatting:
//   Instead of std::to_string(n) (which heap-allocates a std::string), we use
//   std::to_chars() on a local char[32] stack buffer. For a ':' integer reply
//   like ":42\r\n", this means zero heap allocations per response — critical
//   for a high-QPS database server where INCR/DECR commands may fire thousands
//   of times per second.
// ==============================================================================

#include "keva/protocol/resp_encoder.hpp"

#include <charconv>   // std::to_chars
#include <cstring>

namespace keva::protocol {

// --------------------------------------------------------------------------
// Internal helper: write_integer_line
// Formats 'n' as decimal ASCII, followed by \r\n, into the conn write buffer.
// Uses a stack char[32] buffer; zero heap allocation.
// --------------------------------------------------------------------------
void RespEncoder::write_integer_line(net::Connection& conn, i64 n) {
    char buf[32];
    auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), n);
    conn.append_response(std::string_view{buf, static_cast<usize>(end - buf)});
    conn.append_response("\r\n");
}

// --------------------------------------------------------------------------
// ok — "+OK\r\n"
// --------------------------------------------------------------------------
void RespEncoder::ok(net::Connection& conn) {
    conn.append_response("+OK\r\n");
}

// --------------------------------------------------------------------------
// simple_string — "+<msg>\r\n"
// --------------------------------------------------------------------------
void RespEncoder::simple_string(net::Connection& conn, std::string_view msg) {
    conn.append_response("+");
    conn.append_response(msg);
    conn.append_response("\r\n");
}

// --------------------------------------------------------------------------
// error — "-ERR <msg>\r\n"
// --------------------------------------------------------------------------
void RespEncoder::error(net::Connection& conn, std::string_view msg) {
    conn.append_response("-ERR ");
    conn.append_response(msg);
    conn.append_response("\r\n");
}

// --------------------------------------------------------------------------
// wrong_type — "-WRONGTYPE <msg>\r\n"
// --------------------------------------------------------------------------
void RespEncoder::wrong_type(net::Connection& conn, std::string_view msg) {
    conn.append_response("-WRONGTYPE ");
    conn.append_response(msg);
    conn.append_response("\r\n");
}

// --------------------------------------------------------------------------
// integer — ":<value>\r\n"
// --------------------------------------------------------------------------
void RespEncoder::integer(net::Connection& conn, i64 value) {
    conn.append_response(":");
    write_integer_line(conn, value);
}

// --------------------------------------------------------------------------
// bulk_string — "$<len>\r\n<data>\r\n"
// --------------------------------------------------------------------------
void RespEncoder::bulk_string(net::Connection& conn, std::string_view data) {
    conn.append_response("$");
    write_integer_line(conn, static_cast<i64>(data.size()));
    conn.append_response(data);
    conn.append_response("\r\n");
}

// --------------------------------------------------------------------------
// nil — "$-1\r\n" (null bulk string)
// --------------------------------------------------------------------------
void RespEncoder::nil(net::Connection& conn) {
    conn.append_response("$-1\r\n");
}

// --------------------------------------------------------------------------
// array_header — "*<count>\r\n"
// Caller is responsible for encoding exactly 'count' elements afterwards.
// --------------------------------------------------------------------------
void RespEncoder::array_header(net::Connection& conn, i64 count) {
    conn.append_response("*");
    write_integer_line(conn, count);
}

// --------------------------------------------------------------------------
// nil_array — "*-1\r\n" (null array)
// --------------------------------------------------------------------------
void RespEncoder::nil_array(net::Connection& conn) {
    conn.append_response("*-1\r\n");
}

// --------------------------------------------------------------------------
// encode — recursively encode a complete RespValue (for testing/proxying)
// --------------------------------------------------------------------------
void RespEncoder::encode(net::Connection& conn, const RespValue& value) {
    switch (value.type) {
        case RespType::SimpleString:
            simple_string(conn, value.str);
            break;
        case RespType::Error:
            error(conn, value.str);
            break;
        case RespType::Integer:
            integer(conn, value.int_val);
            break;
        case RespType::BulkString:
            bulk_string(conn, value.str);
            break;
        case RespType::Nil:
            nil(conn);
            break;
        case RespType::Array:
            array_header(conn, static_cast<i64>(value.elements.size()));
            for (const auto& elem : value.elements) {
                encode(conn, elem);
            }
            break;
    }
}

} // namespace keva::protocol
