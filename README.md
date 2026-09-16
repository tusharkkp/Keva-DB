# Keva

> A Redis-inspired, high-performance in-memory key-value database built from scratch in C++20.

Keva is a serious systems programming learning project that reimplements the core of a production-grade in-memory database. Every subsystem — the TCP event loop, RESP2 protocol parser, incremental hash table, object encoding system, and RDB persistence engine — is implemented from scratch to demonstrate how real databases work at the OS and hardware level.

---

## Architecture Decisions

| Component | Choice | Rationale |
|---|---|---|
| **Platform** | Linux (WSL2 / Docker) | Direct access to `epoll`, `fork()`, COW memory, POSIX sockets |
| **Language** | C++20 | `std::span`, `std::bit_cast`, `std::bit_ceil`, zero-cost abstractions |
| **Concurrency** | Single-threaded Reactor (Phase 1) → Threaded I/O (Phase 2) | Clean correctness baseline before optimization |
| **Protocol** | RESP2 | 100% `redis-cli` / client library compatibility |
| **Key-Space** | Custom Incremental Hash Table | Dual-table progressive rehashing: zero stop-the-world freezes |
| **Object Model** | Custom `KevaObject` + Binary-Safe Buffer | 16-byte packed header, embedded string, int-as-pointer encoding |
| **Expiry** | Probabilistic Sampling + Approximated LRU | Redis 6.0 model: zero pointer overhead, >99% LRU accuracy |
| **Persistence** | RDB via Linux `fork()` + COW | Point-in-time snapshot with zero data copying overhead |

---

## Quick Start (WSL2)

```bash
# Clone / enter project
cd /mnt/c/PROJECTS/Keva

# Configure (Debug with ASan + UBSan)
cmake -B build -DCMAKE_BUILD_TYPE=Debug -G Ninja

# Build
cmake --build build --parallel

# Run the server
./build/src/keva-server --host 127.0.0.1 --port 6379 --loglevel info

# In another terminal — use redis-cli directly
redis-cli -p 6379 PING
redis-cli -p 6379 SET user:1 "Alice"
redis-cli -p 6379 GET user:1
redis-cli -p 6379 SETEX session 3600 "token_xyz"
redis-cli -p 6379 TTL session
redis-cli -p 6379 BGSAVE
redis-cli -p 6379 INFO
```

## Quick Start (Docker)

```bash
# Build the Docker image
docker build -t keva:latest .

# Run with port forwarding
docker run -p 6379:6379 keva:latest

# Connect from Windows
redis-cli -p 6379 PING
```

---

## Running Tests

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -G Ninja
cmake --build build --parallel
cd build && ctest --output-on-failure
```

---

## Benchmarking

```bash
# Run redis-benchmark against Keva (must be running on port 6379)
redis-benchmark -p 6379 -t get,set,incr -n 100000 -c 50 -q
```

---

## Implemented Commands

### String Commands
`SET` `GET` `GETSET` `MSET` `MGET` `APPEND` `STRLEN`
`INCR` `INCRBY` `DECR` `DECRBY`
`SETEX` `PSETEX` `SETNX`

### Key Commands
`DEL` `EXISTS` `TYPE` `EXPIRE` `PEXPIRE` `EXPIREAT`
`TTL` `PTTL` `PERSIST` `KEYS` `RENAME` `DBSIZE`

### Server Commands
`PING` `ECHO` `FLUSHDB` `INFO` `SAVE` `BGSAVE` `SELECT` `QUIT`

---

## Project Structure

```
Keva/
├── CMakeLists.txt              # C++20, ASan/UBSan debug, -O3 LTO release
├── Dockerfile                  # Multi-stage Linux container build
├── include/keva/
│   ├── common/                 # types.hpp, status.hpp, logger.hpp
│   ├── net/                    # socket.hpp, connection.hpp, event_loop.hpp, tcp_server.hpp
│   ├── protocol/               # resp_types.hpp, resp_parser.hpp, resp_encoder.hpp
│   ├── core/                   # buffer.hpp, object.hpp, dict.hpp, db.hpp, server_context.hpp
│   ├── command/                # command.hpp + handlers/
│   └── persistence/            # rdb.hpp, rdb_save.hpp, rdb_load.hpp
├── src/                        # Implementations
└── tests/
    └── unit/                   # test_dict.cpp, test_buffer.cpp, test_resp.cpp
```

---

## Learning Concepts

Building Keva teaches the following CS fundamentals hands-on:

- **Computer Networks:** TCP state machine, non-blocking sockets, `epoll` I/O multiplexing, Nagle's algorithm, TCP_NODELAY
- **Operating Systems:** Linux syscalls (`fork`, `epoll_wait`, `fcntl`, `mmap`), Copy-on-Write memory, signal handling, process lifecycle
- **Data Structures:** Progressive incremental hash tables, SipHash, power-of-two growth, embedded string optimization
- **OOP / Systems Design:** RAII wrappers, Finite State Machines, table-driven dispatch, Value/Strategy patterns
- **Databases:** Wire protocols (RESP2), binary serialization (RDB), WAL concepts, probabilistic algorithms (LRU sampling)
