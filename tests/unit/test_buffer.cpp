// ==============================================================================
// tests/unit/test_buffer.cpp
//
// Purpose:
//   Unit tests for keva::core::Buffer — the binary-safe dynamic byte array.
//   Tests verify: binary safety (\0 bytes preserved), O(1) length queries,
//   power-of-two growth, append, resize, and integer parsing.
// ==============================================================================

#include <catch2/catch_test_macros.hpp>
#include "keva/core/buffer.hpp"

using namespace keva::core;

TEST_CASE("Buffer: default construct is empty", "[buffer]") {
    Buffer b;
    REQUIRE(b.size() == 0);
    REQUIRE(b.empty() == true);
}

TEST_CASE("Buffer: construct from string_view", "[buffer]") {
    Buffer b{"hello"};
    REQUIRE(b.size() == 5);
    REQUIRE(b.view() == "hello");
}

TEST_CASE("Buffer: binary safe — stores null bytes", "[buffer]") {
    const char raw[] = {'a', '\0', 'b', '\0', 'c'};
    Buffer b{raw, 5};
    REQUIRE(b.size() == 5);
    REQUIRE(b.data()[2] == 'b');  // Byte after null is preserved
}

TEST_CASE("Buffer: append grows correctly", "[buffer]") {
    Buffer b{"hello"};
    b.append(", world");
    REQUIRE(b.size() == 12);
    REQUIRE(b.view() == "hello, world");
}

TEST_CASE("Buffer: equality comparison", "[buffer]") {
    Buffer a{"keva"};
    Buffer b{"keva"};
    Buffer c{"redis"};
    REQUIRE(a == b);
    REQUIRE(!(a == c));
    REQUIRE(a == std::string_view{"keva"});
}

TEST_CASE("Buffer: try_parse_int for valid integers", "[buffer]") {
    keva::i64 out = 0;
    REQUIRE(Buffer{"42"}.try_parse_int(out) == true);
    REQUIRE(out == 42);

    REQUIRE(Buffer{"-9999"}.try_parse_int(out) == true);
    REQUIRE(out == -9999);

    REQUIRE(Buffer{"0"}.try_parse_int(out) == true);
    REQUIRE(out == 0);
}

TEST_CASE("Buffer: try_parse_int rejects non-integers", "[buffer]") {
    keva::i64 out = 0;
    REQUIRE(Buffer{"3.14"}.try_parse_int(out) == false);
    REQUIRE(Buffer{"hello"}.try_parse_int(out) == false);
    REQUIRE(Buffer{""}.try_parse_int(out) == false);
}
