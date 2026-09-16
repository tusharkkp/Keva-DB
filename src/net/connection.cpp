// ==============================================================================
// src/net/connection.cpp
//
// Purpose:
//   Implements the Connection class declared in keva/net/connection.hpp.
//
//   Core buffer management patterns implemented here:
//
//   1. fill_read_buffer():
//      Calls read() in a tight loop until it returns EAGAIN (non-blocking
//      socket has no more data) or an error. This "read as much as possible"
//      strategy minimises the number of epoll_wait() wakeups required to fully
//      receive one large RESP2 array command.
//
//   2. flush_write_buffer():
//      Calls write() in a loop (handling partial writes) until EAGAIN or the
//      write buffer is drained. If EAGAIN is returned, we have filled the
//      kernel's TCP send buffer — we register EPOLLOUT interest on this
//      connection's fd so epoll notifies us when space is available.
//
//   3. consume(n):
//      Zero-copy: simply advances read_pos_ by n. No memory moves occur.
//      compact_read_buffer() is triggered lazily to shift the window.
//
//   4. append_response():
//      Copies response bytes into write_buf_. In Phase 2 (Threaded I/O),
//      this is the method I/O worker threads call in parallel.
// ==============================================================================

#include "keva/net/connection.hpp"
#include "keva/common/logger.hpp"

#include <cerrno>
#include <cstring>
#include <algorithm>

namespace keva::net {

// --------------------------------------------------------------------------
// Constructor — reserve initial buffer capacity to avoid early reallocations
// --------------------------------------------------------------------------
Connection::Connection(Socket socket, u64 client_id) noexcept
    : socket_(std::move(socket))
    , client_id_(client_id)
{
    read_buf_.reserve(constants::READ_BUFFER_SIZE);
    write_buf_.reserve(constants::WRITE_BUFFER_SIZE);
}

// --------------------------------------------------------------------------
// fill_read_buffer — drain the kernel socket receive buffer into read_buf_
// --------------------------------------------------------------------------
ssize_t Connection::fill_read_buffer() {
    // Compact if the consumed prefix is taking more than half the buffer
    if (read_pos_ > read_buf_.size() / 2 && read_pos_ > 0) {
        compact_read_buffer();
    }

    ssize_t total_read = 0;

    // Read in a loop because a single epoll notification may signal more
    // than READ_BUFFER_SIZE bytes waiting in the kernel receive buffer.
    while (true) {
        // Grow the buffer to ensure there is space for at least READ_BUFFER_SIZE
        // bytes beyond the current written end
        const usize current_size = read_buf_.size();
        const usize needed       = current_size + constants::READ_BUFFER_SIZE;
        read_buf_.resize(needed);

        ssize_t n = socket_.read(
            read_buf_.data() + current_size,
            constants::READ_BUFFER_SIZE
        );

        if (n > 0) {
            // Trim back to actual bytes written
            read_buf_.resize(current_size + static_cast<usize>(n));
            total_read += n;
            // Continue reading to drain as much as possible in one epoll event
        } else if (n == 0) {
            // EOF: the client closed the TCP connection gracefully
            read_buf_.resize(current_size);  // Undo the speculative resize
            state_ = ConnectionState::Closed;
            return 0;
        } else {
            // n < 0: error or EAGAIN/EWOULDBLOCK
            read_buf_.resize(current_size);  // Undo the speculative resize
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // No more data available right now — not an error
                break;
            }
            // Real I/O error
            state_ = ConnectionState::Closed;
            return -1;
        }
    }

    return total_read;
}

// --------------------------------------------------------------------------
// flush_write_buffer — send pending write_buf_ bytes to the client
// --------------------------------------------------------------------------
Status Connection::flush_write_buffer() {
    while (write_pos_ < write_buf_.size()) {
        const usize remaining = write_buf_.size() - write_pos_;
        ssize_t n = socket_.write(write_buf_.data() + write_pos_, remaining);

        if (n > 0) {
            write_pos_ += static_cast<usize>(n);
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Kernel TCP send buffer is full. The caller (EventLoop) must
                // register EPOLLOUT interest to be notified when space opens up.
                break;
            }
            state_ = ConnectionState::Closed;
            return Status::io_error(strerror(errno));
        }
    }

    // If all bytes were sent, compact the write buffer
    if (write_pos_ >= write_buf_.size()) {
        write_buf_.clear();
        write_pos_ = 0;
    }

    return Status::success();
}

// --------------------------------------------------------------------------
// read_view — return a string_view of unconsumed bytes in the read buffer
// --------------------------------------------------------------------------
std::string_view Connection::read_view() const noexcept {
    const char* start = reinterpret_cast<const char*>(read_buf_.data()) + read_pos_;
    const usize len   = read_buf_.size() - read_pos_;
    return {start, len};
}

// --------------------------------------------------------------------------
// consume — advance the parser cursor (zero-copy)
// --------------------------------------------------------------------------
void Connection::consume(usize n) noexcept {
    read_pos_ += n;
    // Safety clamp: never advance past the end of the buffer
    if (read_pos_ > read_buf_.size()) {
        read_pos_ = read_buf_.size();
    }
}

// --------------------------------------------------------------------------
// append_response — push response bytes into the write buffer
// --------------------------------------------------------------------------
void Connection::append_response(const void* data, usize len) {
    const auto* bytes = static_cast<const KevaByte*>(data);
    write_buf_.insert(write_buf_.end(), bytes, bytes + len);
}

void Connection::append_response(std::string_view sv) {
    append_response(sv.data(), sv.size());
}

// --------------------------------------------------------------------------
// has_pending_writes — check if write buffer has unsent data
// --------------------------------------------------------------------------
bool Connection::has_pending_writes() const noexcept {
    return write_pos_ < write_buf_.size();
}

// --------------------------------------------------------------------------
// compact_read_buffer — shift unconsumed bytes to the front
// --------------------------------------------------------------------------
void Connection::compact_read_buffer() {
    if (read_pos_ == 0) return;
    const usize remaining = read_buf_.size() - read_pos_;
    if (remaining > 0) {
        // std::move overlapping memory: safe with memmove semantics
        std::memmove(read_buf_.data(), read_buf_.data() + read_pos_, remaining);
    }
    read_buf_.resize(remaining);
    read_pos_ = 0;
}

} // namespace keva::net
