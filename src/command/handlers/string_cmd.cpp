// ==============================================================================
// src/command/handlers/string_cmd.cpp
//
// Purpose:
//   Implements all String command handlers. These are the most frequently-used
//   commands in Redis workloads (SET/GET alone account for ~70% of typical
//   Redis traffic in web application caching scenarios).
//
//   Key implementation patterns demonstrated here:
//
//   1. Type Safety: Every command that reads an existing key must check
//      obj->type() == ObjectType::String before operating. If the key exists
//      with a different type, we return a WRONGTYPE error exactly matching
//      Redis's error message (for redis-cli compatibility).
//
//   2. Integer-path INCR/DECR: The increment commands first try to treat
//      the current value as an integer (via ENCODING_INT fast path or
//      try_parse_int() on the buffer). Overflow is detected by checking
//      whether the result would exceed i64 limits before adding.
//
//   3. SET with options: The SET command parses optional sub-arguments:
//      EX <seconds>, PX <ms>, NX (only if not exists), XX (only if exists).
//      This demonstrates multi-token argument parsing from the RESP array.
//
//   4. APPEND: Must convert an INT-encoded object to a RAW Buffer before
//      appending bytes, since integers cannot be byte-appended in-place.
// ==============================================================================

#include "keva/command/handlers/string_cmd.hpp"
#include "keva/protocol/resp_encoder.hpp"
#include "keva/core/object.hpp"
#include "keva/core/db.hpp"

#include <charconv>
#include <climits>
#include <algorithm>
#include <cctype>

namespace keva::command::handlers {

using protocol::RespEncoder;
using core::KevaObject;
using core::ObjectType;
using core::ObjectEncoding;
using core::lru_clock;

// --------------------------------------------------------------------------
// register_string_commands
// --------------------------------------------------------------------------
void register_string_commands(CommandRegistry& reg) {
    reg.register_command({"SET",    handle_set,    3, -1, CMD_FLAG_WRITE});
    reg.register_command({"GET",    handle_get,    2,  2, CMD_FLAG_READONLY | CMD_FLAG_FAST});
    reg.register_command({"GETSET", handle_getset, 3,  3, CMD_FLAG_WRITE});
    reg.register_command({"MSET",   handle_mset,   3, -1, CMD_FLAG_WRITE});
    reg.register_command({"MGET",   handle_mget,   2, -1, CMD_FLAG_READONLY});
    reg.register_command({"APPEND", handle_append, 3,  3, CMD_FLAG_WRITE});
    reg.register_command({"STRLEN", handle_strlen, 2,  2, CMD_FLAG_READONLY | CMD_FLAG_FAST});
    reg.register_command({"INCR",   handle_incr,   2,  2, CMD_FLAG_WRITE | CMD_FLAG_FAST});
    reg.register_command({"INCRBY", handle_incrby, 3,  3, CMD_FLAG_WRITE | CMD_FLAG_FAST});
    reg.register_command({"DECR",   handle_decr,   2,  2, CMD_FLAG_WRITE | CMD_FLAG_FAST});
    reg.register_command({"DECRBY", handle_decrby, 3,  3, CMD_FLAG_WRITE | CMD_FLAG_FAST});
    reg.register_command({"SETEX",  handle_setex,  4,  4, CMD_FLAG_WRITE});
    reg.register_command({"PSETEX", handle_psetex, 4,  4, CMD_FLAG_WRITE});
    reg.register_command({"SETNX",  handle_setnx,  3,  3, CMD_FLAG_WRITE | CMD_FLAG_FAST});
}

// --------------------------------------------------------------------------
// Helper: get string view of a KevaObject (any encoding)
// --------------------------------------------------------------------------
static std::string get_string_value(const KevaObject* obj, char int_buf[32]) {
    if (obj->encoding() == ObjectEncoding::Int) {
        const i64 val = obj->integer_value();
        auto [end, ec] = std::to_chars(int_buf, int_buf + 32, val);
        return std::string(int_buf, end);
    }
    return std::string(obj->string_view());
}

// --------------------------------------------------------------------------
// handle_set — SET key value [EX seconds] [PX ms] [NX|XX]
// --------------------------------------------------------------------------
void handle_set(CommandContext& ctx) {
    const auto& args = ctx.cmd.array;
    // args[0] = "SET", args[1] = key, args[2] = value
    const std::string_view key = args[1].str;
    const std::string_view val = args[2].str;

    // Parse optional arguments
    i64  expire_ms = -1;
    bool nx = false, xx = false;

    for (usize i = 3; i < args.size(); ++i) {
        std::string opt = args[i].str;
        std::transform(opt.begin(), opt.end(), opt.begin(),
                       [](unsigned char c){ return std::toupper(c); });

        if (opt == "EX" && i + 1 < args.size()) {
            i64 secs = 0;
            if (auto [p, ec] = std::from_chars(args[i+1].str.data(),
                                                args[i+1].str.data() + args[i+1].str.size(),
                                                secs); ec != std::errc{} || secs <= 0) {
                RespEncoder::error(ctx.conn, "invalid expire time in 'set' command");
                return;
            } else { expire_ms = secs * 1000; }
            ++i;
        } else if (opt == "PX" && i + 1 < args.size()) {
            if (auto [p, ec] = std::from_chars(args[i+1].str.data(),
                                                args[i+1].str.data() + args[i+1].str.size(),
                                                expire_ms); ec != std::errc{} || expire_ms <= 0) {
                RespEncoder::error(ctx.conn, "invalid expire time in 'set' command");
                return;
            }
            ++i;
        } else if (opt == "NX") { nx = true; }
        else if (opt == "XX")   { xx = true; }
    }

    const bool key_exists = ctx.db.exists(key);

    if (nx && key_exists)  { RespEncoder::nil(ctx.conn);  return; }
    if (xx && !key_exists) { RespEncoder::nil(ctx.conn);  return; }

    auto* obj = KevaObject::create_string(val, lru_clock::current());
    ctx.db.set(key, obj);

    if (expire_ms > 0) {
        ctx.db.set_expire(key, core::now_milliseconds() + expire_ms);
    }

    RespEncoder::ok(ctx.conn);
}

// --------------------------------------------------------------------------
// handle_get — GET key
// --------------------------------------------------------------------------
void handle_get(CommandContext& ctx) {
    const std::string_view key = ctx.cmd.array[1].str;
    KevaObject* obj = ctx.db.get(key);

    if (!obj) {
        RespEncoder::nil(ctx.conn);
        return;
    }
    if (obj->type() != ObjectType::String) {
        RespEncoder::wrong_type(ctx.conn,
            "Operation against a key holding the wrong kind of value");
        return;
    }

    if (obj->encoding() == ObjectEncoding::Int) {
        char buf[32];
        const i64 val = obj->integer_value();
        auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), val);
        RespEncoder::bulk_string(ctx.conn, std::string_view{buf, static_cast<usize>(end - buf)});
    } else {
        RespEncoder::bulk_string(ctx.conn, obj->string_view());
    }
}

