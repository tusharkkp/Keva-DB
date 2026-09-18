# Keva Database: Comprehensive Technical Documentation & Developer Reference

> **Version:** 0.1.0 (Phase 1 Baseline)  
> **Target Architecture:** Linux x86_64 / aarch64 (POSIX, WSL2, Docker)  
> **Standard:** C++20 (Strict ISO `-Wall -Wextra -Wpedantic`)  
> **Protocol:** RESP2 (Redis Serialization Protocol 2)  
> **Status:** Production-Ready Core Prototype  

---

## Table of Contents

1. [Executive Overview & Vision](#1-executive-overview--vision)
   - [What is Keva?](#what-is-keva)
   - [Core Engineering Philosophies](#core-engineering-philosophies)
   - [Systems Programming Concepts Mastered](#systems-programming-concepts-mastered)
   - [Feature Highlights at a Glance](#feature-highlights-at-a-glance)
2. [Quick Start & Setup Guide](#2-quick-start--setup-guide)
   - [System Prerequisites](#system-prerequisites)
   - [Compiling from Source (WSL2 / Linux)](#compiling-from-source-wsl2--linux)
   - [Running the Server](#running-the-server)
   - [Client Connectivity (`redis-cli`, Python, Go, Node.js)](#client-connectivity-redis-cli-python-go-nodejs)
   - [Containerized Deployment (Docker)](#containerized-deployment-docker)
   - [Running the Test Suites](#running-the-test-suites)
3. [High-Level System Architecture](#3-high-level-system-architecture)
   - [System Topology Diagram](#system-topology-diagram)
   - [Subsystem Directory Mapping](#subsystem-directory-mapping)
   - [Request Execution Pipeline](#request-execution-pipeline)
   - [Architectural Comparison: Keva vs. Redis vs. Naive Caches](#architectural-comparison-keva-vs-redis-vs-naive-caches)
4. [Networking Engine: The Single-Threaded Reactor](#4-networking-engine-the-single-threaded-reactor)
   - [Linux `epoll` I/O Multiplexing](#linux-epoll-io-multiplexing)
   - [Socket Abstractions & Non-Blocking Flags](#socket-abstractions--non-blocking-flags)
   - [Connection Lifecycle & State Machine](#connection-lifecycle--state-machine)
   - [Buffer Compaction & Zero-Copy Slicing](#buffer-compaction--zero-copy-slicing)
   - [Signal Handling via the Self-Pipe Trick](#signal-handling-via-the-self-pipe-trick)
   - [The 10Hz Server Cron (`server_cron`)](#the-10hz-server-cron-server_cron)
5. [RESP2 Protocol Engine](#5-resp2-protocol-engine)
   - [RESP2 Wire Protocol Specification](#resp2-wire-protocol-specification)
   - [Abstract Syntax Tree (`RespValue`)](#abstract-syntax-tree-respvalue)
   - [Streaming FSM Parser (`RespParser`)](#streaming-fsm-parser-respparser)
   - [Zero-Allocation Response Serialization (`RespEncoder`)](#zero-allocation-response-serialization-respencoder)
6. [Core Storage Engine & Memory Model](#6-core-storage-engine--memory-model)
   - [Dynamic Binary-Safe String Buffer (`Buffer`)](#dynamic-binary-safe-string-buffer-buffer)
   - [Universal Value Container (`KevaObject`)](#universal-value-container-kevaobject)
   - [Object Encodings: `RAW`, `INT`, `EMBSTR`](#object-encodings-raw-int-embstr)
   - [Dual-Table Incremental Hash Table (`Dict`)](#dual-table-incremental-hash-table-dict)
   - [SipHash-1-2 & HashDoS Defense](#siphash-1-2--hashdos-defense)
   - [Amortized Progressive Rehashing Mechanics](#amortized-progressive-rehashing-mechanics)
7. [Key Expiry, Eviction & Memory Management](#7-key-expiry-eviction--memory-management)
   - [The Dual-Dictionary Architecture (`keys_` vs. `expires_`)](#the-dual-dictionary-architecture-keys_-vs-expires_)
   - [Passive (Lazy) Expiration on Access](#passive-lazy-expiration-on-access)
   - [Active Probabilistic Expiration Cycle](#active-probabilistic-expiration-cycle)
   - [Approximated LRU Eviction & Candidate Pool](#approximated-lru-eviction--candidate-pool)
8. [Persistence Engine: Point-in-Time RDB Snapshots](#8-persistence-engine-point-in-time-rdb-snapshots)
   - [Snapshot Strategy & Kernel Virtual Memory](#snapshot-strategy--kernel-virtual-memory)
   - [Linux `fork()` & Copy-on-Write (COW) Deep Dive](#linux-fork--copy-on-write-cow-deep-dive)
   - [RDB Binary File Format Specification](#rdb-binary-file-format-specification)
   - [Atomic File Swapping via POSIX `rename()`](#atomic-file-swapping-via-posix-rename)
   - [Synchronous (`SAVE`) vs. Asynchronous (`BGSAVE`) Workflows](#synchronous-save-vs-asynchronous-bgsave-workflows)
   - [Cold-Start Recovery Engine (`rdb_load`)](#cold-start-recovery-engine-rdb_load)
   - [CRC-64 Jones Polynomial Integrity Checksum](#crc-64-jones-polynomial-integrity-checksum)
9. [Complete Command Reference](#9-complete-command-reference)
   - [String Commands](#string-commands)
   - [Key Space & Lifetime Commands](#key-space--lifetime-commands)
   - [Server Administration Commands](#server-administration-commands)
10. [Configuration & Operations Guide](#10-configuration--operations-guide)
    - [CLI Argument Reference](#cli-argument-reference)
    - [Logging Framework & Verbosity Levels](#logging-framework--verbosity-levels)
    - [Server Metrics via `INFO`](#server-metrics-via-info)
    - [Linux Kernel Tuning for Production](#linux-kernel-tuning-for-production)
11. [Benchmarking & Performance Profile](#11-benchmarking--performance-profile)
    - [Running `redis-benchmark`](#running-redis-benchmark)
    - [Performance Optimization Highlights](#performance-optimization-highlights)
12. [Developer Guide, Testing & Codebase Standards](#12-developer-guide-testing--codebase-standards)
    - [Architectural Code Rules & Purpose Comments](#architectural-code-rules--purpose-comments)
    - [Sanitizer Suite (ASan & UBSan)](#sanitizer-suite-asan--ubsan)
    - [Unit Testing Framework (CTest)](#unit-testing-framework-ctest)
    - [End-to-End Integration Testing with Python](#end-to-end-integration-testing-with-python)
13. [Engineering Roadmap (Phase 2 & Beyond)](#13-engineering-roadmap-phase-2--beyond)
    - [Multi-Threaded I/O Workers (Redis 6.0 Model)](#multi-threaded-io-workers-redis-60-model)
    - [Complex Collections: SkipLists, Hashes, and Lists](#complex-collections-skiplists-hashes-and-lists)
    - [Append-Only File (AOF) Persistence](#append-only-file-aof-persistence)

---

## 1. Executive Overview & Vision

### What is Keva?

**Keva** is a high-performance, Redis-inspired in-memory key-value database built from the ground up in modern **C++20**. Designed as a systems programming tour de force, Keva avoids external runtime dependencies and implements every foundational layer of a database engine by hand:

- A POSIX Linux non-blocking network reactor utilizing kernel `epoll`.
- A streaming, zero-copy RESP2 (Redis Serialization Protocol) parser and serializer.
- A binary-safe dynamic byte array (`Buffer`) safe against null-byte truncations.
- A 16-byte packed universal value container (`KevaObject`) supporting multi-encoding optimization and 24-bit LRU clock tracking.
- An incremental, dual-table progressive hash table (`Dict`) offering $O(1)$ operations with zero stop-the-world rehash latency spikes.
- A snapshot persistence subsystem (`RDB`) leveraging Linux `fork()` virtual memory Copy-on-Write (COW) mechanics and atomic file swapping.

Keva is 100% wire-compatible with standard Redis clients: you can immediately point `redis-cli`, `redis-benchmark`, or any Redis client library (Python, Go, Node.js, Rust, Java) to Keva without modification.

### Core Engineering Philosophies

1. **Zero-Dependency Core**:  
   No third-party libraries (no Boost, no libevent, no hiredis) are used for server runtime execution. Every line of networking, data structuring, protocol parsing, and binary storage is written natively.
2. **Mechanical Sympathy with the Linux Kernel**:  
   Keva leverages Linux-specific systems primitives: `epoll_create1`, `epoll_ctl`, `epoll_wait`, `O_NONBLOCK`, `TCP_NODELAY`, `pipe2`, `fork()`, `waitpid(WNOHANG)`, and Copy-on-Write memory virtualization.
3. **Deterministic & Cache-Conscious Memory**:  
   Memory layouts are tightly packed. Headers use bitfields to pack object metadata into 32 bits. Short strings and 64-bit integers bypass separate heap allocations entirely.
4. **Predictable Low Latency (No Stop-the-World Freezes)**:  
   Hash table expansion and shrinking are amortized over incoming queries and background cron ticks. Database clients never experience latency spikes when the keyspace scales.

### Systems Programming Concepts Mastered

Building and operating Keva touches essential computer science and operating systems fundamentals:

```
+-----------------------------------------------------------------------------------------+
|                                  SYSTEMS CONCEPTS IN KEVA                               |
+---------------------------+-----------------------------+-------------------------------+
|     Computer Networks     |      Operating Systems      |   Data Structures & Memory    |
+---------------------------+-----------------------------+-------------------------------+
| - TCP Streaming Semantics | - POSIX Non-Blocking I/O    | - Amortized Hash Tables       |
| - epoll Reactor Pattern   | - Linux fork() COW Memory   | - Binary-Safe Buffers (SDS)   |
| - Framing & Tokenization  | - Self-Pipe Signal Delivery | - 16-Byte Packed Bitfields    |
| - TCP_NODELAY (Nagle Off) | - Zombie Process Reaping    | - SipHash-1-2 HashDoS Defense |
| - Partial Read/Write FSM  | - Atomic File Replacement   | - Approximated LRU Sampling   |
+---------------------------+-----------------------------+-------------------------------+
```

### Feature Highlights at a Glance

- **Protocol Compatibility**: 100% RESP2 compliance for Strings, Keys, Server management, and Error reporting.
- **Binary-Safe Strings**: Keys and values can contain arbitrary byte sequences (including `\0`, UTF-8 emojis, and binary payloads).
- **Embedded String Optimization (`EMBSTR`)**: Strings $\le 44$ bytes are allocated in the same memory block as the object header.
- **Integer Direct Encoding (`INT`)**: Integers in the range $[-2^{63}, 2^{63}-1]$ are stored directly in the pointer field with zero heap allocation.
- **Progressive Incremental Rehashing**: Dual hash tables (`ht[0]` and `ht[1]`) migrate bucket-by-bucket during queries and cron ticks.
- **Dual Expiry Dictionary**: Keys with TTL are stored in an auxiliary dictionary; persistent keys incur zero memory overhead.
- **Dual-Path Expiry Engine**: Passive (lazy check on query) + Active (10Hz probabilistic sampling) eviction.
- **Non-Blocking Persistence (`BGSAVE`)**: Point-in-time RDB snapshots using kernel Copy-on-Write memory cloning.
- **Integrity Verification**: End-to-end CRC-64 checksum generation and validation using the Jones polynomial.

---

## 2. Quick Start & Setup Guide

### System Prerequisites

- **Operating System**: Linux (Ubuntu 22.04 LTS / 24.04 LTS recommended), WSL2 on Windows, or Docker.
- **Compiler**: GCC 12+ or Clang 15+ with complete C++20 support.
- **Build System**: CMake 3.20 or newer, and Ninja build tool.
- **Client Tools**: `redis-tools` (provides `redis-cli` and `redis-benchmark`), Python 3.8+ (for integration tests), and `netcat`.

In Ubuntu or WSL2, install the toolchain via:

```bash
sudo apt-get update
sudo apt-get install -y build-essential gcc-12 g++-12 cmake ninja-build redis-tools netcat python3 python3-pip git
```

### Compiling from Source (WSL2 / Linux)

Navigate to the project root and select your desired build profile:

#### Option A: Debug Build (Recommended for Development & Testing)
Includes comprehensive symbols (`-g3 -O0`), `AddressSanitizer` (ASan) to catch memory safety violations, and `UndefinedBehaviorSanitizer` (UBSan).

```bash
cd /mnt/c/PROJECTS/Keva
cmake -B build -DCMAKE_BUILD_TYPE=Debug -G Ninja
cmake --build build --parallel
```

#### Option B: Release Build (Production Benchmarks)
Enables aggressive optimizations (`-O3`), inlining, and Link-Time Optimization (LTO):

```bash
cd /mnt/c/PROJECTS/Keva
cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build build --parallel
```

### Running the Server

Launch the compiled executable directly:

```bash
./build/src/keva-server --host 127.0.0.1 --port 6379 --loglevel info
```

#### CLI Configuration Flags

| Parameter | Type | Default | Description |
|---|---|---|---|
| `--host`, `-h` | string | `0.0.0.0` | Network IP interface to bind listener |
| `--port`, `-p` | integer | `6379` | TCP port to accept client connections |
| `--loglevel` | string | `info` | Logging verbosity: `debug`, `info`, `warn`, `error` |
| `--maxmemory` | integer | `0` (unlimited) | Memory ceiling in bytes before approximated LRU kicks in |
| `--rdb-filename` | string | `dump.rdb` | Path to the snapshot database file |
| `--no-rdb` | flag | `false` | Disables automated snapshot loading and saving |
| `--help` | flag | - | Prints command-line argument usage guide |

### Client Connectivity (`redis-cli`, Python, Go, Node.js)

Since Keva implements the standard Redis wire protocol (RESP2), you interact with it using existing clients.

#### 1. Official CLI (`redis-cli`)
```bash
redis-cli -p 6379 PING
# Returns: PONG

redis-cli -p 6379 SET user:100 "Alice"
# Returns: OK

redis-cli -p 6379 GET user:100
# Returns: "Alice"

redis-cli -p 6379 INCR counter
# Returns: (integer) 1

redis-cli -p 6379 SETEX temp_session 60 "active"
# Returns: OK

redis-cli -p 6379 TTL temp_session
# Returns: (integer) 59
```

#### 2. Python (`redis-py`)
```python
import redis

client = redis.Redis(host='127.0.0.1', port=6379, decode_responses=True)

# Ping server
assert client.ping() is True

# Key-value operations
client.set("framework", "Keva")
print(client.get("framework"))  # Output: Keva

# Arithmetic operations
client.set("views", 10)
client.incrby("views", 5)
print(client.get("views"))      # Output: 15

# Multi-key & TTL
client.mset({"k1": "val1", "k2": "val2"})
client.expire("k1", 120)
print(client.ttl("k1"))         # Output: ~120
```

#### 3. Go (`go-redis`)
```go
package main

import (
    "context"
    "fmt"
    "github.com/redis/go-redis/v9"
)

func main() {
    ctx := context.Background()
    rdb := redis.NewClient(&redis.Options{
        Addr: "localhost:6379",
    })

    err := rdb.Set(ctx, "cluster_node", "node-alpha", 0).Err()
    if err != nil { panic(err) }

    val, _ := rdb.Get(ctx, "cluster_node").Result()
    fmt.Println("cluster_node:", val)
}
```

#### 4. Node.js (`ioredis`)
```javascript
const Redis = require("ioredis");
const redis = new Redis(6379, "127.0.0.1");

async function run() {
  await redis.set("session:token", "xyz_987", "EX", 300);
  const val = await redis.get("session:token");
  console.log("Token:", val);
  const ttl = await redis.ttl("session:token");
  console.log("TTL:", ttl);
  redis.disconnect();
}
run();
```

#### 5. Raw Netcat (`nc`)
```bash
printf "*3\r\n\$3\r\nSET\r\n\$4\r\nname\r\n\$4\r\nKeva\r\n" | nc 127.0.0.1 6379
# Returns: +OK\r\n

printf "*2\r\n\$3\r\nGET\r\n\$4\r\nname\r\n" | nc 127.0.0.1 6379
# Returns: $4\r\nKeva\r\n
```

### Containerized Deployment (Docker)

Keva includes an optimized multi-stage `Dockerfile`:
- **Stage 1 (Builder)**: Ubuntu 22.04 with GCC-12, Ninja, and CMake; compiles a release binary with `-O3` and LTO.
- **Stage 2 (Runtime)**: Ultra-minimal Ubuntu runtime containing only shared C++ libraries (`libstdc++6`), a dedicated non-root `keva` service user, port 6379 exposed, and a built-in healthcheck.

```bash
# Build the production Docker image
docker build -t keva:latest .

# Run Keva in a background container with port mapping
docker run -d --name keva-db -p 6379:6379 -v keva_data:/var/lib/keva keva:latest

# Check container health and logs
docker ps
docker logs keva-db

# Test ping from the host
redis-cli -p 6379 PING
```

### Running the Test Suites

#### 1. Unit Test Suite (CTest)
Tests the isolated subsystems (Hash Table incremental rehashing, Buffer power-of-two growth, and RESP2 streaming tokenization) built with AddressSanitizer and UBSan:

```bash
cd build
ctest --output-on-failure
```

#### 2. End-to-End Integration Test Suite
Executes end-to-end regression tests across sockets, testing command arithmetic, UTF-8 safety, TTL countdowns, and RDB snapshot lifecycle:

```bash
python3 tests/integration/test_redis_cli.py
```

---

## 3. High-Level System Architecture

### System Topology Diagram

```
+----------------------------------------------------------------------------------------------------+
|                                           CLIENT LAYER                                             |
|                     (redis-cli, redis-benchmark, Python, Go, Node.js, SDKs)                       |
+-------------------------------------------------+--------------------------------------------------+
                                                  | TCP Port 6379
                                                  v
+----------------------------------------------------------------------------------------------------+
|                                      KEVA NETWORKING REACTOR                                       |
|                                                                                                    |
|  +----------------------------------------------------------------------------------------------+  |
|  | Socket Layer: Non-blocking listener (SO_REUSEADDR, TCP_NODELAY, SO_KEEPALIVE)               |  |
|  +----------------------------------------------+-----------------------------------------------+  |
|                                                 |                                                  |
|  +----------------------------------------------v-----------------------------------------------+  |
|  | Event Loop (keva::net::EventLoop): Linux epoll_wait (EPOLLIN, EPOLLOUT, Level-Triggered)    |  |
|  | - Dispatches I/O callbacks to Client Connections                                            |  |
|  | - Self-Pipe trick for signal handling (SIGTERM, SIGINT, SIGCHLD)                             |  |
|  | - 10Hz Server Cron Timer (active expiry, incremental rehash, save points)                   |  |
|  +----------------------------------------------+-----------------------------------------------+  |
|                                                 |                                                  |
|  +----------------------------------------------v-----------------------------------------------+  |
|  | Connection Buffer Engine (keva::net::Connection):                                            |  |
|  | - Streaming read buffer with cursor-based O(1) compaction                                    |  |
|  | - Non-blocking write buffer handling partial writes (EAGAIN loop)                            |  |
|  +----------------------------------------------+-----------------------------------------------+  |
|                                                 |                                                  |
|  +----------------------------------------------v-----------------------------------------------+  |
|  | RESP2 Streaming Protocol Parser (keva::protocol::RespParser):                               |  |
|  | - Zero-copy string_view scanning; parses fragmented packets (*, $, +, -, :)                 |  |
|  +----------------------------------------------+-----------------------------------------------+  |
+-------------------------------------------------|--------------------------------------------------+
                                                  | Parsed Command AST (RespValue Array)
                                                  v
+----------------------------------------------------------------------------------------------------+
|                                    COMMAND DISPATCH & ROUTER                                       |
|  +----------------------------------------------------------------------------------------------+  |
|  | CommandRegistry: O(1) hash map lookup on uppercase command name                             |  |
|  | Arity bounds enforcement (arity_min, arity_max)                                               |  |
|  | CommandContext: binds RespValue, Connection (output), and KevaDatabase                       |  |
|  +----------------------------------------------+-----------------------------------------------+  |
+-------------------------------------------------|--------------------------------------------------+
                                                  | Typed Handler Invocation
                                                  v
+----------------------------------------------------------------------------------------------------+
|                                     CORE STORAGE ENGINE                                            |
|                                                                                                    |
|  +----------------------------------------------------------------------------------------------+  |
|  | Primary Key-Space (keva::core::Dict keys_):                                                  |  |
|  | - Dual hash tables (ht[0], ht[1]) with SipHash-1-2 (randomized 128-bit key)                  |  |
|  | - Amortized progressive rehashing (1 bucket per operation + cron steps)                      |  |
|  +----------------------------------------------+-----------------------------------------------+  |
|                                                 |                                                  |
|  +----------------------------------------------v-----------------------------------------------+  |
|  | Universal Object Model (keva::core::KevaObject):                                            |  |
|  | - 16-byte packed header: [Type: 4b | Encoding: 4b | LRU Clock: 24b | Refcount: 32b]         |  |
|  | - Payload pointer (ptr):                                                                     |  |
|  |   - RAW: Dynamic heap-allocated Buffer                                                       |  |
|  |   - INT: Direct 64-bit integer cast (zero heap allocation)                                   |  |
|  |   - EMBSTR: Single allocation header + byte buffer (<= 44 bytes)                             |  |
|  +----------------------------------------------+-----------------------------------------------+  |
|                                                 |                                                  |
|  +----------------------------------------------v-----------------------------------------------+  |
|  | Expiry Subsystem (keva::core::Dict expires_):                                                |  |
|  | - Auxiliary dictionary: key -> millisecond timestamp                                         |  |
|  | - Passive Expiration: lazy check & delete during get()                                       |  |
|  | - Active Expiration: 10Hz probabilistic random sampling (20 keys, 25% threshold)            |  |
|  +----------------------------------------------+-----------------------------------------------+  |
|                                                 |                                                  |
|  +----------------------------------------------v-----------------------------------------------+  |
|  | Persistence Engine (keva::persistence):                                                      |  |
|  | - Synchronous SAVE: direct sequential write to dump.rdb.tmp + atomic rename()                |  |
|  | - Asynchronous BGSAVE: Linux fork() + Copy-on-Write memory sharing                           |  |
|  | - SIGCHLD reaper: waitpid(WNOHANG) prevents zombie processes                                 |  |
|  | - Data integrity: 64-bit CRC (Jones polynomial) validation                                   |  |
|  +----------------------------------------------------------------------------------------------+  |
+----------------------------------------------------------------------------------------------------+
```

### Subsystem Directory Mapping

The codebase enforces a strict modular structure separating headers (`include/keva/`) and implementations (`src/`):

```
Keva/
├── CMakeLists.txt              # Root build configuration (C++20, ASan/UBSan, LTO)
├── Dockerfile                  # Production multi-stage Dockerfile
├── README.md                   # Repository overview & quick start
├── documentation.md            # Canonical technical specification & architecture
├── include/keva/
│   ├── common/
│   │   ├── types.hpp           # Primitive typedefs (u8, u16, u32, u64, i64, usize), constants
│   │   ├── status.hpp          # Status return pattern (ok, error, io_error, not_found)
│   │   └── logger.hpp          # Thread-safe ANSI colored microsecond console logger
│   ├── net/
│   │   ├── socket.hpp          # RAII socket descriptor wrapper with non-blocking helpers
│   │   ├── connection.hpp      # Client session state, read/write buffers, consume cursors
│   │   ├── event_loop.hpp      # Linux epoll abstraction, timer schedulers, signal pipes
│   │   └── tcp_server.hpp      # Master TCP listener, connection manager, request binder
│   ├── protocol/
│   │   ├── resp_types.hpp      # RESP2 AST nodes (RespType, RespValue struct)
│   │   ├── resp_parser.hpp     # Streaming zero-copy FSM protocol parser
│   │   └── resp_encoder.hpp    # Zero-allocation stack buffer RESP serializer
│   ├── core/
│   │   ├── buffer.hpp          # Binary-safe dynamic byte array (SDS equivalent)
│   │   ├── object.hpp          # 16-byte packed KevaObject, encodings, LRU clock
│   │   ├── dict.hpp            # Dual-table progressive incremental hash table (SipHash)
│   │   ├── db.hpp              # Key space, TTL dictionary, lazy & active expiration
│   │   └── server_context.hpp  # Global server singleton (stats, config, databases, cron)
│   ├── command/
│   │   ├── command.hpp         # Table-driven command router, CommandEntry, arity checking
│   │   └── handlers/
│   │       ├── string_cmd.hpp  # SET, GET, MSET, MGET, INCR, DECR, APPEND, STRLEN, etc.
│   │       ├── key_cmd.hpp     # DEL, EXISTS, TYPE, EXPIRE, TTL, PERSIST, KEYS, RENAME
│   │       └── server_cmd.hpp  # PING, ECHO, FLUSHDB, INFO, SAVE, BGSAVE, SELECT, QUIT
│   └── persistence/
│       ├── rdb.hpp             # Binary RDB opcodes, file layout, RdbWriter, RdbReader
│       ├── rdb_save.hpp        # Synchronous SAVE, fork() BGSAVE, SIGCHLD reaper, CRC-64
│       └── rdb_load.hpp        # Startup snapshot parser and keyspace reconstruction
├── src/                        # Implementations corresponding 1:1 with headers
└── tests/
    ├── unit/                   # C++ unit tests (test_buffer, test_dict, test_resp)
    └── integration/            # Python automated end-to-end integration tests
```

### Request Execution Pipeline

Tracing the exact lifetime of a request: `SET user:1 "Alice" EX 60`:

```
1. Client Sockets
   - Client sends: "*5\r\n$3\r\nSET\r\n$6\r\nuser:1\r\n$5\r\nAlice\r\n$2\r\nEX\r\n$2\r\n60\r\n"
   
2. Kernel & epoll
   - Bytes arrive in TCP receive buffer. epoll_wait() returns EPOLLIN for client fd.
   
3. EventLoop & Connection
   - Connection::fill_read_buffer() reads bytes into read_buf_.
   - TcpServer invokes RequestHandler callback -> CommandRegistry::dispatch().
   
4. Protocol Parsing
   - RespParser::parse() scans string_view, validates array header (*5), and extracts 5 bulk strings.
   - Connection::consume() advances cursor by consumed byte count.
   
5. Command Dispatch
   - Dispatcher extracts command token "SET", converts to uppercase.
   - Looks up "SET" in command registry table.
   - Validates arity: 5 elements is within [3, -1].
   
6. Handler Execution (handle_set)
   - Parses "EX 60" -> converts to absolute epoch millisecond: now_ms + 60,000.
   - KevaObject::create_string("Alice"): selects EMBSTR encoding (5 bytes <= 44).
   - KevaDatabase::set("user:1", obj):
     - Dict::set("user:1", obj): inserts into ht[0] (or ht[1] if rehashing).
     - Dict::rehash_step(1): migrates 1 bucket if rehashing active.
   - KevaDatabase::set_expire("user:1", expire_ms):
     - Inserts key -> timestamp in expires_ dictionary.
   - ServerContext::record_write(): dirty_keys counter increments.
   
7. Response Serialization & Flush
   - RespEncoder::ok(conn) writes "+OK\r\n" directly to Connection::write_buf_.
   - Connection::flush_write_buffer() performs non-blocking write() to socket.
   - Client receives "+OK\r\n".
```

### Architectural Comparison: Keva vs. Redis vs. Naive Caches

| Architectural Feature | Keva (Phase 1 Baseline) | Redis (v6.0 / v7.0) | Naive In-Memory Cache (e.g. Map + Mutex) |
|---|---|---|---|
| **Language & Tooling** | C++20 (Strict ISO, Zero Dependencies) | ANSI C (C99 / C11) | Go, Python, or Java with standard maps |
| **Concurrency Architecture** | Single-Threaded Reactor (`epoll`) | Reactor + Multi-Threaded I/O Workers | Multi-threaded with `std::mutex` or `sync.RWMutex` |
| **Key Collision Latency** | Amortized Progressive Rehash (zero pause) | Amortized Progressive Rehash (zero pause) | Stop-the-world table rehash (100ms+ spikes) |
| **Wire Protocol** | RESP2 (Binary-Safe Streaming FSM) | RESP2 & RESP3 | Custom JSON, REST HTTP, or gRPC |
| **Object Header Overhead** | 16 Bytes (Packed Bitfields) | 16 Bytes (`robj` bitfields) | 32–64 Bytes (standard class/struct metadata) |
| **Short String Optimization** | Embedded `EMBSTR` ($\le 44$ bytes) | Embedded `EMBSTR` ($\le 44$ bytes) | Full heap allocation per string |
| **Integer Optimization** | Direct pointer encoding (64-bit int) | Direct pointer encoding + shared integer pool | Separate boxed integer allocation |
| **Memory Eviction** | Approximated LRU (24-bit clock sampling) | Approximated LRU / LFU sampling | Strict LRU linked list ($O(1)$ lock contention on reads) |
| **Persistence Mechanism** | Linux `fork()` COW Snapshot (RDB) | RDB Snapshot + AOF Journaling | Periodic JSON dumping or none |
| **Data Integrity Verification** | 64-Bit CRC (Jones Polynomial) | 64-Bit CRC | None or simple MD5/SHA256 |

---

## 4. Networking Engine: The Single-Threaded Reactor

### Linux `epoll` I/O Multiplexing

In a traditional synchronous multi-threaded architecture, each client connection is assigned a dedicated OS thread. When scaling to 10,000 concurrent connections, this model collapses under memory overhead (each thread stack consumes 2MB–8MB) and kernel context-switching latency.

Keva employs the **Reactor Pattern** via Linux's `epoll` subsystem. A single thread monitors thousands of socket file descriptors in $O(1)$ time:

```
                  +-----------------------------------+
                  |      Linux epoll Kernel Space     |
                  +-----------------+-----------------+
                                    |
            epoll_wait() returns list of ready file descriptors
                                    |
                                    v
                  +-----------------------------------+
                  |    keva::net::EventLoop::run()    |
                  +-----------------+-----------------+
                                    |
         +--------------------------+--------------------------+
         |                                                     |
         v                                                     v
[Listening Socket Ready]                              [Client Socket Ready]
         |                                                     |
  ::accept4(O_NONBLOCK)                                +-------+-------+
         |                                             |               |
New Connection Created                                 v               v
Registered with epoll                               EPOLLIN         EPOLLOUT
                                                       |               |
                                              fill_read_buffer()  flush_write_buffer()
                                              Dispatch Handler
```

1. **`epoll_create1(EPOLL_CLOEXEC)`**: Allocates the kernel epoll interest list.
2. **`epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev)`**: Registers sockets for `EPOLLIN` (readable) and `EPOLLOUT` (writable).
3. **`epoll_wait(epoll_fd, events, MAX_EVENTS, timeout_ms)`**: Suspends the thread until I/O events occur or the timer expires. Returns only active descriptors.

#### Level-Triggered (LT) vs. Edge-Triggered (ET)
Keva uses **Level-Triggered (LT)** notification:
- In LT mode, `epoll_wait` continues to report a socket as readable as long as unconsumed bytes remain in the kernel TCP buffer.
- This simplifies buffer management and prevents starvation or missed packets if a single read does not consume all incoming bytes.

### Socket Abstractions & Non-Blocking Flags

All socket interactions are encapsulated within the RAII class `keva::net::Socket`:
- Sockets are automatically closed via `::close()` upon destruction.
- Move-only semantics prevent accidental file descriptor duplication.

Critical socket options configured at startup:
- **`O_NONBLOCK`**: Configured via `::fcntl(fd, F_SETFL, flags | O_NONBLOCK)`. Reads and writes return immediately with `EAGAIN` / `EWOULDBLOCK` rather than suspending the thread.
- **`SO_REUSEADDR`**: Allows immediate server reboot and re-binding to port 6379 without waiting for `TIME_WAIT` sockets to clear.
- **`TCP_NODELAY`**: **Disables Nagle's algorithm**. Nagle buffers small packets to aggregate them into full MTU frames. For database servers returning small replies (like `+PONG\r\n` or `+OK\r\n`), Nagle introduces up to 40ms of synthetic TCP latency. Disabling it ensures immediate packet transmission.
- **`SO_KEEPALIVE`**: Detects dead peer sockets when clients terminate ungracefully.

### Connection Lifecycle & State Machine

Each connected client is managed by an instance of `keva::net::Connection`:

```
+-------------------------------------------------------------------------------+
|                            Connection State Machine                           |
+-------------------------------------------------------------------------------+

        [Client Connects]
               |
               v
     +-------------------+
     | ConnectionState:: | <====== Normal state: reads commands, flushes responses
     |    Connected      |
     +---------+---------+
               |
               | (Client sends QUIT command)
               v
     +-------------------+
     | ConnectionState:: | <====== Flush remaining write_buf_, then close
     |    WriteOnly      |
     +---------+---------+
               |
               | (Write buffer empty OR socket error / EOF)
               v
     +-------------------+
     | ConnectionState:: | <====== Deregister from epoll, close fd, free buffers
     |     Closed        |
     +-------------------+
```

### Buffer Compaction & Zero-Copy Slicing

TCP delivers bytes as an unsegmented stream. A command may arrive across multiple `read()` calls, or multiple commands may arrive in a single packet (pipelining).

#### The Read Buffer Offset Strategy
Rather than shifting all bytes left via `std::vector::erase()` on every parsed token ($O(N)$ memory copies):
1. Incoming bytes are appended to the back of `read_buf_`.
2. The parser operates over a lightweight `std::string_view` (`read_view()`) starting from `read_pos_`.
3. When `read_pos_` exceeds `4096` bytes and constitutes more than half of `read_buf_`, Keva compacts the buffer in-place:

```cpp
void Connection::consume(usize n) noexcept {
    read_pos_ += n;
    // Compaction condition: prevent buffer creep without thrashing
    if (read_pos_ >= 4096 && read_pos_ >= read_buf_.size() / 2) {
        const usize remaining = read_buf_.size() - read_pos_;
        std::memmove(read_buf_.data(), read_buf_.data() + read_pos_, remaining);
        read_buf_.resize(remaining);
        read_pos_ = 0;
    }
}
```

### Signal Handling via the Self-Pipe Trick

Linux signals (`SIGTERM`, `SIGINT`, `SIGCHLD`) execute asynchronously, interrupting process execution at arbitrary instructions. Calling complex database code or allocating memory inside a signal handler causes deadlocks and undefined behavior (reentrancy violation).

Keva solves this with the classic **POSIX Self-Pipe Trick**:

```
[OS Signal: SIGINT, SIGTERM, SIGCHLD]
             |
             v
+-------------------------------------------------------------+
| static void signal_handler(int signum)                      |
| -> Writes 1 byte containing 'signum' to g_signal_pipe[1]    |
+-------------------------------------------------------------+
             |
             v (Kernel pipe buffer)
+-------------------------------------------------------------+
| Linux epoll wakes up: g_signal_pipe[0] is marked EPOLLIN    |
+-------------------------------------------------------------+
             |
             v
+-------------------------------------------------------------+
| EventLoop dispatches on_signal_pipe_readable() synchronously |
| - SIGTERM / SIGINT: loop.stop() -> graceful teardown        |
| - SIGCHLD: waitpid() -> reaps BGSAVE child, logs exit status |
+-------------------------------------------------------------+
```

### The 10Hz Server Cron (`server_cron`)

Keva configures an epoll timer tick running at `hz` frequency (default: 10Hz, every 100 milliseconds). Every tick executes `ServerContext::server_cron()`:

1. **Active Expiry Sweep**: Runs `KevaDatabase::active_expire_cycle()` across databases to proactively delete expired TTL keys.
2. **Incremental Rehash Progression**: Runs `KevaDatabase::rehash_step()` to advance ongoing hash table migrations.
3. **Automated Save Points Check**: Evaluates if `dirty_keys` and elapsed seconds meet any configured save point thresholds:
   - Save after 3600 seconds if $\ge 1$ key changed.
   - Save after 300 seconds if $\ge 100$ keys changed.
   - Save after 60 seconds if $\ge 10,000$ keys changed.

---

## 5. RESP2 Protocol Engine

### RESP2 Wire Protocol Specification

RESP2 is a binary-safe, human-readable text-and-frame protocol. The initial byte denotes the payload type:

```
+------------------+--------+----------------------------+-------------------------------------+
| RESP Type        | Prefix | Wire Format Example        | Meaning / Client Representation     |
+------------------+--------+----------------------------+-------------------------------------+
| Simple String    |  '+'   | "+OK\r\n"                  | Status replies, non-binary string   |
| Error            |  '-'   | "-ERR unknown command\r\n" | Error message with type prefix      |
| Integer          |  ':'   | ":1000\r\n"                | Signed 64-bit integer               |
| Bulk String      |  '$'   | "$5\r\nhello\r\n"          | Binary-safe string of exact length  |
| Null Bulk String |  '$'   | "$-1\r\n"                  | Null / nil object (missing key)     |
| Array            |  '*'   | "*2\r\n$3\r\nGET\r\n$1\r\na"| Ordered collection of RESP elements |
| Null Array       |  '*'   | "*-1\r\n"                  | Null array response                 |
+------------------+--------+----------------------------+-------------------------------------+
```

### Abstract Syntax Tree (`RespValue`)

The parser transforms wire frames into `keva::protocol::RespValue` structures:

```cpp
struct RespValue {
    RespType               type = RespType::Nil;
    std::string            str;        // SimpleString, Error, BulkString payload
    i64                    int_val = 0;// Integer payload
    std::vector<RespValue> elements;   // Array payload (recursive)
};
```

### Streaming FSM Parser (`RespParser`)

The parser operates as a streaming Finite State Machine. If a client transmits a 10MB payload across several TCP segments, the parser safely returns `ParseResult::Incomplete` without dropping state or resetting the connection:

```
                     +---------------------------------------+
                     |         Input: string_view            |
                     +-------------------+-------------------+
                                         |
                       Examine prefix byte at current pos
                                         |
         +-------------+-------------+---+---------+-------------+
         |             |             |             |             |
        '+'           '-'           ':'           '$'           '*'
         |             |             |             |             |
         v             v             v             |             |
   [Simple String]  [Error]      [Integer]         |             |
         |             |             |             |             |
         +-------------+-------------+             |             |
                       |                           |             |
               Scan for \r\n line                  |             |
                       |                           v             |
                       |                     [Bulk String]       |
                       |                           |             |
                       |                  Parse length prefix    |
                       |                  Length == -1 ? -> Nil  |
                       |                  Length > 0 ?           |
                       |                  Check input.size()     |
                       |                  < len + 2 ? Incomplete |
                       |                  Extract payload string |
                       |                  Validate trailing \r\n |
                       |                           |             |
                       +---------------------------+             |
                                                                 v
                                                              [Array]
                                                                 |
                                                        Parse element count
                                                        Count == -1 ? -> Nil
                                                        Loop i from 0 to count:
                                                          Recursive parse_value()
                                                          If Incomplete -> return Incomplete
```

### Zero-Allocation Response Serialization (`RespEncoder`)

In high-throughput databases, constructing strings via `std::to_string()` or `std::stringstream` introduces immense allocator lock contention and heap fragmentation.

`RespEncoder` formats responses **directly into the connection's write buffer** using stack-allocated buffers and C++17 `std::to_chars`:

```cpp
void RespEncoder::integer(net::Connection& conn, i64 value) {
    conn.append_response(":");
    char buf[32];
    auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), value);
    conn.append_response(std::string_view{buf, static_cast<usize>(end - buf)});
    conn.append_response("\r\n");
}
```
For an `INCR` command, this guarantees **zero heap allocations** during response formatting.

---

## 6. Core Storage Engine & Memory Model

### Dynamic Binary-Safe String Buffer (`Buffer`)

#### Why not `std::string`?
`std::string` uses null-terminator sentinels in many C-interop scenarios (`c_str()`, legacy searches). If an application caches raw gzip bytes or serialized protobufs containing embedded `\0` bytes, standard string implementations risk subtle truncation bugs.

Keva's `Buffer` implements an SDS-style (Simple Dynamic String) memory model:

```
+-----------------------------------------------------------------------------+
|                             Buffer Memory Layout                            |
+-----------------------------------------------------------------------------+

  data_ pointer
    |
    v
  +---+---+---+---+---+---+---+---+----+----+----+----+----+----+----+----+
  | H | e | l | l | o | \0| W | o | r  | l  | d  | \0 |    |    |    |    |
  +---+---+---+---+---+---+---+---+----+----+----+----+----+----+----+----+
  |<-------------- size_ = 11 --------------->| \0 |<-- unused cap_ -->|
                                                sentinel
```

- **Explicit Length**: Tracks `size_` as a dedicated integer; binary safe for any byte stream.
- **Null-Terminated Sentinel**: Automatically appends a hidden `\0` byte past `size_`, allowing safe read-only passing to legacy POSIX APIs (`open`, `strerror`).
- **Power-of-Two Growth Strategy**: Capacity doubles on reallocation ($16, 32, 64, \dots$) ensuring amortized $O(1)$ appends.

### Universal Value Container (`KevaObject`)

Every value in Keva is encapsulated by a packed 16-byte `KevaObject` header:

```
+-----------------------------------------------------------------------------+
|                    KevaObject 16-Byte Header Memory Layout                  |
+-----------------------------------------------------------------------------+

 Byte 0..3:  Packed 32-bit Metadata Word (meta)
             +------------------+------------------+--------------------------+
             | Bits 0..3 (4b)   | Bits 4..7 (4b)   | Bits 8..31 (24b)         |
             | ObjectType       | ObjectEncoding   | LRU Clock (Seconds)      |
             +------------------+------------------+--------------------------+

 Byte 4..7:  u32 refcount (Reference counter for shared object pools)

 Byte 8..15: void* ptr (64-bit Pointer to payload OR direct integer value)

 Total Header Footprint: 32 bits + 32 bits + 64 bits = 128 bits = EXACTLY 16 BYTES
```

### Object Encodings: `RAW`, `INT`, `EMBSTR`

Keva dynamically optimizes memory layout based on value payload characteristics:

```
1. ENCODING_INT (64-bit Signed Integer):
   +------------------------------------+
   | KevaObject (16 bytes)              |
   |   meta: [Type=String, Enc=Int, LRU]|
   |   ptr : 0x000000000000002A (val=42)| -> ZERO heap allocations!
   +------------------------------------+

2. ENCODING_EMBSTR (Embedded String <= 44 bytes):
   +--------------------------------------------------------------------------+
   | Single Contiguous Heap Block (64 bytes total)                            |
   | +------------------------------------+---------------------------------+ |
   | | KevaObject Header (16 bytes)       | Buffer + Chars (48 bytes)       | |
   | |   meta: [Type=String, Enc=Embstr]  |   "user_session_token_xyz..."   | |
   | |   ptr : points 16 bytes forward    |                                 | |
   | +------------------------------------+---------------------------------+ |
   +--------------------------------------------------------------------------+
   -> Exactly 1 malloc() call, 1 cache-line hit (64 bytes), zero pointer chasing!

3. ENCODING_RAW (Large String > 44 bytes):
   +------------------------------------+      +------------------------------+
   | KevaObject Header (16 bytes)       | ---> | Heap-Allocated Buffer Struct |
   |   meta: [Type=String, Enc=Raw]     |      |   len, cap, data[] array     |
   |   ptr : pointer to external Buffer |      +------------------------------+
   +------------------------------------+
```

### Dual-Table Incremental Hash Table (`Dict`)

The primary keyspace is managed by `keva::core::Dict`.

#### Why not `std::unordered_map`?
`std::unordered_map` performs bucket reallocation in a single monolithic operation. When rehashing a table containing 1,000,000 keys, the thread locks up for 50–200 milliseconds allocating new buckets and moving nodes. In high-concurrency environments, this violates SLA latency limits.

Keva employs **dual hash tables (`ht_[0]` and `ht_[1]`)** inspired by Redis's `dict.c`:

```
+-----------------------------------------------------------------------------+
|                               Dict Architecture                             |
+-----------------------------------------------------------------------------+

  rehashidx_ = -1 (Normal State: Not Rehashing)
  +-------------------------------+
  | ht_[0]: Live Table            | ---> [Bucket 0] -> [Entry A] -> [Entry B]
  |   size: 16, used: 12          |      [Bucket 1] -> nullptr
  +-------------------------------+      [Bucket 2] -> [Entry C]
  | ht_[1]: Inactive (size: 0)    |
  +-------------------------------+

  rehashidx_ = 1 (Rehashing in Progress: Migrating Buckets)
  +-------------------------------+
  | ht_[0]: Old Table (Migrating) | ---> [Bucket 0] -> nullptr (MIGRATED)
  |   size: 16, used: 8           |      [Bucket 1] -> [Entry B] (CURRENT)
  +-------------------------------+      [Bucket 2] -> [Entry C] (PENDING)
  | ht_[1]: New Table (Active)    | ---> [Bucket 0] -> [Entry A]
  |   size: 32, used: 4           |      [Bucket 1] -> nullptr
  +-------------------------------+
```

### SipHash-1-2 & HashDoS Defense

Predictable hash functions (e.g., MurmurHash, FNV, or modulo) expose databases to **HashDoS attacks**: an attacker deliberately constructs strings that hash to identical bucket indices. This degenerates $O(1)$ hash table lookups into $O(N)$ linked-list traversals, causing 100% CPU starvation.

Keva implements **SipHash-1-2**:
- Seeded at server startup with a cryptographically secure 128-bit key (`k0`, `k1`) generated from `/dev/urandom`.
- Makes hash outputs completely unpredictable to outside clients, mathematically neutralizing algorithmic complexity attacks.

### Amortized Progressive Rehashing Mechanics

Rehashing is triggered when the load factor (`used / size`) exceeds `1.0`:
1. `ht_[1]` is allocated at the next power-of-two capacity ($2 \times \text{size}$).
2. `rehashidx_` is set to `0`.
3. **Step-by-Step Migration**:
   - Every lookup, insertion, or deletion (`get`, `set`, `del`) migrates `1` bucket from `ht_[0]` to `ht_[1]`.
   - The 10Hz `server_cron` executes background steps to ensure completion even during read-heavy or idle workloads.
4. **Querying During Migration**:
   - Keys are first searched in `ht_[0]`. If not found, the search queries `ht_[1]`.
   - New insertions are written directly to `ht_[1]`.
5. **Completion**: When `ht_[0].used == 0`, `ht_[0]` bucket memory is freed, `ht_[0]` is swapped with `ht_[1]`, and `rehashidx_` resets to `-1`.

---

## 7. Key Expiry, Eviction & Memory Management

### The Dual-Dictionary Architecture (`keys_` vs. `expires_`)

Most databases append expiry timestamps directly onto every key-value node. If a user stores 10,000,000 keys but only 100 have TTLs, storing a 64-bit timestamp per node wastes 80MB of memory.

Keva uses **dual parallel dictionaries** inside `KevaDatabase`:
- `keys_`: Maps `key -> KevaObject*` (Primary key-space).
- `expires_`: Maps `key -> MillisecondTimestamp` (Only keys with an active TTL).

Keys created via standard `SET` (without `EX`/`PX`) incur **zero bytes** of TTL memory overhead.

### Passive (Lazy) Expiration on Access

Every key lookup (`get()`, `del()`, `exists()`) performs a passive expiration check:

```cpp
KevaObject* KevaDatabase::get(std::string_view key) {
    if (is_expired(key, now_milliseconds())) {
        delete_key(key);  // Deletes from both keys_ and expires_
        return nullptr;   // Returns nil to client
    }
    KevaObject* obj = static_cast<KevaObject*>(keys_.get(key));
    if (obj) obj->touch(lru_clock::current()); // Update LRU timestamp
    return obj;
}
```
Expired keys accessed by clients are purged immediately.

### Active Probabilistic Expiration Cycle

If an application writes millions of expired keys and never accesses them again, passive expiration alone would leak memory.

The 10Hz `server_cron` triggers `active_expire_cycle()`:
1. Randomly samples `ACTIVE_EXPIRE_SAMPLE_SIZE` (20 keys) from `expires_`.
2. Inspects their timestamps; deletes all keys whose `timestamp < now_ms`.
3. If **more than 25%** of the sampled keys were expired, the cycle repeats immediately.
4. An iteration ceiling prevents this loop from blocking the single-threaded reactor for more than 25 milliseconds.

**Mathematical Result**: Over time, the percentage of expired keys in memory is kept strictly below 25%, guaranteeing bounded memory consumption.

### Approximated LRU Eviction & Candidate Pool

When `--maxmemory <bytes>` is configured and memory limits are reached, Keva evicts keys using an **Approximated LRU** algorithm:
- Traditional LRU requires maintaining a doubly-linked list of all keys. Every read operation must write to the list to move the accessed key to the head, destroying CPU cache locality and causing lock contention.
- Keva stores a 24-bit LRU clock directly in `KevaObject::meta`.
- **Sampling Pool**: When memory is exhausted, Keva randomly samples $K$ keys (default: 5), calculates their idle time (`idle_seconds = current_lru - obj_lru`), and evicts the key with the largest idle time.
- Approximated LRU with $K=5$ matches true LRU precision by over 99% while avoiding all linked-list memory overhead.

---

## 8. Persistence Engine: Point-in-Time RDB Snapshots

### Snapshot Strategy & Kernel Virtual Memory

Disk persistence in in-memory databases presents a fundamental challenge: saving gigabytes of memory to disk takes seconds. If the server pauses to write data, client requests freeze. If the server writes asynchronously on another thread while processing writes, race conditions corrupt data structures.

Keva solves this utilizing **Linux Kernel Page Table Virtualization and Copy-on-Write (COW)**.

### Linux `fork()` & Copy-on-Write (COW) Deep Dive

```
1. Before BGSAVE:
   +-------------------------------------------------------------+
   | Parent Process (PID 1000): Keva Server                      |
   | Page Table: [Page 1] [Page 2] [Page 3] [Page 4]             |
   +----------------------+--------------------------------------+
                          |
                          v (Physical RAM)
                     [RAM Page 1] [RAM Page 2] [RAM Page 3] [RAM Page 4]

2. BGSAVE Triggered -> Linux fork() Call:
   +-------------------------------------------------------------+
   | Parent (PID 1000)                                           |
   | Page Table: [Page 1] [Page 2] [Page 3] [Page 4] (Read-Only) |
   +----------------------+--------------------------------------+
                          |
                          +---------------+ (Zero Physical Memory Copied!)
                          |               |
   +----------------------v---------------+----------------------+
   | Child Process (PID 1001)                                    |
   | Page Table: [Page 1] [Page 2] [Page 3] [Page 4] (Read-Only) |
   +-------------------------------------------------------------+
   * The child starts serializing physical pages to dump.rdb.tmp.

3. Client executes SET key "new_val" (Modifies Page 2):
   - Parent attempts write to read-only Page 2.
   - Kernel intercepts CPU Page Fault -> allocates new physical page (RAM Page 2B).
   - Parent's page table points to RAM Page 2B.
   - Child continues reading original RAM Page 2 undisturbed!
```

- **Instantaneous Snapshot**: `fork()` takes $\approx 1$ millisecond regardless of database size.
- **Zero Lock Contention**: The parent serves queries at full speed without mutexes.

### RDB Binary File Format Specification

Keva files follow a binary layout with little-endian encoding:

```
+-----------------------------------------------------------------------------+
|                             Keva RDB Binary Layout                          |
+-----------------------------------------------------------------------------+

[Header]
  Magic Bytes       : "KEVA" (4 bytes ASCII: 0x4B, 0x45, 0x56, 0x41)
  Format Version    : u16 (2 bytes: 0x0001)

[Database Section]
  DB Selector Opcode: 0xFE (1 byte)
  DB Index Number   : u32 (4 bytes: 0x00000000 for DB 0)

[Key-Value Stream] (Repeated for every entry)
  [Optional Expire] : Opcode 0xFC (1 byte) + i64 (8 bytes: millisecond timestamp)
  Value Type Byte   : 0x00 (RDB_TYPE_STRING)
  Key Length        : Varint-encoded length (1 to 5 bytes)
  Key Data          : Raw byte sequence
  Value Length      : Varint-encoded length (1 to 5 bytes)
  Value Data        : Raw byte sequence

[Footer]
  EOF Opcode        : 0xFF (1 byte)
  Integrity Checksum: u64 (8 bytes: CRC-64 Jones polynomial)
```

#### Varint String Length Encoding
Matches Redis RDB string compression:
- `00xxxxxx`: Length fits in 6 bits (0 to 63).
- `01xxxxxx xxxxxxxx`: Length fits in 14 bits (64 to 16,383).
- `10000000 + 4 bytes`: Full 32-bit unsigned length prefix.

### Atomic File Swapping via POSIX `rename()`

Writing directly to `dump.rdb` risks corrupting data if the server crashes or runs out of disk space mid-write.

Keva enforces **atomic durability**:
1. All bytes are written to a temporary sibling file: `dump.rdb.tmp`.
2. Upon file close and CRC-64 flush, Keva invokes `::rename("dump.rdb.tmp", "dump.rdb")`.
3. POSIX guarantees `rename()` is an atomic inode pointer swap at the filesystem level. The existing `dump.rdb` is never exposed to partial states.

### Synchronous (`SAVE`) vs. Asynchronous (`BGSAVE`) Workflows

- **`SAVE`**: Executed on the main thread. Blocks all network I/O until disk synchronization completes. Suitable for maintenance scripts or pre-shutdown snapshots.
- **`BGSAVE`**: Forks a child worker. Parent immediately returns `+Background saving started`. The child process writes `dump.rdb.tmp`, renames it, and exits via `_exit(0)`. The parent reaps the child via `SIGCHLD`.

### Cold-Start Recovery Engine (`rdb_load`)

On server startup (`main.cpp`):
1. Keva checks for the presence of `dump.rdb`.
2. Validates the 4-byte magic signature (`KEVA`) and format version.
3. Iteratively reads opcodes, reconstructing keys, strings, and TTL records into `KevaDatabase`.
4. Discards keys whose stored TTL timestamp is already in the past.
5. Computes the CRC-64 checksum across all bytes up to `0xFF (EOF)` and validates it against the file's footer.

### CRC-64 Jones Polynomial Integrity Checksum

Keva implements an optimized 64-bit Cyclic Redundancy Check utilizing the **Jones polynomial** (`0xad93d23594c935a9ULL`):
- Pre-computes a 256-entry lookup table (`crc64_table`) on initialization.
- Computes the checksum byte-by-byte in $O(1)$ time per byte.
- Protects snapshots against bit rot, storage degradation, and truncated writes.

---

## 9. Complete Command Reference

Every command implemented in Keva matches Redis RESP2 specifications.

### String Commands

---

#### `SET`
- **Syntax:** `SET key value [EX seconds] [PX milliseconds] [NX|XX]`
- **Arity:** $\ge 3$
- **Complexity:** $O(1)$
- **Description:** Sets the string value of a key. Overwrites any previous value and resets existing TTLs unless flags are specified.
- **Options:**
  - `EX seconds`: Sets an expiration time in seconds.
  - `PX milliseconds`: Sets an expiration time in milliseconds.
  - `NX`: Only set the key if it does **not** already exist.
  - `XX`: Only set the key if it **already exists**.
- **Return Value:** `+OK\r\n` on success; `$-1\r\n` (nil) if `NX` or `XX` conditions fail.
- **Example Wire Session:**
  ```
  Client: *3\r\n$3\r\nSET\r\n$4\r\nuser\r\n$5\r\nAlice\r\n
  Server: +OK\r\n
  ```

---

#### `GET`
- **Syntax:** `GET key`
- **Arity:** 2
- **Complexity:** $O(1)$
- **Description:** Retrieves the value of `key`. Performs passive expiry validation.
- **Return Value:** Bulk String containing the value, or `$-1\r\n` (nil) if the key does not exist or has expired.
- **Errors:** Returns `-WRONGTYPE` if the key contains a non-string type.
- **Example Wire Session:**
  ```
  Client: *2\r\n$3\r\nGET\r\n$4\r\nuser\r\n
  Server: $5\r\nAlice\r\n
  ```

---

#### `GETSET`
- **Syntax:** `GETSET key value`
- **Arity:** 3
- **Complexity:** $O(1)$
- **Description:** Atomically sets `key` to `value` and returns the old value stored at `key`.
- **Return Value:** Bulk String of the previous value, or `$-1\r\n` (nil) if key did not exist.

---

#### `MSET`
- **Syntax:** `MSET key1 value1 [key2 value2 ...]`
- **Arity:** $\ge 3$ (must be odd number of tokens)
- **Complexity:** $O(N)$ where $N$ is the number of keys to set.
- **Description:** Sets the given keys to their respective values. Overwrites existing values.
- **Return Value:** `+OK\r\n`.

---

#### `MGET`
- **Syntax:** `MGET key1 [key2 ...]`
- **Arity:** $\ge 2$
- **Complexity:** $O(N)$ where $N$ is the number of keys to retrieve.
- **Description:** Returns the values of all specified keys.
- **Return Value:** Array of Bulk Strings or Nil for missing/expired keys.
- **Example Wire Session:**
  ```
  Client: *3\r\n$4\r\nMGET\r\n$2\r\nk1\r\n$2\r\nk2\r\n
  Server: *2\r\n$4\r\nval1\r\n$-1\r\n
  ```

---

#### `APPEND`
- **Syntax:** `APPEND key value`
- **Arity:** 3
- **Complexity:** Amortized $O(1)$
- **Description:** Appends `value` to the existing value of `key`. If the key does not exist, it is created empty and appended to. Converts `INT`-encoded keys to `RAW`.
- **Return Value:** Integer representing the total byte length of the string after appending.

---

#### `STRLEN`
- **Syntax:** `STRLEN key`
- **Arity:** 2
- **Complexity:** $O(1)$
- **Description:** Returns the byte length of the string value stored at `key`.
- **Return Value:** Integer representing length, or `0` if key does not exist.

---

#### `INCR` / `DECR`
- **Syntax:** `INCR key` / `DECR key`
- **Arity:** 2
- **Complexity:** $O(1)$
- **Description:** Increments or decrements the number stored at `key` by one. If the key does not exist, it is initialized to `0` before performing the operation.
- **Return Value:** Integer representing the value of `key` after the increment/decrement.
- **Errors:** Returns `-ERR value is not an integer or out of range` if string cannot be parsed as a 64-bit signed integer. Returns `-ERR increment or decrement would overflow` if value exceeds `LLONG_MAX` or `LLONG_MIN`.

---

#### `INCRBY` / `DECRBY`
- **Syntax:** `INCRBY key increment` / `DECRBY key decrement`
- **Arity:** 3
- **Complexity:** $O(1)$
- **Description:** Increments or decrements the number stored at `key` by the specified 64-bit integer offset.
- **Return Value:** Integer representing the resulting value.

---

#### `SETEX` / `PSETEX`
- **Syntax:** `SETEX key seconds value` / `PSETEX key milliseconds value`
- **Arity:** 4
- **Complexity:** $O(1)$
- **Description:** Atomically sets `key` to `value` and associates an expiration timeout.
- **Return Value:** `+OK\r\n`.

---

#### `SETNX`
- **Syntax:** `SETNX key value`
- **Arity:** 3
- **Complexity:** $O(1)$
- **Description:** Sets `key` to `value` if and only if `key` does not already exist.
- **Return Value:** `:1\r\n` if key was set; `:0\r\n` if key already existed.

---

### Key Space & Lifetime Commands

---

#### `DEL`
- **Syntax:** `DEL key [key ...]`
- **Arity:** $\ge 2$
- **Complexity:** $O(N)$ where $N$ is the number of keys to remove.
- **Description:** Removes the specified keys and their TTL entries.
- **Return Value:** Integer representing the number of keys that were removed.

---

#### `EXISTS`
- **Syntax:** `EXISTS key [key ...]`
- **Arity:** $\ge 2$
- **Complexity:** $O(N)$
- **Description:** Checks whether the specified keys exist and are unexpired.
- **Return Value:** Integer count of existing keys.

---

#### `TYPE`
- **Syntax:** `TYPE key`
- **Arity:** 2
- **Complexity:** $O(1)$
- **Description:** Returns the string representation of the value type stored at `key`. Does not update the LRU clock (`peek()`).
- **Return Value:** Simple String: `"string"`, `"list"`, `"set"`, `"zset"`, `"hash"`, or `"none"` if key is missing.

---

#### `EXPIRE` / `PEXPIRE` / `EXPIREAT`
- **Syntax:**
  - `EXPIRE key seconds`
  - `PEXPIRE key milliseconds`
  - `EXPIREAT key unix-time-seconds`
- **Arity:** 3
- **Complexity:** $O(1)$
- **Description:** Sets a timeout on `key`. After the timeout has expired, the key will automatically be deleted.
- **Return Value:** `:1\r\n` if the timeout was set; `:0\r\n` if key does not exist.

---

#### `TTL` / `PTTL`
- **Syntax:** `TTL key` / `PTTL key`
- **Arity:** 2
- **Complexity:** $O(1)$
- **Description:** Returns the remaining time to live of a key. `TTL` returns seconds; `PTTL` returns milliseconds.
- **Return Value:**
  - Non-negative Integer: Remaining lifetime.
  - `-1`: Key exists but has no associated expiration.
  - `-2`: Key does not exist or has expired.

---

#### `PERSIST`
- **Syntax:** `PERSIST key`
- **Arity:** 2
- **Complexity:** $O(1)$
- **Description:** Removes the existing timeout on `key`, turning it into a persistent key.
- **Return Value:** `:1\r\n` if the timeout was removed; `:0\r\n` if key had no timeout or did not exist.

---

#### `KEYS`
- **Syntax:** `KEYS pattern`
- **Arity:** 2
- **Complexity:** $O(N)$ where $N$ is total keys in database.
- **Description:** Returns all keys matching `pattern` using glob matching:
  - `*`: Matches any sequence of characters.
  - `?`: Matches any single character.
- **Return Value:** Array of matching key names.

---

#### `RENAME`
- **Syntax:** `RENAME key newkey`
- **Arity:** 3
- **Complexity:** $O(1)$
- **Description:** Renames `key` to `newkey`. Overwrites `newkey` if it exists.
- **Return Value:** `+OK\r\n`.
- **Errors:** Returns `-ERR no such key` if source `key` does not exist.

---

#### `DBSIZE`
- **Syntax:** `DBSIZE`
- **Arity:** 1
- **Complexity:** $O(1)$
- **Description:** Returns the number of keys stored in the active database.
- **Return Value:** Integer representing total key count.

---

### Server Administration Commands

---

#### `PING`
- **Syntax:** `PING [message]`
- **Arity:** 1 or 2
- **Complexity:** $O(1)$
- **Description:** Health check. If no argument is provided, returns `+PONG\r\n`. If `message` is provided, returns `message` as a Bulk String.

---

#### `ECHO`
- **Syntax:** `ECHO message`
- **Arity:** 2
- **Complexity:** $O(1)$
- **Description:** Returns `message` unchanged as a Bulk String.

---

#### `FLUSHDB`
- **Syntax:** `FLUSHDB`
- **Arity:** 1
- **Complexity:** $O(N)$
- **Description:** Deletes all keys and TTL records from the current database.
- **Return Value:** `+OK\r\n`.

---

#### `INFO`
- **Syntax:** `INFO [section]`
- **Arity:** 1 or 2
- **Complexity:** $O(1)$
- **Description:** Returns structured text lines containing server health, uptime, client connections, and keyspace statistics.
- **Return Value:** Bulk String formatted with section headers (`# Server`, `# Clients`, `# Stats`, `# Persistence`, `# Keyspace`).

---

#### `SAVE`
- **Syntax:** `SAVE`
- **Arity:** 1
- **Complexity:** $O(N)$
- **Description:** Synchronously saves the database to `dump.rdb`. Blocks until complete.
- **Return Value:** `+OK\r\n` on success; `-ERR <message>` on failure.

---

#### `BGSAVE`
- **Syntax:** `BGSAVE`
- **Arity:** 1
- **Complexity:** $O(1)$ fork + background write
- **Description:** Asynchronously saves the database snapshot to disk using Linux `fork()` and Copy-on-Write memory sharing.
- **Return Value:** `+Background saving started\r\n`.
- **Errors:** Returns `-ERR Background save already in progress` if a child process is currently executing.

---

#### `SELECT`
- **Syntax:** `SELECT index`
- **Arity:** 2
- **Complexity:** $O(1)$
- **Description:** Selects the active database. Phase 1 supports database `0`.
- **Return Value:** `+OK\r\n` for index `0`; `-ERR DB index is out of range` for indices $\ne 0$.

---

#### `QUIT`
- **Syntax:** `QUIT`
- **Arity:** 1
- **Complexity:** $O(1)$
- **Description:** Closes the client connection gracefully after flushing pending writes.
- **Return Value:** `+OK\r\n`.

---

## 10. Configuration & Operations Guide

### CLI Argument Reference

```bash
keva-server [OPTIONS]
```

- `--host <ip>`: Bind listener to specific IP interface (Default: `0.0.0.0`).
- `--port <port>`: TCP listening port (Default: `6379`).
- `--loglevel <level>`: Set logging threshold: `debug`, `info`, `warn`, `error` (Default: `info`).
- `--maxmemory <bytes>`: Maximum bytes of memory allowed before key eviction triggers. `0` disables eviction (Default: `0`).
- `--rdb-filename <path>`: File path for snapshot storage (Default: `dump.rdb`).
- `--no-rdb`: Completely disables RDB snapshotting and cold-start loading.
- `--help`: Outputs configuration parameter syntax.

### Logging Framework & Verbosity Levels

Keva includes an internal microsecond console logger:
- Formats timestamps with microsecond precision: `2026-09-19 00:30:15.123456`.
- Categorizes log events with distinct ANSI color coding:
  - `DEBUG` (Cyan): Verbose packet dumps and per-step rehash migrations.
  - `INFO` (Green): Startup stages, client connects/disconnects, BGSAVE status.
  - `WARN` (Yellow): Missing configuration fallbacks, high memory warnings.
  - `ERROR` (Red): Socket failures, RDB file corruption, system errors.

### Server Metrics via `INFO`

Querying `INFO` outputs diagnostic sections:

```ini
# Server
keva_version:0.1.0
os:Linux
arch_bits:64
hz:10
uptime_in_seconds:1420
tcp_port:6379

# Clients
connected_clients:1

# Stats
total_commands_processed:150240
total_connections_received:42
keyspace_hits:120400
keyspace_misses:4120
expired_keys:850
evicted_keys:0

# Persistence
rdb_enabled:1
rdb_last_save_time:1789854000
rdb_changes_since_last_save:14

# Keyspace
db0:keys=10500,expires=450
```

### Linux Kernel Tuning for Production

For bare-metal or containerized production deployment, configure the host Linux kernel:

#### 1. Memory Overcommit (`vm.overcommit_memory = 1`)
During `BGSAVE`, Linux's `fork()` clones the process memory address space. By default, Linux uses heuristic overcommit checking; if your database consumes 12GB of RAM on a 16GB machine, the kernel may reject `fork()` with `ENOMEM`. Setting overcommit to `1` informs the kernel to always grant memory requests under COW assumptions:
```bash
sudo sysctl vm.overcommit_memory=1
echo "vm.overcommit_memory = 1" | sudo tee -a /etc/sysctl.conf
```

#### 2. Disable Transparent Huge Pages (THP)
Transparent Huge Pages (THP) groups memory into 2MB chunks instead of 4KB pages. During `BGSAVE`, any minor write to a key forces the kernel to copy an entire **2MB page** instead of a 4KB page, multiplying disk I/O and memory usage by $512\times$:
```bash
echo never | sudo tee /sys/kernel/mm/transparent_hugepage/enabled
```

#### 3. TCP Max SYN Backlog & Somaxconn
Increase the socket listen backlog queue to prevent dropped connection attempts during traffic spikes:
```bash
sudo sysctl -w net.core.somaxconn=65535
sudo sysctl -w net.ipv4.tcp_max_syn_backlog=65535
```

---

## 11. Benchmarking & Performance Profile

### Running `redis-benchmark`

Evaluate Keva's raw throughput using the standard Redis benchmark utility:

```bash
# High-concurrency throughput test: 100,000 requests, 50 parallel clients
redis-benchmark -p 6379 -t set,get,incr -n 100000 -c 50 -q
```

Sample Benchmark Output (WSL2 Ubuntu 24.04, AMD Ryzen 9):
```
SET: 112485.94 requests per second, p50=0.38ms
GET: 124532.99 requests per second, p50=0.32ms
INCR: 118623.96 requests per second, p50=0.35ms
```

### Performance Optimization Highlights

1. **Zero-Allocation Stack Integer Serialization**:  
   Using `std::to_chars` with stack buffers for integer and length lines completely eliminates heap allocations during response encoding.
2. **Buffer Cursor Compaction**:  
   Cursor-based consuming prevents $O(N)$ memory copying on incoming streaming reads.
3. **Contiguous Embedded Strings (`EMBSTR`)**:  
   Small strings ($\le 44$ bytes) are laid out alongside the 16-byte `KevaObject` header in a single 64-byte block, fitting cleanly into a single CPU L1 cache line.
4. **Nagle Disabling (`TCP_NODELAY`)**:  
   Ensures instantaneous delivery of small RESP packets without 40ms kernel buffering delays.

---

## 12. Developer Guide, Testing & Codebase Standards

### Architectural Code Rules & Purpose Comments

Every file in the Keva repository conforms to strict systems engineering practices:
1. **Mandatory Header Comment**:  
   Every source and header file begins with a structured purpose block explaining:
   - The architectural role of the file.
   - Core OS and data structure design decisions.
   - Performance trade-offs.
2. **Deterministic Ownership & RAII**:  
   File descriptors, sockets, child processes, and memory allocations are tied strictly to C++ RAII destructors. No naked `free()` or unhandled POSIX descriptor leaks.
3. **No Unhandled Errors**:  
   Syscalls (`read`, `write`, `epoll_ctl`, `fork`) always inspect return values and check `errno` against `EINTR` and `EAGAIN`.

### Sanitizer Suite (ASan & UBSan)

Debug builds enable AddressSanitizer and UndefinedBehaviorSanitizer:
- Catches out-of-bounds reads/writes on heap, stack, or global memory.
- Detects memory leaks on shutdown.
- Identifies undefined behavior: signed integer overflows, misaligned pointer dereferences, or null pointer offsets.

Build and verify with sanitizers:
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -G Ninja
cmake --build build
./build/src/keva-server --no-rdb
```

### Unit Testing Framework (CTest)

Keva includes dedicated standalone C++ test suites in `tests/unit/`:
- `test_buffer.cpp`: Verifies SDS length tracking, binary safety with embedded `\0`, exponential growth, and integer parsing.
- `test_dict.cpp`: Verifies dual-table progressive rehashing, bucket migration states, SipHash distribution, collision handling, and random sampling.
- `test_resp.cpp`: Validates fragmented TCP packet parsing, nested array trees, nil bulk strings, and syntax error detections.

Run all tests via CTest:
```bash
cd build && ctest --output-on-failure
```

### End-to-End Integration Testing with Python

The integration suite (`tests/integration/test_redis_cli.py`) verifies server compliance across live TCP sockets:
- PING/ECHO mechanics and case-insensitivity.
- String CRUD, binary safety with UTF-8 emojis.
- Expiration lifecycles: `SETEX`, `TTL` countdowns, passive expiration, `PERSIST`.
- Numeric arithmetic: `INCR`, `DECR`, `INCRBY`, `DECRBY`, and 64-bit overflow boundaries.
- Multi-key operations: `MSET`, `MGET`, `EXISTS`, `DEL`.
- Persistence: `SAVE`, `BGSAVE`, and snapshot reload after process termination.

Run the test suite:
```bash
python3 tests/integration/test_redis_cli.py
```

---

## 13. Engineering Roadmap (Phase 2 & Beyond)

With Phase 1 complete and fully verified, the codebase is architected for seamless evolution into Phase 2:

### Multi-Threaded I/O Workers (Redis 6.0 Model)

While command execution in Keva is inherently single-threaded (ensuring zero lock overhead over the database hash table), network I/O and protocol serialization can be offloaded:
- **Architecture**: A pool of $N$ I/O threads handle socket reads, RESP2 parsing, and socket writes.
- **Workflow**: The main reactor thread handles connection acceptance and coordinates work queues. Once worker threads parse commands into `RespValue` trees, the main thread executes handlers sequentially against the database, then assigns response buffers back to workers for asynchronous socket transmission.
- **Benefit**: Multiplies network throughput on multi-gigabit interfaces while preserving lock-free single-threaded database consistency.

### Complex Collections: SkipLists, Hashes, and Lists

Phase 2 will introduce advanced Redis data structures:
1. **Sorted Sets (`ZSET`)**:  
   - Implemented via a **SkipList (`zskiplist`)** paired with a dual `Dict`.
   - Supports $O(\log N)$ inserts, deletions, score updates, and range queries (`ZRANGEBYSCORE`, `ZRANK`).
2. **Hashes (`HASH`)**:  
   - Encoded as a compact `Listpack` for small field counts, upgrading to a full `Dict` for large sets.
   - Commands: `HSET`, `HGET`, `HDEL`, `HGETALL`, `HINCRBY`.
3. **Lists (`LIST`)**:  
   - Implemented via a doubly-linked list of memory blocks (`Quicklist`).
   - Commands: `LPUSH`, `RPUSH`, `LPOP`, `RPOP`, `LRANGE`, `LLEN`.

### Append-Only File (AOF) Persistence

Complementing point-in-time RDB snapshots with real-time durability:
- Logs every state-modifying command to an append-only file on disk.
- Configurable `fsync` policies: `always`, `everysec` (recommended), or `no`.
- Background AOF rewrite (`BGREWRITEAOF`) via `fork()` to compact transaction logs.

---

## Appendix: Wire Protocol Quick Reference Card

```
Simple String:  + <text> \r\n
Error:          - <message> \r\n
Integer:        : <signed-64-bit-integer> \r\n
Bulk String:    $ <length> \r\n <bytes> \r\n
Null String:    $ -1 \r\n
Array:          * <count> \r\n <element_1> ... <element_N>
Null Array:     * -1 \r\n
```

*Keva Database Documentation — Maintained by the Keva Core Development Team.*
