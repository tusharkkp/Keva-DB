// ==============================================================================
// keva/persistence/rdb_save.hpp
//
// Purpose:
//   Declares the RDB snapshot saving functions — both synchronous (SAVE command)
//   and background asynchronous (BGSAVE command using Linux fork() + COW).
//
//   rdb_save_sync():
//   Serializes the entire database to a temporary file, then atomically renames
//   it to the target filename. The temporary file + rename pattern ensures that
//   the database file is never left in a partially-written, corrupt state:
//   - If the server crashes mid-save, the old complete dump.rdb is still intact.
//   - Only after a successful complete write does the new file atomically replace
//     the old one via rename() (which is an atomic POSIX filesystem operation).
//
//   rdb_save_background():
//   Uses the Linux fork() syscall to create a child process that inherits a
//   point-in-time copy of the parent's virtual address space:
//   1. The parent calls fork().
//   2. The kernel creates a child process with the SAME page table entries as
//      the parent, but marks all pages as Copy-on-Write (COW).
//   3. The child writes the snapshot to disk. It sees the database state as
//      it was at the moment of fork() — a true point-in-time snapshot.
//   4. The PARENT CONTINUES SERVING REQUESTS. When the parent modifies a
//      memory page, the kernel silently duplicates just that page (COW),
//      so the child's view remains unchanged.
//   5. When the child finishes, it exits. The parent receives SIGCHLD and
//      calls waitpid() to reap the child and check its exit status.
// ==============================================================================

#pragma once

#include "keva/core/db.hpp"
#include "keva/common/status.hpp"

#include <string_view>
#include <sys/types.h>   // pid_t

namespace keva::persistence {

// Synchronous save: blocks until the entire DB is written to disk.
// Used by the SAVE command and as a fallback when fork() is unavailable.
Status rdb_save_sync(core::KevaDatabase& db, std::string_view filename);

// Background save: fork() a child process to write the snapshot.
// The child PID is written to 'out_child_pid'.
// The parent returns immediately with Status::ok() if fork() succeeded.
Status rdb_save_background(core::KevaDatabase& db,
                            std::string_view    filename,
                            pid_t&              out_child_pid);

// Called from the SIGCHLD signal handler when a BGSAVE child exits.
// Checks the child's exit status and updates last_save_time and dirty_keys.
void on_bgsave_child_exit(pid_t child_pid, int wait_status);

} // namespace keva::persistence
