#include "http_socket.h"

namespace meshcli {
namespace net {

bool init_networking() {
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    return true;
#endif
}

void cleanup_networking() {
#ifdef _WIN32
    WSACleanup();
#endif
}

void close_socket(socket_t fd) {
    if (fd == kInvalidSocket) return;
#ifdef _WIN32
    ::closesocket(fd);
#else
    ::close(fd);
#endif
}

bool set_non_blocking(socket_t fd) {
    if (fd == kInvalidSocket) return false;
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
#endif
}

bool set_tcp_nodelay(socket_t fd) {
    if (fd == kInvalidSocket) return false;
    int opt = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&opt), sizeof(opt)) == 0;
}

bool set_reuse_addr(socket_t fd) {
    if (fd == kInvalidSocket) return false;
    int opt = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt)) == 0;
}

bool is_would_block() {
#ifdef _WIN32
    int err = WSAGetLastError();
    return (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS);
#else
    return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS);
#endif
}

int poll_sockets(std::vector<PollFd>& fds, int timeout_ms) {
    if (fds.empty()) return 0;
#ifdef _WIN32
    std::vector<WSAPOLLFD> win_fds(fds.size());
    for (size_t i = 0; i < fds.size(); ++i) {
        win_fds[i].fd = fds[i].fd;
        win_fds[i].events = fds[i].events;
        win_fds[i].revents = 0;
    }
    int rc = WSAPoll(win_fds.data(), static_cast<ULONG>(win_fds.size()), timeout_ms);
    if (rc > 0) {
        for (size_t i = 0; i < fds.size(); ++i) {
            fds[i].revents = win_fds[i].revents;
        }
    }
    return rc;
#else
    std::vector<pollfd> posix_fds(fds.size());
    for (size_t i = 0; i < fds.size(); ++i) {
        posix_fds[i].fd = fds[i].fd;
        posix_fds[i].events = fds[i].events;
        posix_fds[i].revents = 0;
    }
    int rc = poll(posix_fds.data(), static_cast<nfds_t>(posix_fds.size()), timeout_ms);
    if (rc > 0) {
        for (size_t i = 0; i < fds.size(); ++i) {
            fds[i].revents = posix_fds[i].revents;
        }
    }
    return rc;
#endif
}

} // namespace net
} // namespace meshcli
