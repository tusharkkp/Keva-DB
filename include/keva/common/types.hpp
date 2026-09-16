// ==============================================================================
// keva/common/types.hpp
//
// Purpose:
//   Defines the fundamental, project-wide primitive type aliases and constants
//   used across every Keva subsystem (networking, storage, protocol, persistence).
//
//   Using explicit stdint-width aliases (e.g. u8, u64, i64) prevents the subtle
//   platform-dependent behaviour of 'int', 'long', etc. — critical when dealing
//   with network byte ordering, binary serialization (RDB format), and
//   bitfield layout inside KevaObject headers.
//
//   Also defines the KevaByte type (uint8_t alias) used throughout the raw
//   buffer and socket I/O code as the canonical single-byte type.
// ==============================================================================

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>

namespace keva {

// --------------------------------------------------------------------------
// Integer type aliases — explicit widths for deterministic layout
// --------------------------------------------------------------------------
using u8  = std::uint8_t;    // Unsigned  8-bit (raw byte)
using u16 = std::uint16_t;   // Unsigned 16-bit
using u32 = std::uint32_t;   // Unsigned 32-bit (hash values, lengths)
using u64 = std::uint64_t;   // Unsigned 64-bit (timestamps, memory sizes)

using i8  = std::int8_t;     // Signed  8-bit
using i16 = std::int16_t;    // Signed 16-bit
using i32 = std::int32_t;    // Signed 32-bit
using i64 = std::int64_t;    // Signed 64-bit (Redis-compatible integer values)

using usize = std::size_t;   // Platform pointer-width unsigned (array sizes, offsets)

// --------------------------------------------------------------------------
// Byte alias — used for raw socket I/O buffers and binary RDB serialization
// --------------------------------------------------------------------------
using KevaByte = u8;

// --------------------------------------------------------------------------
// Time alias — all timestamps in Keva are in milliseconds since Unix epoch
// (consistent with Redis PEXPIRE / PTTL semantics)
// --------------------------------------------------------------------------
using MillisecondTimestamp = i64;

// --------------------------------------------------------------------------
// Constants
// --------------------------------------------------------------------------
namespace constants {

    // Default TCP port Keva listens on (same as Redis for drop-in redis-cli use)
    inline constexpr u16 DEFAULT_PORT = 6379;

    // Default maximum number of concurrent client connections
    inline constexpr u32 DEFAULT_MAX_CLIENTS = 10000;

    // Size of the per-connection read buffer (16 KiB)
    // Sized to accommodate a large RESP2 array command with bulk string args.
    inline constexpr usize READ_BUFFER_SIZE = 16 * 1024;

    // Size of the per-connection write (output) buffer (64 KiB)
    inline constexpr usize WRITE_BUFFER_SIZE = 64 * 1024;

    // TCP listen backlog: max number of pending un-accepted connections
    // in the OS kernel accept queue before new connections are refused.
    inline constexpr int TCP_BACKLOG = 511;

    // Active key expiry cron tick rate (times per second)
    // The expiry scan runs at 10 Hz — matching Redis's default hz=10.
    inline constexpr int SERVER_HZ = 10;

    // Number of random keys to sample per expiry cron tick per database
    inline constexpr int ACTIVE_EXPIRE_SAMPLE_SIZE = 20;

    // If more than this percentage of sampled keys are expired, repeat the cycle
    inline constexpr int ACTIVE_EXPIRE_CYCLE_LOOKUPS_PER_LOOP = 20;

    // Approximated LRU eviction pool size (number of candidate keys tracked)
    inline constexpr int LRU_POOL_SIZE = 16;

    // LRU clock resolution in seconds (24-bit clock wraps every ~194 days)
    inline constexpr u32 LRU_CLOCK_MAX = (1 << 24);

    // Embedded string maximum byte length (strings <= this use single allocation)
    // Chosen so that KevaObject (16 bytes) + embstr header (3 bytes) + data + '\0'
    // all fit within two 64-byte CPU cache lines (128 bytes total).
    inline constexpr usize EMBSTR_MAX_LEN = 44;

    // Dict load factor threshold for resizing (expand when load > 1.0)
    // Shrink when load < 0.1 (10%) to reclaim memory after mass deletions.
    inline constexpr float DICT_EXPAND_RATIO  = 1.0f;
    inline constexpr float DICT_SHRINK_RATIO  = 0.1f;

} // namespace constants

} // namespace keva
