// ==============================================================================
// src/command/handlers/key_cmd.cpp
//
// Purpose:
//   Implements key-space commands that operate on keys regardless of their
//   value type: DEL, EXISTS, TYPE, EXPIRE/PEXPIRE, TTL/PTTL, PERSIST,
//   KEYS (pattern match), RENAME, and DBSIZE.
//
//   KEYS pattern matching:
//   Redis's KEYS command supports glob patterns (*, ?, [abc]).
//   We implement a simple recursive glob matcher for the common cases:
//   '*' matches zero or more characters, '?' matches exactly one character.
//   Note: KEYS is O(N) — it scans the entire keyspace. In production, Redis
//   discourages KEYS in favor of SCAN (cursor-based iteration). We implement
//   KEYS first for simplicity; SCAN can be added as Phase 2 enhancement.
// ==============================================================================

#include "keva/command/handlers/key_cmd.hpp"
#include "keva/protocol/resp_encoder.hpp"
#include "keva/core/object.hpp"

#include <charconv>
#include <vector>
#include <string>

namespace keva::command::handlers {

using protocol::RespEncoder;
using core::ObjectType;

// --------------------------------------------------------------------------
// register_key_commands
// --------------------------------------------------------------------------
void register_key_commands(CommandRegistry& reg) {
    reg.register_command({"DEL",      handle_del,      2, -1, CMD_FLAG_WRITE});
    reg.register_command({"EXISTS",   handle_exists,   2, -1, CMD_FLAG_READONLY | CMD_FLAG_FAST});
    reg.register_command({"TYPE",     handle_type,     2,  2, CMD_FLAG_READONLY | CMD_FLAG_FAST});
    reg.register_command({"EXPIRE",   handle_expire,   3,  3, CMD_FLAG_WRITE});
    reg.register_command({"PEXPIRE",  handle_pexpire,  3,  3, CMD_FLAG_WRITE});
    reg.register_command({"EXPIREAT", handle_expireat, 3,  3, CMD_FLAG_WRITE});
    reg.register_command({"TTL",      handle_ttl,      2,  2, CMD_FLAG_READONLY | CMD_FLAG_FAST});
    reg.register_command({"PTTL",     handle_pttl,     2,  2, CMD_FLAG_READONLY | CMD_FLAG_FAST});
    reg.register_command({"PERSIST",  handle_persist,  2,  2, CMD_FLAG_WRITE});
    reg.register_command({"KEYS",     handle_keys,     2,  2, CMD_FLAG_READONLY});
    reg.register_command({"RENAME",   handle_rename,   3,  3, CMD_FLAG_WRITE});
    reg.register_command({"DBSIZE",   handle_dbsize,   1,  1, CMD_FLAG_READONLY | CMD_FLAG_FAST});
}

// --------------------------------------------------------------------------
// Helper: simple glob pattern matcher (* and ? only)
// --------------------------------------------------------------------------
static bool glob_match(std::string_view pattern, std::string_view str) {
    if (pattern.empty()) return str.empty();
    if (pattern[0] == '*') {
        // '*' matches zero or more characters
        for (usize i = 0; i <= str.size(); ++i) {
            if (glob_match(pattern.substr(1), str.substr(i))) return true;
        }
        return false;
    }
    if (str.empty()) return false;
    if (pattern[0] == '?' || pattern[0] == str[0]) {
        return glob_match(pattern.substr(1), str.substr(1));
    }
    return false;
}

// --------------------------------------------------------------------------
// handle_del — DEL key [key ...]
// --------------------------------------------------------------------------
void handle_del(CommandContext& ctx) {
    i64 deleted = 0;
    for (usize i = 1; i < ctx.cmd.elements.size(); ++i) {
        if (ctx.db.del(ctx.cmd.elements[i].str)) ++deleted;
    }
    RespEncoder::integer(ctx.conn, deleted);
}

// --------------------------------------------------------------------------
// handle_exists — EXISTS key [key ...]
// --------------------------------------------------------------------------
void handle_exists(CommandContext& ctx) {
    i64 count = 0;
    for (usize i = 1; i < ctx.cmd.elements.size(); ++i) {
        if (ctx.db.exists(ctx.cmd.elements[i].str)) ++count;
    }
    RespEncoder::integer(ctx.conn, count);
}

// --------------------------------------------------------------------------
// handle_type — TYPE key
// --------------------------------------------------------------------------
void handle_type(CommandContext& ctx) {
    const auto* obj = ctx.db.peek(ctx.cmd.elements[1].str);
    if (!obj) { RespEncoder::simple_string(ctx.conn, "none"); return; }

    switch (obj->type()) {
        case ObjectType::String: RespEncoder::simple_string(ctx.conn, "string"); break;
        case ObjectType::List:   RespEncoder::simple_string(ctx.conn, "list");   break;
        case ObjectType::Set:    RespEncoder::simple_string(ctx.conn, "set");    break;
        case ObjectType::ZSet:   RespEncoder::simple_string(ctx.conn, "zset");   break;
        case ObjectType::Hash:   RespEncoder::simple_string(ctx.conn, "hash");   break;
    }
}

// --------------------------------------------------------------------------
// handle_expire — EXPIRE key seconds
// --------------------------------------------------------------------------
void handle_expire(CommandContext& ctx) {
    const std::string_view key = ctx.cmd.elements[1].str;
    i64 seconds = 0;
    if (auto [p, ec] = std::from_chars(ctx.cmd.elements[2].str.data(),
                                        ctx.cmd.elements[2].str.data() + ctx.cmd.elements[2].str.size(),
                                        seconds); ec != std::errc{}) {
        RespEncoder::error(ctx.conn, "value is not an integer or out of range"); return;
    }
    if (!ctx.db.exists(key)) { RespEncoder::integer(ctx.conn, 0); return; }
    ctx.db.set_expire(key, core::now_milliseconds() + seconds * 1000);
    RespEncoder::integer(ctx.conn, 1);
}

// --------------------------------------------------------------------------
// handle_pexpire — PEXPIRE key milliseconds
// --------------------------------------------------------------------------
void handle_pexpire(CommandContext& ctx) {
    const std::string_view key = ctx.cmd.elements[1].str;
    i64 ms = 0;
    if (auto [p, ec] = std::from_chars(ctx.cmd.elements[2].str.data(),
                                        ctx.cmd.elements[2].str.data() + ctx.cmd.elements[2].str.size(),
                                        ms); ec != std::errc{}) {
        RespEncoder::error(ctx.conn, "value is not an integer or out of range"); return;
    }
    if (!ctx.db.exists(key)) { RespEncoder::integer(ctx.conn, 0); return; }
    ctx.db.set_expire(key, core::now_milliseconds() + ms);
    RespEncoder::integer(ctx.conn, 1);
}

// --------------------------------------------------------------------------
// handle_expireat — EXPIREAT key unix-timestamp-seconds
// --------------------------------------------------------------------------
void handle_expireat(CommandContext& ctx) {
    const std::string_view key = ctx.cmd.elements[1].str;
    i64 ts = 0;
    if (auto [p, ec] = std::from_chars(ctx.cmd.elements[2].str.data(),
                                        ctx.cmd.elements[2].str.data() + ctx.cmd.elements[2].str.size(),
                                        ts); ec != std::errc{}) {
        RespEncoder::error(ctx.conn, "value is not an integer or out of range"); return;
    }
    if (!ctx.db.exists(key)) { RespEncoder::integer(ctx.conn, 0); return; }
    ctx.db.set_expire(key, ts * 1000);  // Convert Unix seconds to milliseconds
    RespEncoder::integer(ctx.conn, 1);
}

// --------------------------------------------------------------------------
// handle_ttl — TTL key (returns remaining seconds, -1 if no expiry, -2 if missing)
// --------------------------------------------------------------------------
void handle_ttl(CommandContext& ctx) {
    const i64 pttl_ms = ctx.db.pttl(ctx.cmd.elements[1].str);
    if (pttl_ms == -2) { RespEncoder::integer(ctx.conn, -2); return; }
    if (pttl_ms == -1) { RespEncoder::integer(ctx.conn, -1); return; }
    RespEncoder::integer(ctx.conn, pttl_ms / 1000);
}

// --------------------------------------------------------------------------
// handle_pttl — PTTL key (returns remaining milliseconds)
// --------------------------------------------------------------------------
void handle_pttl(CommandContext& ctx) {
    RespEncoder::integer(ctx.conn, ctx.db.pttl(ctx.cmd.elements[1].str));
}

// --------------------------------------------------------------------------
// handle_persist — PERSIST key (remove expiry)
// --------------------------------------------------------------------------
void handle_persist(CommandContext& ctx) {
    const std::string_view key = ctx.cmd.elements[1].str;
    if (!ctx.db.exists(key)) { RespEncoder::integer(ctx.conn, 0); return; }
    ctx.db.remove_expire(key);
    RespEncoder::integer(ctx.conn, 1);
}

// --------------------------------------------------------------------------
// handle_keys — KEYS pattern (glob matching over full keyspace)
// --------------------------------------------------------------------------
void handle_keys(CommandContext& ctx) {
    const std::string_view pattern = ctx.cmd.elements[1].str;
    std::vector<std::string> matches;

    ctx.db.for_each([&](std::string_view key, core::KevaObject* /*obj*/) {
        if (glob_match(pattern, key)) {
            matches.emplace_back(key);
        }
    });

    RespEncoder::array_header(ctx.conn, static_cast<i64>(matches.size()));
    for (const auto& k : matches) {
        RespEncoder::bulk_string(ctx.conn, k);
    }
}

// --------------------------------------------------------------------------
// handle_rename — RENAME key newkey
// --------------------------------------------------------------------------
void handle_rename(CommandContext& ctx) {
    const std::string_view key    = ctx.cmd.elements[1].str;
    const std::string_view newkey = ctx.cmd.elements[2].str;

    core::KevaObject* obj = ctx.db.get(key);
    if (!obj) {
        RespEncoder::error(ctx.conn, "ERR no such key");
        return;
    }

    // We need to move the object: get the raw pointer from keys_ and reassign
    // For safety: create a new object with the same content and delete the old
    core::KevaObject* new_obj = nullptr;
    if (obj->encoding() == core::ObjectEncoding::Int) {
        new_obj = core::KevaObject::create_string_from_int(
            obj->integer_value(), core::lru_clock::current());
    } else {
        new_obj = core::KevaObject::create_string(
            obj->string_view(), core::lru_clock::current());
    }

    ctx.db.del(key);
    ctx.db.set(newkey, new_obj);
    RespEncoder::ok(ctx.conn);
}

// --------------------------------------------------------------------------
// handle_dbsize — DBSIZE
// --------------------------------------------------------------------------
void handle_dbsize(CommandContext& ctx) {
    RespEncoder::integer(ctx.conn, static_cast<i64>(ctx.db.dbsize()));
}

} // namespace keva::command::handlers