// --------------------------------------------------------------------------
// handle_getset — GETSET key value (atomic get-then-set)
// --------------------------------------------------------------------------
void handle_getset(CommandContext& ctx) {
    const std::string_view key = ctx.cmd.array[1].str;
    const std::string_view val = ctx.cmd.array[2].str;

    KevaObject* old = ctx.db.get(key);

    if (old && old->type() != ObjectType::String) {
        RespEncoder::wrong_type(ctx.conn,
            "Operation against a key holding the wrong kind of value");
        return;
    }

    // Return the old value
    if (!old) {
        RespEncoder::nil(ctx.conn);
    } else if (old->encoding() == ObjectEncoding::Int) {
        char buf[32];
        const i64 v = old->integer_value();
        auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), v);
        RespEncoder::bulk_string(ctx.conn, std::string_view{buf, static_cast<usize>(end - buf)});
    } else {
        RespEncoder::bulk_string(ctx.conn, old->string_view());
    }

    // Set the new value
    auto* new_obj = KevaObject::create_string(val, lru_clock::current());
    ctx.db.set(key, new_obj);
}

// --------------------------------------------------------------------------
// handle_mset — MSET key1 val1 key2 val2 ...
// --------------------------------------------------------------------------
void handle_mset(CommandContext& ctx) {
    const auto& args = ctx.cmd.array;
    if (args.size() % 2 != 1) {
        RespEncoder::error(ctx.conn, "wrong number of arguments for MSET");
        return;
    }
    for (usize i = 1; i < args.size(); i += 2) {
        auto* obj = KevaObject::create_string(args[i+1].str, lru_clock::current());
        ctx.db.set(args[i].str, obj);
    }
    RespEncoder::ok(ctx.conn);
}

// --------------------------------------------------------------------------
// handle_mget — MGET key1 key2 ...
// --------------------------------------------------------------------------
void handle_mget(CommandContext& ctx) {
    const auto& args = ctx.cmd.array;
    RespEncoder::array_header(ctx.conn, static_cast<i64>(args.size() - 1));

    for (usize i = 1; i < args.size(); ++i) {
        KevaObject* obj = ctx.db.get(args[i].str);
        if (!obj || obj->type() != ObjectType::String) {
            RespEncoder::nil(ctx.conn);
        } else if (obj->encoding() == ObjectEncoding::Int) {
            char buf[32];
            const i64 v = obj->integer_value();
            auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), v);
            RespEncoder::bulk_string(ctx.conn, std::string_view{buf, static_cast<usize>(end - buf)});
        } else {
            RespEncoder::bulk_string(ctx.conn, obj->string_view());
        }
    }
}

