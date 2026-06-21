/*
 * Minimal Linux epoll server scaffold (single-threaded, non-blocking).
 *
 *   - Listens on a TCP port (host:port, IPv4)
 *   - accept()s new connections, registers them with epoll
 *   - Calls user-supplied callback whenever a connection has bytes ready
 *   - Closes connections on EOF or callback-requested teardown
 *
 * The callback receives:
 *     int fd                                   — the client socket
 *     const uint8_t* data, std::size_t n       — the chunk of bytes
 * and returns a continuation: KEEP, CLOSE.
 *
 * Why a custom scaffold instead of asio/libuv: this file is < 200 lines,
 * shows the actual epoll(7) API (epoll_create1, EPOLLIN | EPOLLET-style
 * level-triggered loop, EAGAIN handling), and keeps the lab dependency-
 * free.
 *
 * Production gaps (intentional, documented):
 *   - EPOLLET edge-triggered mode is NOT used here (simpler debugging,
 *     callers must drain in a loop themselves). Switch to EPOLLET when
 *     latency matters more than implementation complexity.
 *   - No write buffering: send() is assumed to complete in full. Real
 *     exchanges need an outbound queue per fd with EPOLLOUT.
 *   - No SO_REUSEPORT (single-listener; fine for the demo).
 */
#pragma once

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>


namespace net {

enum class Continuation { KEEP, CLOSE };

using OnDataFn = std::function<Continuation(int fd, const std::uint8_t* data, std::size_t n)>;


class EpollServer {
    int listen_fd_ = -1;
    int epoll_fd_  = -1;
    OnDataFn on_data_;
    bool     stop_ = false;

    static bool set_nonblock(int fd) noexcept {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
    }

    bool epoll_add(int fd, std::uint32_t events) noexcept {
        epoll_event ev{};
        ev.events  = events;
        ev.data.fd = fd;
        return ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) == 0;
    }

    void close_client(int fd) noexcept {
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        ::close(fd);
    }

public:
    EpollServer()  = default;
    ~EpollServer() { teardown(); }

    EpollServer(const EpollServer&)            = delete;
    EpollServer& operator=(const EpollServer&) = delete;
    EpollServer(EpollServer&&)                 = delete;
    EpollServer& operator=(EpollServer&&)      = delete;

    // listen: bind + listen on (host=0.0.0.0, given port). Returns false on any failure.
    bool listen(int port, int backlog = 32) noexcept {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return false;

        int yes = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port        = htons(static_cast<std::uint16_t>(port));

        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
        if (::listen(listen_fd_, backlog) != 0) return false;
        if (!set_nonblock(listen_fd_))         return false;

        epoll_fd_ = ::epoll_create1(0);
        if (epoll_fd_ < 0) return false;
        if (!epoll_add(listen_fd_, EPOLLIN)) return false;
        return true;
    }

    // run: poll loop; returns when stop() is called from another thread
    // or when the callback returns CLOSE on the last connection.
    // Returns the number of bytes processed (for diagnostics).
    std::uint64_t run(OnDataFn on_data, int timeout_ms = 100) {
        on_data_ = std::move(on_data);
        std::uint64_t bytes_seen = 0;

        constexpr int  MAX_EVENTS = 64;
        epoll_event    events[MAX_EVENTS];
        std::uint8_t   read_buf[4096];

        while (!stop_) {
            const int n = ::epoll_wait(epoll_fd_, events, MAX_EVENTS, timeout_ms);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            for (int i = 0; i < n; ++i) {
                const int fd = events[i].data.fd;
                if (fd == listen_fd_) {
                    // Drain ALL pending connections in a loop. With a non-blocking
                    // listener, accept() returns EAGAIN/EWOULDBLOCK once the backlog
                    // is empty — that's our stop condition. Accepting only one per
                    // wakeup would leave a connection burst queued until the next
                    // epoll_wait, adding latency under load.
                    for (;;) {
                        sockaddr_in caddr{};
                        socklen_t   clen = sizeof(caddr);
                        const int cfd = ::accept(listen_fd_,
                                                 reinterpret_cast<sockaddr*>(&caddr), &clen);
                        if (cfd < 0) break;        // EAGAIN/EWOULDBLOCK: no more pending
                        set_nonblock(cfd);
                        epoll_add(cfd, EPOLLIN);
                    }
                } else {
                    // data on existing connection
                    const ssize_t got = ::recv(fd, read_buf, sizeof(read_buf), 0);
                    if (got <= 0) {
                        close_client(fd);
                        continue;
                    }
                    bytes_seen += static_cast<std::uint64_t>(got);
                    if (on_data_(fd, read_buf, static_cast<std::size_t>(got)) == Continuation::CLOSE) {
                        close_client(fd);
                    }
                }
            }
        }
        return bytes_seen;
    }

    // stop: ask the loop to exit on its next iteration. Safe to call from
    // inside the data callback or from another thread. Does NOT close FDs
    // (that's teardown's job, run after the loop has actually exited).
    void stop() noexcept { stop_ = true; }

    // teardown: close epoll + listen FDs. Called by destructor; safe to
    // call manually after run() returns.
    void teardown() noexcept {
        if (epoll_fd_  >= 0) { ::close(epoll_fd_);  epoll_fd_  = -1; }
        if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
    }

    // Convenience send for echo / ack patterns from inside the callback.
    static bool send_all(int fd, const void* data, std::size_t n) noexcept {
        const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
        while (n > 0) {
            const ssize_t s = ::send(fd, p, n, MSG_NOSIGNAL);
            if (s <= 0) return false;
            p += s;
            n -= static_cast<std::size_t>(s);
        }
        return true;
    }
};

}  // namespace net
