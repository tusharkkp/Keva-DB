// ==============================================================================
// keva/persistence/rdb_load.hpp
// Purpose: Declares the RDB snapshot loader used at server startup to restore
//          the database from a previously saved dump.rdb file.
// ==============================================================================
#pragma once
#include "keva/core/db.hpp"
#include "keva/common/status.hpp"
#include <string_view>

namespace keva::persistence {

// Load an RDB file into the given database.
// Returns Status::ok() on success. On failure, the database may be partially
// populated — the caller should flush and re-initialize if a load error occurs.
// Returns Status::not_found() if the file does not exist (first startup).
Status rdb_load(core::KevaDatabase& db, std::string_view filename);

} // namespace keva::persistence
