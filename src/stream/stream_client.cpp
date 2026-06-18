#include "stream_client.h"
#include "mesh/mesh_codec.h"
#include "util/log.h"

#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <random>
#include <thread>
#include <sys/types.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#define close closesocket
#define read(fd, buf, len) recv(fd, buf, len, 0)
#define write(fd, buf, len) send(fd, (const char*)buf, len, 0)
#define poll WSAPoll
#else
#include <poll.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>
#include <random>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#endif

#ifdef ENABLE_MESH_NET
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

namespace meshcli {

using namespace std::chrono_literals;

// ---- transport openers -------------------------------------------------

intptr_t tcp_connect(const std::string& host, uint16_t port) {
#ifdef _WIN32
    static bool wsa_init = false;
    if (!wsa_init) {
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
        wsa_init = true;
    }
#endif
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
    intptr_t fd = -1;
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
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
    LOG_INFO() << "TCP connected to " << host << ":" << port;
    return fd;
}

intptr_t serial_open(const std::string& device, int baud) {
#ifdef _WIN32
    LOG_ERROR() << "Serial port is not yet supported on Windows";
    return -1;
#else
    intptr_t fd = open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
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
#endif
}

// ---- StreamClient ------------------------------------------------------

StreamClient::StreamClient(intptr_t fd, std::string display_name, EventSink sink)
    : fd_(fd), display_name_(std::move(display_name)), sink_(std::move(sink)) {
    device_id_ = "stream:" + display_name_;
}

StreamClient::~StreamClient() { stop(); }

#ifdef ENABLE_MESH_NET
void StreamClient::enable_tls(const std::string& user, const std::string& password) {
    use_tls_ = true;
    tls_user_ = user;
    tls_password_ = password;
}
#endif

std::string StreamClient::start() {
    running_ = true;
    connected_ = true;

#ifdef ENABLE_MESH_NET
    if (use_tls_) {
        ssl_ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ssl_ctx_) {
            emit_error("Failed to create SSL context");
            return "";
        }
        
        // Harden TLS
        SSL_CTX_set_min_proto_version(ssl_ctx_, TLS1_2_VERSION);
        SSL_CTX_set_cipher_list(ssl_ctx_, "HIGH:!aNULL:!kRSA:!PSK:!SRP:!MD5:!RC4");

        // Do not verify certificates for this simple mesh demo
        SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_NONE, nullptr);
        
        // Set socket timeouts for connection phase
#ifdef _WIN32
        DWORD timeout = 5000;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
#else
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

        ssl_ = SSL_new(ssl_ctx_);
        SSL_set_fd(ssl_, static_cast<int>(fd_));
        if (SSL_connect(ssl_) <= 0) {
            emit_error("SSL handshake failed");
            return "";
        }
        std::string auth_cmd = "AUTH " + tls_user_ + " " + tls_password_ + "\n";
        SSL_write(ssl_, auth_cmd.c_str(), auth_cmd.size());
        char resp[16] = {0};
        SSL_read(ssl_, resp, sizeof(resp) - 1);
        if (std::string(resp).find("OK") == std::string::npos) {
            emit_error("Mesh authentication failed");
            return "";
        }
        LOG_INFO() << "Mesh TLS authentication successful";
    }
#endif

    EvConnected evc;
    evc.device = device_id_;
    evc.display_name = display_name_;
    emit(evc);

    thread_ = std::thread(&StreamClient::read_loop, this);

    // Send want_config to trigger the protocol handshake.
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(1, 0xFFFFFFFF);
    uint32_t config_id = dist(gen);
    LOG_INFO() << "stream sending want_config_id=" << config_id;
    auto payload = MeshCodec::encode_want_config(config_id);
    send_to_radio(payload);

    return device_id_;
}

void StreamClient::stop() {
    if (!running_.exchange(false)) return;
    connected_ = false;
#ifdef ENABLE_MESH_NET
    if (use_tls_ && ssl_) {
        SSL_shutdown(ssl_);
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (use_tls_ && ssl_ctx_) {
        SSL_CTX_free(ssl_ctx_);
        ssl_ctx_ = nullptr;
    }
#endif
    if (fd_ >= 0) { close(fd_); fd_ = -1; }
    if (thread_.joinable()) thread_.join();
}

bool StreamClient::send_to_radio(const std::string& bytes) {
    if (!connected_ || fd_ < 0) return false;
    auto framed = frame(bytes);
    size_t written = 0;
    int retries = 0;
    static constexpr int kMaxRetries = 10;
    while (written < framed.size() && retries < kMaxRetries) {
        ssize_t n = -1;
#ifdef ENABLE_MESH_NET
        if (use_tls_ && ssl_) {
            n = SSL_write(ssl_, framed.data() + written, framed.size() - written);
            if (n <= 0) {
                int err = SSL_get_error(ssl_, n);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                    n = -1; // handle as EAGAIN
                    errno = EAGAIN;
                }
            }
        } else {
            n = ::write(fd_, framed.data() + written, framed.size() - written);
        }
#else
        n = ::write(fd_, framed.data() + written, framed.size() - written);
#endif
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                ++retries;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
#ifdef _WIN32
            if (WSAGetLastError() == WSAEWOULDBLOCK) {
                ++retries;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
#endif
            LOG_WARN() << "stream write error: " << std::strerror(errno);
            return false;
        }
        written += n;
        retries = 0;
    }
    if (written < framed.size()) {
        LOG_WARN() << "stream write short after retries: " << written << " of " << framed.size();
        return false;
    }
    return true;
}

void StreamClient::read_loop() {
    static constexpr size_t kMaxBufBytes = 65536;
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

        ssize_t n = -1;
#ifdef ENABLE_MESH_NET
        if (use_tls_ && ssl_) {
            n = SSL_read(ssl_, tmp, sizeof(tmp));
            if (n <= 0) {
                int err = SSL_get_error(ssl_, n);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                    n = -1;
                    errno = EAGAIN;
                } else {
                    n = 0; // treat as EOF
                }
            }
        } else {
            n = read(fd_, tmp, sizeof(tmp));
        }
#else
        n = read(fd_, tmp, sizeof(tmp));
#endif
        if (n < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEWOULDBLOCK || errno == EAGAIN || errno == EWOULDBLOCK) continue;
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
#endif
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
                    if (buf.size() > 1 && buf.size() <= kMaxBufBytes)
                        buf = buf.substr(buf.size() - 1);
                    else if (buf.size() > kMaxBufBytes) {
                        LOG_WARN() << "stream buffer overflow (> " << kMaxBufBytes
                                   << " bytes), discarding";
                        buf.clear();
                        state = WaitStart1;
                    }
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
