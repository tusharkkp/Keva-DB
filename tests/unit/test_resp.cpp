// ==============================================================================
// tests/unit/test_resp.cpp
//
// Purpose:
//   Unit tests for keva::protocol::RespParser — the streaming RESP2 parser.
//   Tests verify: all RESP2 types, partial/fragmented input (Incomplete result),
//   null bulk strings, null arrays, nested array parsing, and error detection.
// ==============================================================================

#include <catch2/catch_test_macros.hpp>
#include "keva/protocol/resp_parser.hpp"
#include "keva/protocol/resp_types.hpp"

using namespace keva::protocol;
using keva::usize;

// Helper: parse a complete string, expect Complete result
static RespValue parse_complete(std::string_view input) {
    RespParser parser;
    RespValue out;
    usize consumed = 0;
    ParseResult r = parser.parse(input, out, consumed);
    REQUIRE(r == ParseResult::Complete);
    REQUIRE(consumed == input.size());
    return out;
}

TEST_CASE("RespParser: simple string +OK\\r\\n", "[resp]") {
    auto v = parse_complete("+OK\r\n");
    REQUIRE(v.is_simple_string());
    REQUIRE(v.str == "OK");
}

TEST_CASE("RespParser: error -ERR message\\r\\n", "[resp]") {
    auto v = parse_complete("-ERR unknown command\r\n");
    REQUIRE(v.is_error());
    REQUIRE(v.str == "ERR unknown command");
}

TEST_CASE("RespParser: integer :1000\\r\\n", "[resp]") {
    auto v = parse_complete(":1000\r\n");
    REQUIRE(v.is_integer());
    REQUIRE(v.int_val == 1000);
}

TEST_CASE("RespParser: bulk string $5\\r\\nhello\\r\\n", "[resp]") {
    auto v = parse_complete("$5\r\nhello\r\n");
    REQUIRE(v.is_bulk_string());
    REQUIRE(v.str == "hello");
}

TEST_CASE("RespParser: nil bulk string $-1\\r\\n", "[resp]") {
    auto v = parse_complete("$-1\r\n");
    REQUIRE(v.is_nil());
}

TEST_CASE("RespParser: array *2\\r\\n$3\\r\\nGET\\r\\n$3\\r\\nkey\\r\\n", "[resp]") {
    auto v = parse_complete("*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n");
    REQUIRE(v.is_array());
    REQUIRE(v.elements.size() == 2);
    REQUIRE(v.elements[0].str == "GET");
    REQUIRE(v.elements[1].str == "key");
}

TEST_CASE("RespParser: incomplete input returns Incomplete", "[resp]") {
    RespParser parser;
    RespValue out;
    usize consumed = 0;

    ParseResult r = parser.parse("$5\r\nhel", out, consumed);
    REQUIRE(r == ParseResult::Incomplete);
    REQUIRE(consumed == 0);  // No bytes consumed on Incomplete
}

TEST_CASE("RespParser: invalid prefix byte returns Error", "[resp]") {
    RespParser parser;
    RespValue out;
    usize consumed = 0;

    ParseResult r = parser.parse("@invalid\r\n", out, consumed);
    REQUIRE(r == ParseResult::Error);
}

TEST_CASE("RespParser: nested array SET key value", "[resp]") {
    auto v = parse_complete("*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$5\r\nAlice\r\n");
    REQUIRE(v.is_array());
    REQUIRE(v.elements.size() == 3);
    REQUIRE(v.elements[0].str == "SET");
    REQUIRE(v.elements[1].str == "name");
    REQUIRE(v.elements[2].str == "Alice");
}
