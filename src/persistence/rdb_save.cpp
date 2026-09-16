// ==============================================================================
// src/persistence/rdb_save.cpp
//
// Purpose:
//   Implements the RDB snapshot persistence engine — the most educationally
//   rich file in Keva from an Operating Systems perspective.
//
//   Key OS concepts demonstrated:
//
//   1. fork() and Copy-on-Write (COW):
//      fork() is a Linux syscall that creates a child process. The child inherits
//      an exact copy of the parent's virtual memory space. However, the kernel
//      does NOT physically copy all memory pages immediately — this would be
//      prohibitively expensive for a database with gigabytes of data.
//      Instead, both parent and child initially share the same PHYSICAL RAM pages,
//      marked as read-only. When EITHER process writes to a page, the kernel
//      intercepts the write (via a page fault), physically copies the 4KB page,
//      and gives each process their own copy. This is Copy-on-Write (COW).
//      For RDB saving: the child only reads pages (to serialize them), so most
//      pages are NEVER physically copied. The child sees a perfect snapshot
//      of the database state at the exact moment of fork().
//
//   2. Atomic file replacement (write-to-temp + rename):
//      We write to "dump.rdb.tmp", then call rename("dump.rdb.tmp", "dump.rdb").
//      rename() is guaranteed by POSIX to be atomic — the old file is never
//      partially overwritten. This prevents data corruption on server crashes.
//
//   3. SIGCHLD signal handling:
//      When the BGSAVE child exits, the kernel sends SIGCHLD to the parent.
//      The parent's signal handler calls waitpid() to reap the child (preventing
//      zombie process accumulation) and checks the exit status to confirm
//      whether the save succeeded or failed.
//
//   4. CRC-64 checksum:
//      We compute a CRC-64 over all bytes written to the RDB file and append
//      it as the last 8 bytes. On load, we verify this checksum to detect
//      file corruption due to disk errors, partial writes, or bit rot.
// ==============================================================================

#include "keva/persistence/rdb_save.hpp"
#include "keva/persistence/rdb.hpp"
#include "keva/core/server_context.hpp"
#include "keva/core/object.hpp"
#include "keva/common/logger.hpp"

#include <fcntl.h>      // open(), O_WRONLY, O_CREAT, O_TRUNC
#include <unistd.h>     // write(), close(), rename(), fork()
#include <sys/wait.h>   // waitpid(), WIFEXITED, WEXITSTATUS
#include <cerrno>
#include <cstring>
#include <ctime>
#include <string>

namespace keva::persistence {

// --------------------------------------------------------------------------
// CRC-64 (Jones polynomial) — self-contained implementation
// Pre-computed lookup table approach (O(1) per byte)
// --------------------------------------------------------------------------
namespace {

static const u64 CRC64_TABLE[256] = {
    // Generated using Jones polynomial: 0xad93d23594c935a9
    // (Full 256-entry table omitted for brevity; generated at init)
    0ULL, // Filled at first use via init_crc64_table()
};

static u64 crc64_table[256];
static bool crc64_initialized = false;

static void init_crc64_table() {
    if (crc64_initialized) return;
    const u64 poly = 0xad93d23594c935a9ULL;
    for (u64 i = 0; i < 256; ++i) {
        u64 c = i;
        for (int j = 0; j < 8; ++j) {
            c = (c & 1) ? (poly ^ (c >> 1)) : (c >> 1);
        }
        crc64_table[i] = c;
    }
    crc64_initialized = true;
}

static u64 crc64_update(u64 crc, const void* data, usize len) {
    init_crc64_table();
    const u8* p = static_cast<const u8*>(data);
    crc = ~crc;
    for (usize i = 0; i < len; ++i) {
        crc = crc64_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

} // anonymous namespace

// --------------------------------------------------------------------------
// RdbWriter implementation
// --------------------------------------------------------------------------
RdbWriter::RdbWriter(int fd) : fd_(fd) {}

void RdbWriter::update_crc(const void* data, usize len) {
    crc_state_ = crc64_update(crc_state_, data, len);
}

Status RdbWriter::write_bytes(const void* data, usize len) {
    if (len == 0) return Status::ok();
    const char* p = static_cast<const char*>(data);
    usize remaining = len;
    while (remaining > 0) {
        ssize_t n = ::write(fd_, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            return Status::io_error(strerror(errno));
        }
        update_crc(p, static_cast<usize>(n));
        p += n;
        remaining -= static_cast<usize>(n);
        bytes_written_ += static_cast<usize>(n);
    }
    return Status::ok();
}

Status RdbWriter::write_u8(u8 v)   { return write_bytes(&v, 1); }
Status RdbWriter::write_u16(u16 v) { return write_bytes(&v, 2); }
Status RdbWriter::write_u32(u32 v) { return write_bytes(&v, 4); }
Status RdbWriter::write_i64(i64 v) { return write_bytes(&v, 8); }

Status RdbWriter::write_string(std::string_view sv) {
    // Varint length encoding
    const usize len = sv.size();
    if (len < 64) {
        if (auto s = write_u8(static_cast<u8>(len)); !s.ok()) return s;
    } else if (len < 16384) {
        u8 b0 = static_cast<u8>(0x80 | (len >> 8));
        u8 b1 = static_cast<u8>(len & 0xFF);
        if (auto s = write_u8(b0); !s.ok()) return s;
        if (auto s = write_u8(b1); !s.ok()) return s;
    } else {
        u8 marker = 0xC0;
        u32 len32 = static_cast<u32>(len);
        if (auto s = write_u8(marker); !s.ok()) return s;
        if (auto s = write_u32(len32); !s.ok()) return s;
    }
    return write_bytes(sv.data(), sv.size());
}

Status RdbWriter::write_crc64() {
    // Write the CRC state (NOT updating CRC with the CRC itself)
    u64 crc = crc_state_;
    ssize_t n = ::write(fd_, &crc, 8);
    if (n != 8) return Status::io_error("Failed to write CRC64");
    return Status::ok();
}

// --------------------------------------------------------------------------
// save_database_to_writer — serialize all keys in a KevaDatabase
// --------------------------------------------------------------------------
static Status save_database_to_writer(RdbWriter& w, core::KevaDatabase& db) {
    // Write DB selector
    if (auto s = w.write_u8(RDB_OPCODE_DB); !s.ok()) return s;
    if (auto s = w.write_u32(static_cast<u32>(db.db_id())); !s.ok()) return s;

    Status result = Status::ok();

    db.for_each([&](std::string_view key, core::KevaObject* obj) {
        if (!result.ok()) return;  // Short-circuit on error

        // Check if this key has an expiry
        const i64 ttl_ms = db.pttl(key);
        if (ttl_ms >= 0) {
            // Write expire opcode + absolute timestamp
            u8 opcode = RDB_OPCODE_EXPIRE;
            result = w.write_u8(opcode);
            if (!result.ok()) return;

            const i64 abs_expire = core::now_milliseconds() + ttl_ms;
            result = w.write_i64(abs_expire);
            if (!result.ok()) return;
        }

        // Write value type byte
        result = w.write_u8(RDB_TYPE_STRING);
        if (!result.ok()) return;

        // Write key
        result = w.write_string(key);
        if (!result.ok()) return;

        // Write value
        if (obj->encoding() == core::ObjectEncoding::Int) {
            // Serialize integer as decimal string
            char buf[32];
            const i64 val = obj->integer_value();
            auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), val);
            result = w.write_string(std::string_view{buf, static_cast<usize>(end - buf)});
        } else {
            result = w.write_string(obj->string_view());
        }
    });

