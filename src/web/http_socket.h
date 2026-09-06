#pragma once

#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

namespace meshcli {
namespace net {

// Initialize network subsystem (WSAStartup on Windows, no-op on POSIX)
bool init_networking();

// Cleanup network subsystem
void cleanup_networking();

// Close socket handle safely
void close_socket(socket_t fd);

// Set non-blocking mode
bool set_non_blocking(socket_t fd);

// Set TCP_NODELAY
bool set_tcp_nodelay(socket_t fd);

// Set SO_REUSEADDR
bool set_reuse_addr(socket_t fd);

// Returns true if the last socket error indicates a non-fatal would-block state
bool is_would_block();

// Portable poll wrapper
struct PollFd {
    socket_t fd;
    short events;
    short revents;
};

#ifdef _WIN32
constexpr short kPollIn = POLLRDNORM | POLLRDBAND;
constexpr short kPollOut = POLLWRNORM;
constexpr short kPollErr = POLLERR;
constexpr short kPollHup = POLLHUP;
#else
constexpr short kPollIn = POLLIN;
constexpr short kPollOut = POLLOUT;
constexpr short kPollErr = POLLERR;
constexpr short kPollHup = POLLHUP;
#endif

int poll_sockets(std::vector<PollFd>& fds, int timeout_ms);

} // namespace net
} // namespace meshcli