// --------------------------------------------------------------------------
// handle_append — APPEND key value
// --------------------------------------------------------------------------
void handle_append(CommandContext& ctx) {
    const std::string_view key = ctx.cmd.array[1].str;
    const std::string_view val = ctx.cmd.array[2].str;

    KevaObject* obj = ctx.db.get(key);
    if (obj && obj->type() != ObjectType::String) {
        RespEncoder::wrong_type(ctx.conn,
            "Operation against a key holding the wrong kind of value");
        return;
    }

    if (!obj) {
        // Key does not exist: create a new string
        auto* new_obj = KevaObject::create_string(val, lru_clock::current());
        ctx.db.set(key, new_obj);
        RespEncoder::integer(ctx.conn, static_cast<i64>(val.size()));
        return;
    }

    // Convert INT-encoded object to RAW for byte-level append
    std::string current;
    if (obj->encoding() == ObjectEncoding::Int) {
        char buf[32];
        const i64 v = obj->integer_value();
        auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), v);
        current = std::string(buf, end);
    } else {
        current = std::string(obj->string_view());
    }
    current.append(val);

    auto* new_obj = KevaObject::create_string(current, lru_clock::current());
    ctx.db.set(key, new_obj);
    RespEncoder::integer(ctx.conn, static_cast<i64>(current.size()));
}

// --------------------------------------------------------------------------
// handle_strlen — STRLEN key
// --------------------------------------------------------------------------
void handle_strlen(CommandContext& ctx) {
    const std::string_view key = ctx.cmd.array[1].str;
    KevaObject* obj = ctx.db.get(key);

    if (!obj) { RespEncoder::integer(ctx.conn, 0); return; }
    if (obj->type() != ObjectType::String) {
        RespEncoder::wrong_type(ctx.conn,
            "Operation against a key holding the wrong kind of value");
        return;
    }

    if (obj->encoding() == ObjectEncoding::Int) {
        char buf[32];
        const i64 v = obj->integer_value();
        auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), v);
        RespEncoder::integer(ctx.conn, static_cast<i64>(end - buf));
    } else {
        RespEncoder::integer(ctx.conn, static_cast<i64>(obj->string_view().size()));
    }
}

// --------------------------------------------------------------------------
// Helper: get integer value from a KevaObject (returns false on error)
// --------------------------------------------------------------------------
static bool get_integer_obj(KevaObject* obj, i64& out) {
    if (!obj) { out = 0; return true; }
    if (obj->type() != ObjectType::String) return false;
    if (obj->encoding() == ObjectEncoding::Int) {
        out = obj->integer_value();
        return true;
    }
    return obj->buffer()->try_parse_int(out);
}

// --------------------------------------------------------------------------
// handle_incr — INCR key
// --------------------------------------------------------------------------
void handle_incr(CommandContext& ctx) {
    const std::string_view key = ctx.cmd.array[1].str;
    KevaObject* obj = ctx.db.get(key);

    i64 current = 0;
    if (!get_integer_obj(obj, current)) {
        RespEncoder::error(ctx.conn, "value is not an integer or out of range");
        return;
    }
    if (current == LLONG_MAX) {
        RespEncoder::error(ctx.conn, "increment or decrement would overflow");
        return;
    }

    const i64 new_val = current + 1;
    auto* new_obj = KevaObject::create_string_from_int(new_val, lru_clock::current());
    ctx.db.set(key, new_obj);
    RespEncoder::integer(ctx.conn, new_val);
}

// --------------------------------------------------------------------------
// handle_incrby — INCRBY key increment
// --------------------------------------------------------------------------
void handle_incrby(CommandContext& ctx) {
    const std::string_view key = ctx.cmd.array[1].str;
    i64 increment = 0;
    if (auto [p, ec] = std::from_chars(ctx.cmd.array[2].str.data(),
                                        ctx.cmd.array[2].str.data() + ctx.cmd.array[2].str.size(),
                                        increment); ec != std::errc{}) {
        RespEncoder::error(ctx.conn, "value is not an integer or out of range");
        return;
    }

    KevaObject* obj = ctx.db.get(key);
    i64 current = 0;
    if (!get_integer_obj(obj, current)) {
        RespEncoder::error(ctx.conn, "value is not an integer or out of range");
        return;
    }

    // Overflow check
    if ((increment > 0 && current > LLONG_MAX - increment) ||
        (increment < 0 && current < LLONG_MIN - increment)) {
        RespEncoder::error(ctx.conn, "increment or decrement would overflow");
        return;
    }

    const i64 new_val = current + increment;
    ctx.db.set(key, KevaObject::create_string_from_int(new_val, lru_clock::current()));
    RespEncoder::integer(ctx.conn, new_val);
}