    return result;
}

// --------------------------------------------------------------------------
// rdb_save_sync — synchronous RDB save (used by SAVE command)
// --------------------------------------------------------------------------
Status rdb_save_sync(core::KevaDatabase& db, std::string_view filename) {
    std::string tmp_path = std::string(filename) + ".tmp";

    int fd = ::open(tmp_path.c_str(),
                    O_WRONLY | O_CREAT | O_TRUNC,
                    0644);
    if (fd < 0) {
        return Status::io_error(strerror(errno));
    }

    RdbWriter w{fd};

    // Write magic + version
    if (auto s = w.write_bytes(RDB_MAGIC, 4); !s.ok()) {
        ::close(fd); return s;
    }
    if (auto s = w.write_u16(RDB_VERSION); !s.ok()) {
        ::close(fd); return s;
    }

    // Write the database
    if (auto s = save_database_to_writer(w, db); !s.ok()) {
        ::close(fd); return s;
    }

    // Write EOF opcode
    if (auto s = w.write_u8(RDB_OPCODE_EOF); !s.ok()) {
        ::close(fd); return s;
    }

    // Write CRC64
    if (auto s = w.write_crc64(); !s.ok()) {
        ::close(fd); return s;
    }

    ::close(fd);

    // Atomically replace the target file
    if (::rename(tmp_path.c_str(), std::string(filename).c_str()) < 0) {
        return Status::io_error(strerror(errno));
    }

    log::info("RDB: saved %zu keys to %.*s",
              db.dbsize(), static_cast<int>(filename.size()), filename.data());
    return Status::ok();
}

// --------------------------------------------------------------------------
// rdb_save_background — fork() a child to save asynchronously
// --------------------------------------------------------------------------
Status rdb_save_background(core::KevaDatabase& db,
                            std::string_view    filename,
                            pid_t&              out_child_pid) {
    pid_t pid = ::fork();

    if (pid < 0) {
        return Status::io_error(strerror(errno));
    }

    if (pid == 0) {
        // === CHILD PROCESS ===
        // The child sees the database state as a point-in-time snapshot via COW.
        // We perform the full synchronous save and exit.
        // Note: we must NOT touch the parent's file descriptors, mutexes,
        // or any global state that could conflict with the parent.

        Status s = rdb_save_sync(db, filename);
        if (s.ok()) {
            log::info("BGSAVE child: snapshot complete");
            _exit(0);  // Use _exit (not exit) to avoid flushing parent's stdio buffers
        } else {
            log::error("BGSAVE child: snapshot failed: %s", s.message().c_str());
            _exit(1);
        }
    }

    // === PARENT PROCESS ===
    // The parent continues serving requests immediately.
    out_child_pid = pid;
    log::info("BGSAVE: child process spawned (pid=%d)", pid);
    return Status::ok();
}

// --------------------------------------------------------------------------
// on_bgsave_child_exit — called from SIGCHLD handler to reap the child
// --------------------------------------------------------------------------
void on_bgsave_child_exit(pid_t child_pid, int wait_status) {
    auto& srv = core::ServerContext::instance();

    // Reap the child to prevent zombie accumulation
    pid_t reaped = ::waitpid(child_pid, &wait_status, WNOHANG);
    if (reaped <= 0) return;

    srv.bgsave_child_pid = -1;

    if (WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0) {
        log::info("BGSAVE: child completed successfully (pid=%d)", child_pid);
        srv.last_save_time.store(static_cast<i64>(::time(nullptr)));
        srv.dirty_keys.store(0);
    } else {
        log::error("BGSAVE: child exited with error (pid=%d, status=%d)",
                   child_pid, WEXITSTATUS(wait_status));
    }
}

} // namespace keva::persistence
