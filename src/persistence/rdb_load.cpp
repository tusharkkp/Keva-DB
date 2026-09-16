// ==============================================================================
// src/persistence/rdb_load.cpp
//
// Purpose:
//   Implements RDB snapshot loading: reads the binary format written by
//   rdb_save.cpp and reconstructs all key-value pairs in the KevaDatabase.
//
//   Loading is always synchronous (happens before the event loop starts),
//   so there are no concurrency concerns — the database is single-threaded
//   during the load phase.
//
//   Error handling strategy:
//   If the CRC64 checksum does not match (indicating file corruption or
//   partial write), we return an error and the caller can choose to start
//   with an empty database rather than serving corrupt data.
// ==============================================================================

#include "keva/persistence/rdb_load.hpp"
#include "keva/persistence/rdb.hpp"
#include "keva/core/object.hpp"
#include "keva/common/logger.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <string>

namespace keva::persistence {

// --------------------------------------------------------------------------
// RdbReader implementation
// --------------------------------------------------------------------------
RdbReader::RdbReader(int fd) : fd_(fd) {}

static u64 g_crc64_table[256];
static bool g_crc64_init = false;

static void ensure_crc64_table() {
    if (g_crc64_init) return;
    const u64 poly = 0xad93d23594c935a9ULL;
    for (u64 i = 0; i < 256; ++i) {
        u64 c = i;
        for (int j = 0; j < 8; ++j) c = (c & 1) ? (poly ^ (c >> 1)) : (c >> 1);
        g_crc64_table[i] = c;
    }
    g_crc64_init = true;
}

void RdbReader::update_crc(const void* data, usize len) {
    ensure_crc64_table();
    const u8* p = static_cast<const u8*>(data);
    crc_state_ = ~crc_state_;
    for (usize i = 0; i < len; ++i) {
        crc_state_ = g_crc64_table[(crc_state_ ^ p[i]) & 0xFF] ^ (crc_state_ >> 8);
    }
    crc_state_ = ~crc_state_;
}

Status RdbReader::read_bytes(void* out, usize len) {
    u8* p = static_cast<u8*>(out);
    usize remaining = len;
    while (remaining > 0) {
        ssize_t n = ::read(fd_, p, remaining);
        if (n == 0) return Status::io_error("Unexpected EOF in RDB file");
        if (n < 0) {
            if (errno == EINTR) continue;
            return Status::io_error(strerror(errno));
        }
        update_crc(p, static_cast<usize>(n));
        p += n;
        remaining -= static_cast<usize>(n);
        bytes_read_ += static_cast<usize>(n);
    }
    return Status::success();
}

Status RdbReader::read_u8(u8& out)   { return read_bytes(&out, 1); }
Status RdbReader::read_u16(u16& out) { return read_bytes(&out, 2); }
Status RdbReader::read_u32(u32& out) { return read_bytes(&out, 4); }
Status RdbReader::read_i64(i64& out) { return read_bytes(&out, 8); }

Status RdbReader::read_string(std::string& out) {
    u8 first = 0;
    if (auto s = read_u8(first); !s.ok()) return s;

    usize len = 0;
    if ((first & 0xC0) == 0x00) {
        len = first & 0x3F;
    } else if ((first & 0xC0) == 0x80) {
        u8 second = 0;
        if (auto s = read_u8(second); !s.ok()) return s;
        len = (static_cast<usize>(first & 0x3F) << 8) | second;
    } else if ((first & 0xC0) == 0xC0) {
        u32 len32 = 0;
        if (auto s = read_u32(len32); !s.ok()) return s;
        len = len32;
    } else {
        return Status::proto_error("Unknown RDB string length encoding");
    }

    out.resize(len);
    return read_bytes(out.data(), len);
}

Status RdbReader::verify_crc64() {
    u64 stored_crc = 0;
    // Read CRC without updating our accumulator
    ssize_t n = ::read(fd_, &stored_crc, 8);
    if (n != 8) return Status::io_error("Failed to read CRC64 from RDB");
    if (stored_crc != crc_state_) {
        return Status::io_error("RDB CRC64 checksum mismatch — file may be corrupt");
    }
    return Status::success();
}

// --------------------------------------------------------------------------
// rdb_load — load an RDB file into a KevaDatabase
// --------------------------------------------------------------------------
Status rdb_load(core::KevaDatabase& db, std::string_view filename) {
    int fd = ::open(std::string(filename).c_str(), O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            log::info("RDB: no existing snapshot found (%.*s) — starting fresh",
                      static_cast<int>(filename.size()), filename.data());
            return Status::not_found();
        }
        return Status::io_error(strerror(errno));
    }

    log::info("RDB: loading snapshot from %.*s",
              static_cast<int>(filename.size()), filename.data());

    RdbReader r{fd};

    // Verify magic bytes
    u8 magic[4];
    if (auto s = r.read_bytes(magic, 4); !s.ok()) { ::close(fd); return s; }
    if (magic[0] != 'K' || magic[1] != 'E' || magic[2] != 'V' || magic[3] != 'A') {
        ::close(fd);
        return Status::proto_error("Not a valid Keva RDB file (wrong magic bytes)");
    }

    // Read and check version
    u16 version = 0;
    if (auto s = r.read_u16(version); !s.ok()) { ::close(fd); return s; }
    if (version > RDB_VERSION) {
        ::close(fd);
        return Status::proto_error("RDB version is newer than this server supports");
    }

    usize keys_loaded = 0;
    i64   pending_expire = -1;  // -1 means no pending expire for next key

    while (true) {
        u8 opcode = 0;
        if (auto s = r.read_u8(opcode); !s.ok()) { ::close(fd); return s; }

        if (opcode == RDB_OPCODE_EOF) break;

        if (opcode == RDB_OPCODE_DB) {
            u32 db_idx = 0;
            if (auto s = r.read_u32(db_idx); !s.ok()) { ::close(fd); return s; }
            // Phase 1: only db 0 supported
            continue;
        }

        if (opcode == RDB_OPCODE_EXPIRE) {
            if (auto s = r.read_i64(pending_expire); !s.ok()) { ::close(fd); return s; }
            // Read the actual type byte next
            if (auto s = r.read_u8(opcode); !s.ok()) { ::close(fd); return s; }
        }

        if (opcode == RDB_TYPE_STRING) {
            std::string key, value;
            if (auto s = r.read_string(key);   !s.ok()) { ::close(fd); return s; }
            if (auto s = r.read_string(value); !s.ok()) { ::close(fd); return s; }

            auto* obj = core::KevaObject::create_string(value, core::lru_clock::current());
            db.set(key, obj);

            if (pending_expire != -1) {
                db.set_expire(key, pending_expire);
                pending_expire = -1;
            }

            ++keys_loaded;
        }
        // Phase 2: handle RDB_TYPE_LIST, RDB_TYPE_SET, etc.
    }

    if (auto s = r.verify_crc64(); !s.ok()) {
        ::close(fd);
        log::error("RDB: %s", s.message().c_str());
        return s;
    }

    ::close(fd);
    log::info("RDB: loaded %zu keys from snapshot", keys_loaded);
    return Status::success();
}

} // namespace keva::persistence