// --------------------------------------------------------------------------
// handle_decr — DECR key
// --------------------------------------------------------------------------
void handle_decr(CommandContext& ctx) {
    const std::string_view key = ctx.cmd.array[1].str;
    KevaObject* obj = ctx.db.get(key);
    i64 current = 0;
    if (!get_integer_obj(obj, current)) {
        RespEncoder::error(ctx.conn, "value is not an integer or out of range");
        return;
    }
    if (current == LLONG_MIN) {
        RespEncoder::error(ctx.conn, "increment or decrement would overflow");
        return;
    }
    const i64 new_val = current - 1;
    ctx.db.set(key, KevaObject::create_string_from_int(new_val, lru_clock::current()));
    RespEncoder::integer(ctx.conn, new_val);
}

// --------------------------------------------------------------------------
// handle_decrby — DECRBY key decrement
// --------------------------------------------------------------------------
void handle_decrby(CommandContext& ctx) {
    const std::string_view key = ctx.cmd.array[1].str;
    i64 decrement = 0;
    if (auto [p, ec] = std::from_chars(ctx.cmd.array[2].str.data(),
                                        ctx.cmd.array[2].str.data() + ctx.cmd.array[2].str.size(),
                                        decrement); ec != std::errc{}) {
        RespEncoder::error(ctx.conn, "value is not an integer or out of range");
        return;
    }

    KevaObject* obj = ctx.db.get(key);
    i64 current = 0;
    if (!get_integer_obj(obj, current)) {
        RespEncoder::error(ctx.conn, "value is not an integer or out of range");
        return;
    }

    const i64 new_val = current - decrement;
    ctx.db.set(key, KevaObject::create_string_from_int(new_val, lru_clock::current()));
    RespEncoder::integer(ctx.conn, new_val);
}

// --------------------------------------------------------------------------
// handle_setex — SETEX key seconds value
// --------------------------------------------------------------------------
void handle_setex(CommandContext& ctx) {
    const std::string_view key  = ctx.cmd.array[1].str;
    const std::string_view secs = ctx.cmd.array[2].str;
    const std::string_view val  = ctx.cmd.array[3].str;

    i64 seconds = 0;
    if (auto [p, ec] = std::from_chars(secs.data(), secs.data() + secs.size(), seconds);
        ec != std::errc{} || seconds <= 0) {
        RespEncoder::error(ctx.conn, "invalid expire time in 'setex' command");
        return;
    }

    auto* obj = KevaObject::create_string(val, lru_clock::current());
    ctx.db.set(key, obj);
    ctx.db.set_expire(key, core::now_milliseconds() + seconds * 1000);
    RespEncoder::ok(ctx.conn);
}

// --------------------------------------------------------------------------
// handle_psetex — PSETEX key milliseconds value
// --------------------------------------------------------------------------
void handle_psetex(CommandContext& ctx) {
    const std::string_view key = ctx.cmd.array[1].str;
    i64 ms = 0;
    if (auto [p, ec] = std::from_chars(ctx.cmd.array[2].str.data(),
                                        ctx.cmd.array[2].str.data() + ctx.cmd.array[2].str.size(),
                                        ms); ec != std::errc{} || ms <= 0) {
        RespEncoder::error(ctx.conn, "invalid expire time in 'psetex' command");
        return;
    }

    auto* obj = KevaObject::create_string(ctx.cmd.array[3].str, lru_clock::current());
    ctx.db.set(key, obj);
    ctx.db.set_expire(key, core::now_milliseconds() + ms);
    RespEncoder::ok(ctx.conn);
}

// --------------------------------------------------------------------------
// handle_setnx — SETNX key value (set if not exists)
// --------------------------------------------------------------------------
void handle_setnx(CommandContext& ctx) {
    const std::string_view key = ctx.cmd.array[1].str;
    const std::string_view val = ctx.cmd.array[2].str;

    if (ctx.db.exists(key)) {
        RespEncoder::integer(ctx.conn, 0);
        return;
    }

    auto* obj = KevaObject::create_string(val, lru_clock::current());
    ctx.db.set(key, obj);
    RespEncoder::integer(ctx.conn, 1);
}

} // namespace keva::command::handlers
