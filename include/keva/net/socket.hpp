// ==============================================================================
// keva/net/socket.hpp
//
// Purpose:
//   Declares the Socket class — a RAII (Resource Acquisition Is Initialization)
//   wrapper around a raw Linux file descriptor that represents a TCP socket.
//
//   Why a RAII wrapper?
//   In C, a socket is just an integer (file descriptor). If you open a socket
//   and then hit an error path, you must manually call close(fd). In complex
//   systems with many error paths, forgetting a close() causes file descriptor
//   leaks that eventually crash the server ("too many open files").
//
//   The RAII pattern ties the fd's lifetime to the Socket object: when the
//   Socket object is destroyed (goes out of scope or is moved), the destructor
//   automatically calls close(fd). This makes file descriptor leaks impossible.
//
//   Key POSIX socket flags set by this class:
//   - O_NONBLOCK  : Sockets are non-blocking. read()/write() return immediately
//                   with EAGAIN instead of sleeping. Required for our epoll event
//                   loop — we cannot afford a blocking socket to stall the reactor.
//   - SO_REUSEADDR: Allows keva-server to bind to port 6379 immediately after
//                   restart, even if the old socket is in TIME_WAIT state.
//   - TCP_NODELAY : Disables Nagle's algorithm. Nagle buffers small packets to
//                   coalesce them, which introduces 40-200ms artificial latency
//                   for small Redis-style request/response messages. Disabling it
//                   ensures RESP2 commands are sent immediately.
// ==============================================================================

#pragma once

#include "keva/common/types.hpp"
#include "keva/common/status.hpp"

#include <string_view>

namespace keva::net {

// --------------------------------------------------------------------------
// Socket — RAII file descriptor wrapper for TCP sockets
// --------------------------------------------------------------------------
class Socket {
public:
    // Invalid / uninitialized socket sentinel value
    static constexpr int INVALID_FD = -1;

    // Default-construct an empty (invalid) socket
    Socket() noexcept = default;

    // Construct from an existing raw file descriptor (e.g., from accept())
    explicit Socket(int fd) noexcept : fd_(fd) {}

    // Non-copyable: two objects cannot own the same fd (double-close bug)
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    // Move-constructible: transfers ownership; the moved-from socket becomes invalid
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    // Destructor: automatically closes the file descriptor if valid
    ~Socket();

    // --------------------------------------------------------------------------
    // Factory methods
    // --------------------------------------------------------------------------

    // Create a new TCP listening socket bound to host:port.
    // Sets SO_REUSEADDR, TCP_NODELAY, and O_NONBLOCK.
    // Returns an error Status if any syscall fails.
    static std::pair<Socket, Status> create_server(std::string_view host, u16 port);

    // --------------------------------------------------------------------------
    // Configuration helpers (called on raw accept()-returned sockets)
    // --------------------------------------------------------------------------

    // Set this socket to non-blocking mode (O_NONBLOCK via fcntl)
    Status set_nonblocking() noexcept;

    // Disable Nagle's algorithm on this socket (TCP_NODELAY via setsockopt)
    Status set_nodelay() noexcept;

    // --------------------------------------------------------------------------
    // I/O methods
    // --------------------------------------------------------------------------

    // Read up to 'len' bytes into 'buf'. Returns bytes read, 0 on EOF, -1 on error.
    // Callers must check errno for EAGAIN / EWOULDBLOCK (not an error, just no data).
    ssize_t read(void* buf, usize len) const noexcept;

    // Write up to 'len' bytes from 'buf'. Returns bytes written, -1 on error.
    // May return fewer bytes than requested (partial write) — callers must loop.
    ssize_t write(const void* buf, usize len) const noexcept;

    // --------------------------------------------------------------------------
    // Observers
    // --------------------------------------------------------------------------
    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ != INVALID_FD; }
    explicit operator bool() const noexcept { return valid(); }

private:
    void close_fd() noexcept;

    int fd_ = INVALID_FD;
};

} // namespace keva::net
