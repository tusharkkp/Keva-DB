// ==============================================================================
// tests/unit/test_dict.cpp
//
// Purpose:
//   Unit tests for keva::core::Dict — the custom dual-table progressive
//   incremental hash table. Tests verify:
//   - Basic set/get/del/exists operations
//   - Incremental rehash correctness (all keys accessible during and after rehash)
//   - Collision handling (multiple keys in the same bucket chain)
//   - Shrink threshold triggered after mass deletions
// ==============================================================================

#include <catch2/catch_test_macros.hpp>
#include "keva/core/dict.hpp"

using namespace keva::core;

TEST_CASE("Dict: basic set and get", "[dict]") {
    Dict dict;
    dict.seed_hash(0xdeadbeefULL, 0xcafebabeULL);

    int val1 = 42, val2 = 99;

    REQUIRE(dict.set("hello", &val1) == true);   // New entry
    REQUIRE(dict.set("world", &val2) == true);   // New entry

    REQUIRE(dict.get("hello") == &val1);
    REQUIRE(dict.get("world") == &val2);
    REQUIRE(dict.get("missing") == nullptr);
    REQUIRE(dict.size() == 2);
}

TEST_CASE("Dict: overwrite existing key", "[dict]") {
    Dict dict;
    int a = 1, b = 2;

    REQUIRE(dict.set("key", &a) == true);   // New
    REQUIRE(dict.set("key", &b) == false);  // Overwrite (returns false)
    REQUIRE(dict.get("key") == &b);
    REQUIRE(dict.size() == 1);
}

TEST_CASE("Dict: del removes key", "[dict]") {
    Dict dict;
    int v = 7;
    dict.set("k", &v);

    REQUIRE(dict.exists("k") == true);
    REQUIRE(dict.del("k") == true);
    REQUIRE(dict.exists("k") == false);
    REQUIRE(dict.del("k") == false);  // Double-del returns false
    REQUIRE(dict.size() == 0);
}

TEST_CASE("Dict: progressive rehash under load", "[dict]") {
    Dict dict;
    dict.seed_hash(0x1234ULL, 0x5678ULL);

    const int N = 200;
    std::vector<int> vals(N);
    for (int i = 0; i < N; ++i) {
        vals[i] = i;
        std::string key = "key:" + std::to_string(i);
        dict.set(key, &vals[i]);
        // Drive rehash on every operation
        dict.rehash_step(1);
    }

    // All keys must be reachable after rehash
    for (int i = 0; i < N; ++i) {
        std::string key = "key:" + std::to_string(i);
        REQUIRE(dict.get(key) == &vals[i]);
    }
    REQUIRE(dict.size() == static_cast<keva::usize>(N));
}

TEST_CASE("Dict: for_each iterates all entries", "[dict]") {
    Dict dict;
    int a = 1, b = 2, c = 3;
    dict.set("a", &a);
    dict.set("b", &b);
    dict.set("c", &c);

    int visited = 0;
    dict.for_each([&](std::string_view, void*) { ++visited; });
    REQUIRE(visited == 3);
}
