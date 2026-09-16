// ==============================================================================
// keva/persistence/rdb.hpp
//
// Purpose:
//   Defines the binary RDB (Redis Database) file format constants and
//   serialization primitives for the Keva snapshot persistence engine.
//
//   RDB File Format (custom binary format inspired by Redis RDB):
//
//   +-----------------------------------+
//   | Magic: "KEVA" (4 bytes)           |
//   | Version: u16 (2 bytes)            |
//   +-----------------------------------+
//   | DB Selector opcode (1 byte: 0xFE) |
//   | DB Index: u32 (4 bytes)           |
//   +-----------------------------------+
//   | For each key-value pair:          |
//   |   Type byte (1 byte)              |
//   |   [Expire opcode (0xFC) + i64ms]  | (optional, only if key has TTL)
//   |   Key: length-prefixed string     |
//   |   Value: type-dependent encoding  |
//   +-----------------------------------+
//   | EOF opcode (1 byte: 0xFF)         |
//   | CRC64 checksum (8 bytes)          |
//   +-----------------------------------+
//
//   Length-prefixed strings:
//   The key and string values are serialized as:
//     [length: varint (1-5 bytes)][raw bytes]
//   A varint uses the MSBs of the first byte to indicate if more bytes follow:
//     0xxxxxxx : length fits in 6 bits (0-63)
//     10xxxxxx xx : length fits in 14 bits (63-16383)
//     11000000 + 4 bytes: full 32-bit length
//   This encoding matches Redis's RDB string encoding for interoperability.
// ==============================================================================

#pragma once

#include "keva/common/types.hpp"
#include "keva/common/status.hpp"

namespace keva::persistence {

// --------------------------------------------------------------------------
// RDB file format constants
// --------------------------------------------------------------------------
constexpr u8  RDB_MAGIC[]        = {'K', 'E', 'V', 'A'};  // File magic bytes
constexpr u16 RDB_VERSION        = 1;                       // Keva RDB version

constexpr u8  RDB_OPCODE_DB      = 0xFE;  // Selects which database follows
constexpr u8  RDB_OPCODE_EXPIRE  = 0xFC;  // Next key has a millisecond expiry
constexpr u8  RDB_OPCODE_EOF     = 0xFF;  // End of RDB data; CRC64 follows

// Value type bytes (written before each key-value pair)
constexpr u8  RDB_TYPE_STRING    = 0x00;
// Phase 2 type bytes (reserved for future collection types):
// constexpr u8 RDB_TYPE_LIST    = 0x01;
// constexpr u8 RDB_TYPE_SET     = 0x02;
// constexpr u8 RDB_TYPE_ZSET    = 0x03;
// constexpr u8 RDB_TYPE_HASH    = 0x04;

// --------------------------------------------------------------------------
// RdbWriter — sequential binary stream writer (writes to an int fd)
// --------------------------------------------------------------------------
class RdbWriter {
public:
    explicit RdbWriter(int fd);

    // Write raw bytes to the file
    Status write_bytes(const void* data, usize len);

    // Write a single byte
    Status write_u8(u8 value);

    // Write a 16-bit unsigned integer (little-endian)
    Status write_u16(u16 value);

    // Write a 32-bit unsigned integer (little-endian)
    Status write_u32(u32 value);

    // Write a 64-bit signed integer (little-endian)
    Status write_i64(i64 value);

    // Write a length-prefixed string (varint length + raw bytes)
    Status write_string(std::string_view sv);

    // Returns the number of bytes written so far (for CRC calculation)
    [[nodiscard]] usize bytes_written() const noexcept { return bytes_written_; }

    // Finalize: write CRC64 checksum of all preceding bytes
    Status write_crc64();

private:
    int    fd_;
    usize  bytes_written_ = 0;
    u64    crc_state_      = 0;

    // Update CRC accumulator with new bytes
    void update_crc(const void* data, usize len);
};

// --------------------------------------------------------------------------
// RdbReader — sequential binary stream reader
// --------------------------------------------------------------------------
class RdbReader {
public:
    explicit RdbReader(int fd);

    Status read_bytes(void* out, usize len);
    Status read_u8(u8& out);
    Status read_u16(u16& out);
    Status read_u32(u32& out);
    Status read_i64(i64& out);
    Status read_string(std::string& out);  // Allocates; only used at startup load

    // Verify the trailing CRC64
    Status verify_crc64();

private:
    int   fd_;
    usize bytes_read_ = 0;
    u64   crc_state_  = 0;

    void update_crc(const void* data, usize len);
};

} // namespace keva::persistence
