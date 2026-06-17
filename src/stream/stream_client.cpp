#include "stream_client.h"
#include "mesh/mesh_codec.h"
#include "util/log.h"

#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <random>
#include <sys/socket.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>

namespace meshcli {

using namespace std::chrono_literals;

// ---- transport openers -------------------------------------------------

int tcp_connect(const std::string& host, uint16_t port) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    std::snprintf(port_str, sizeof(port_str), "%u", port);
    int rc = getaddrinfo(host.c_str(), port_str, &hints, &res);
    if (rc != 0) {
        LOG_ERROR() << "getaddrinfo(" << host << "): " << gai_strerror(rc);
        return -1;
    }
    int fd = -1;
    for (auto* rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        LOG_ERROR() << "tcp_connect(" << host << ":" << port << ") failed";
        return -1;
    }
    // Set non-blocking.
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    LOG_INFO() << "TCP connected to " << host << ":" << port;
    return fd;
}

int serial_open(const std::string& device, int baud) {
    int fd = open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        LOG_ERROR() << "serial_open(" << device << "): " << std::strerror(errno);
        return -1;
    }
    struct termios tio{};
    if (tcgetattr(fd, &tio) < 0) {
        LOG_ERROR() << "tcgetattr: " << std::strerror(errno);
        close(fd);
        return -1;
    }
    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL;
    speed_t speed;
    switch (baud) {
        case 1200:   speed = B1200;   break;
        case 2400:   speed = B2400;   break;
        case 4800:   speed = B4800;   break;
        case 9600:   speed = B9600;   break;
        case 19200:  speed = B19200;  break;
        case 38400:  speed = B38400;  break;
        case 57600:  speed = B57600;  break;
        case 115200: speed = B115200; break;
        case 230400: speed = B230400; break;
        case 460800: speed = B460800; break;
        default:     speed = B115200; break;
    }
    cfsetospeed(&tio, speed);
    cfsetispeed(&tio, speed);
    // 8N1
    tio.c_cflag &= ~CSIZE;
    tio.c_cflag |= CS8;
    tio.c_cflag &= ~PARENB;
    tio.c_cflag &= ~CSTOPB;
    tcflush(fd, TCIOFLUSH);
    if (tcsetattr(fd, TCSANOW, &tio) < 0) {
        LOG_ERROR() << "tcsetattr: " << std::strerror(errno);
        close(fd);
        return -1;
    }
    LOG_INFO() << "serial opened " << device << " @ " << baud;
    return fd;
}

// ---- StreamClient ------------------------------------------------------

StreamClient::StreamClient(int fd, std::string display_name, EventSink sink)
    : fd_(fd), display_name_(std::move(display_name)), sink_(std::move(sink)) {
    device_id_ = "stream:" + display_name_;
}

StreamClient::~StreamClient() { stop(); }

std::string StreamClient::start() {
    running_ = true;
    connected_ = true;

    EvConnected evc;
    evc.device = device_id_;
    evc.display_name = display_name_;
    emit(evc);

    thread_ = std::thread(&StreamClient::read_loop, this);

    // Send want_config to trigger the protocol handshake.
    uint32_t config_id = static_cast<uint32_t>(std::random_device{}());
    LOG_INFO() << "stream sending want_config_id=" << config_id;
    auto payload = MeshCodec::encode_want_config(config_id);
    send_to_radio(payload);

    return device_id_;
}

void StreamClient::stop() {
    if (!running_.exchange(false)) return;
    connected_ = false;
    if (fd_ >= 0) { close(fd_); fd_ = -1; }
    if (thread_.joinable()) thread_.join();
}

bool StreamClient::send_to_radio(const std::string& bytes) {
    if (!connected_ || fd_ < 0) return false;
    auto framed = frame(bytes);
    ssize_t n = ::write(fd_, framed.data(), framed.size());
    if (n < 0) {
        LOG_WARN() << "stream write error: " << std::strerror(errno);
        return false;
    }
    if (static_cast<size_t>(n) < framed.size()) {
        LOG_WARN() << "stream write short: " << n << " of " << framed.size();
        return false;
    }
    return true;
}

