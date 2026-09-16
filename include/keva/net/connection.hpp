// ==============================================================================
// keva/net/connection.hpp
//
// Purpose:
//   Declares the Connection class — the stateful object representing a single
//   connected TCP client. Each time a client connects, the TCP server creates
//   one Connection object that persists for the lifetime of that client session.
//
//   Why a separate Connection class?
//   A raw Socket gives us the file descriptor, but a database client connection
//   has far more state:
//   - A read buffer accumulating incoming bytes (because TCP can deliver data
//     in fragments — one RESP2 command might arrive across 2-3 read() syscalls).
//   - A write (output) buffer of response bytes to be flushed back asynchronously.
//   - The RESP2 parser state machine, which must retain mid-parse state between
//     partial reads.
//   - The client ID and connection metadata (for INFO stats and CLIENT LIST).
//
//   Buffer design — why not use std::vector<char>?
//   We use a std::vector<KevaByte> as the underlying storage but wrap it with
//   a "consume offset" pattern (read_pos_):
//   - Bytes arrive at the back of the buffer.
//   - As the parser consumes bytes, it advances read_pos_ (a cursor), rather
//     than shifting the entire array left (O(N) cost on every partial parse).
//   - When read_pos_ advances far enough, the buffer is compacted in-place.
//   This gives us O(1) amortized consume operations.
// ==============================================================================

#pragma once

#include "keva/net/socket.hpp"
#include "keva/common/types.hpp"
#include "keva/common/status.hpp"

#include <vector>
#include <cstdint>

namespace keva::net {

// --------------------------------------------------------------------------
// ConnectionState — lifecycle state machine for a client connection
// --------------------------------------------------------------------------
enum class ConnectionState : u8 {
    Connected,     // Normal state: reading and writing
    WriteOnly,     // Response queued; drain the write buffer then disconnect
    Closed,        // Connection terminated; safe to destroy
};

// --------------------------------------------------------------------------
// Connection — full state for one TCP client session
// --------------------------------------------------------------------------
class Connection {
public:
    // Construct a Connection from an accepted client socket and a unique ID
    explicit Connection(Socket socket, u64 client_id) noexcept;

    // Non-copyable (owns a socket fd)
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // Movable (transfers socket ownership)
    Connection(Connection&&) noexcept = default;
    Connection& operator=(Connection&&) noexcept = default;

    ~Connection() = default;

    // --------------------------------------------------------------------------
    // I/O Operations
    // --------------------------------------------------------------------------

    // Read available bytes from the kernel socket buffer into read_buf_.
    // Returns the number of bytes newly read, 0 on EOF (client disconnected),
    // -1 on a real I/O error, or EAGAIN if no data is currently available.
    ssize_t fill_read_buffer();

    // Flush the write buffer to the socket. Handles partial writes (loops
    // internally until write returns EAGAIN or all bytes are sent).
    // Returns Status::ok() if all pending bytes were sent or EAGAIN hit,
    // Status::io_error() on a real socket error.
    Status flush_write_buffer();

    // --------------------------------------------------------------------------
    // Buffer accessors (used by the RESP parser and command encoder)
    // --------------------------------------------------------------------------

    // View of unconsumed bytes in the read buffer awaiting parsing
    [[nodiscard]] std::string_view read_view() const noexcept;

    // Advance the read cursor (marks 'n' bytes as consumed by the parser)
    void consume(usize n) noexcept;

    // Append bytes to the write (output) buffer (called by response encoder)
    void append_response(const void* data, usize len);
    void append_response(std::string_view sv);

    // True if there is unflushed response data pending in the write buffer
    [[nodiscard]] bool has_pending_writes() const noexcept;

    // --------------------------------------------------------------------------
    // State & Metadata
    // --------------------------------------------------------------------------
    [[nodiscard]] int fd()             const noexcept { return socket_.fd(); }
    [[nodiscard]] u64 client_id()      const noexcept { return client_id_; }
    [[nodiscard]] ConnectionState state() const noexcept { return state_; }
    void set_state(ConnectionState s) noexcept { state_ = s; }

    [[nodiscard]] bool is_closed()     const noexcept { return state_ == ConnectionState::Closed; }

private:
    // Compact the read buffer: shift unconsumed bytes to the front
    // (called when read_pos_ > READ_BUFFER_SIZE / 2 to avoid unbounded growth)
    void compact_read_buffer();

    Socket          socket_;
    u64             client_id_;
    ConnectionState state_ = ConnectionState::Connected;

    // Read (incoming) buffer: bytes from the socket accumulate here
    std::vector<KevaByte> read_buf_;
    usize read_pos_ = 0;   // Cursor: index of first unconsumed byte

    // Write (outgoing) buffer: encoded RESP responses wait here to be sent
    std::vector<KevaByte> write_buf_;
    usize write_pos_ = 0;  // Cursor: index of first unsent byte
};

} // namespace keva::net
