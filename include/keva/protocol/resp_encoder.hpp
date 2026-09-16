// ==============================================================================
// keva/protocol/resp_encoder.hpp
//
// Purpose:
//   Declares the RespEncoder class — a zero-allocation RESP2 response serializer
//   that writes encoded bytes directly into a Connection's write buffer.
//
//   Why write directly into the connection buffer?
//   An alternative design would be to build a std::string response and then
//   copy it into the write buffer. This results in two memory allocations and
//   a data copy per response. Our encoder writes directly into the target buffer,
//   eliminating the intermediate allocation.
//
//   The encoder produces byte sequences in RESP2 wire format:
//   - ok()          -> "+OK\r\n"
//   - simple(s)     -> "+<s>\r\n"
//   - error(msg)    -> "-ERR <msg>\r\n"
//   - integer(n)    -> ":<n>\r\n"
//   - bulk(s)       -> "$<len>\r\n<s>\r\n"
//   - nil()         -> "$-1\r\n"
//   - array_header  -> "*<count>\r\n"  (caller then encodes each element)
//
//   All methods take a Connection reference and append to its write buffer.
//   The caller (command handler) decides whether to call flush immediately.
// ==============================================================================

#pragma once

#include "keva/net/connection.hpp"
#include "keva/protocol/resp_types.hpp"
#include "keva/common/types.hpp"

#include <string_view>

namespace keva::protocol {

// --------------------------------------------------------------------------
// RespEncoder — stateless RESP2 response serializer
// --------------------------------------------------------------------------
class RespEncoder {
public:
    // --------------------------------------------------------------------------
    // Server response primitives (writes directly into conn's write buffer)
    // --------------------------------------------------------------------------

    // "+OK\r\n"
    static void ok(net::Connection& conn);

    // "+<message>\r\n"  (for simple string replies)
    static void simple_string(net::Connection& conn, std::string_view msg);

    // "-ERR <message>\r\n"
    static void error(net::Connection& conn, std::string_view msg);

    // "-WRONGTYPE <message>\r\n"
    static void wrong_type(net::Connection& conn, std::string_view msg);

    // ":<value>\r\n"
    static void integer(net::Connection& conn, i64 value);

    // "$<len>\r\n<data>\r\n"
    static void bulk_string(net::Connection& conn, std::string_view data);

    // "$-1\r\n"  (Nil bulk string — used for missing keys)
    static void nil(net::Connection& conn);

    // "*<count>\r\n"  (array header; caller encodes each element individually)
    static void array_header(net::Connection& conn, i64 count);

    // "*-1\r\n"  (Nil array)
    static void nil_array(net::Connection& conn);

    // Encode a complete RespValue (for testing / proxying)
    static void encode(net::Connection& conn, const RespValue& value);

private:
    // Write an integer as decimal ASCII characters + \r\n into the buffer
    // (avoids std::to_string allocation by using a stack char[32] buffer)
    static void write_integer_line(net::Connection& conn, i64 n);
};

} // namespace keva::protocol