void StreamClient::read_loop() {
    // Buffer for assembling framed messages.
    std::string buf;
    buf.reserve(4096);
    enum State { WaitStart1, WaitStart2, ReadLenHi, ReadLenLo, ReadPayload };
    State state = WaitStart1;
    uint16_t payload_len = 0;

    LOG_INFO() << "stream read loop started (" << display_name_ << ")";

    while (running_) {
        char tmp[1024];
        struct pollfd pfd;
        pfd.fd = fd_;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, 500);
        if (pr < 0) {
            if (errno == EINTR) continue;
            emit_error("stream poll error: " + std::string(std::strerror(errno)));
            return;
        }
        if (pr == 0) continue;  // timeout

        ssize_t n = read(fd_, tmp, sizeof(tmp));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            emit_error("stream read error: " + std::string(std::strerror(errno)));
            return;
        }
        if (n == 0) {
            // EOF — connection closed by remote.
            LOG_INFO() << "stream EOF (" << display_name_ << ")";
            EvDisconnected ev;
            ev.device = device_id_;
            ev.reason = "remote closed";
            emit(ev);
            return;
        }
        buf.append(tmp, n);

        // Process any complete framed messages in the buffer.
        while (running_ && buf.size() >= 4) {
            if (state == WaitStart1) {
                // Find START1 (0x94).
                auto pos = buf.find(static_cast<char>(0x94));
                if (pos == std::string::npos) {
                    // No START1 in buffer — keep last byte (could be partial 0x94).
                    if (buf.size() > 1)
                        buf = buf.substr(buf.size() - 1);
                    break;
                }
                buf = buf.substr(pos);
                state = WaitStart2;
            }
            if (state == WaitStart2 && buf.size() >= 2) {
                if (static_cast<unsigned char>(buf[1]) != 0xC3) {
                    // Invalid START2 — skip this byte and retry.
                    buf = buf.substr(1);
                    state = WaitStart1;
                    continue;
                }
                buf = buf.substr(2);
                state = ReadLenHi;
            }
            if (state == ReadLenHi && buf.size() >= 1) {
                payload_len = static_cast<unsigned char>(buf[0]) << 8;
                buf = buf.substr(1);
                state = ReadLenLo;
            }
            if (state == ReadLenLo && buf.size() >= 1) {
                payload_len |= static_cast<unsigned char>(buf[0]);
                buf = buf.substr(1);
                state = ReadPayload;
            }
            if (state == ReadPayload && buf.size() >= payload_len) {
                std::string payload = buf.substr(0, payload_len);
                buf = buf.substr(payload_len);
                state = WaitStart1;

                // Decode and emit.
                uint32_t config_id = 0;
                auto ev = MeshCodec::decode_from_radio(payload, device_id_, config_id);
                if (ev) {
                    LOG_DEBUG() << "stream FromRadio: emitting (variant=" << ev->index() << ")";
                    emit(*ev);
                }
            } else if (state == ReadPayload) {
                // Not enough data yet — wait for more.
                break;
            }
        }
    }
}

void StreamClient::emit(MeshEvent ev) {
    if (sink_) sink_(std::move(ev));
}

void StreamClient::emit_error(std::string msg) {
    LOG_ERROR() << msg;
    EvError e;
    e.device = device_id_;
    e.message = std::move(msg);
    emit(e);
}

std::string StreamClient::frame(const std::string& payload) {
    std::string out;
    out.reserve(4 + payload.size());
    out += static_cast<char>(0x94);
    out += static_cast<char>(0xC3);
    uint16_t len = static_cast<uint16_t>(payload.size());
    out += static_cast<char>((len >> 8) & 0xFF);
    out += static_cast<char>(len & 0xFF);
    out += payload;
    return out;
}

} // namespace meshcli
