#include "stream_server.h"

#ifdef ENABLE_MESH_NET

#include "util/log.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/crypto.h>

#ifdef _WIN32
#define NOCRYPT
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#undef X509_NAME
#undef X509_CERT_PAIR
#undef X509_EXTENSIONS
#endif

namespace meshcli {

namespace {

// Generate a temporary self-signed certificate in memory for the server
bool generate_temp_cert(SSL_CTX* ctx) {
    EVP_PKEY* pkey = EVP_PKEY_new();
    RSA* rsa = RSA_generate_key(2048, RSA_F4, nullptr, nullptr);
    if (!EVP_PKEY_assign_RSA(pkey, rsa)) return false;

    X509* x509 = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 31536000L); // 1 year

    X509_set_pubkey(x509, pkey);
    X509_NAME* name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC, (unsigned char*)"US", -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, (unsigned char*)"fmesh-cli", -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (unsigned char*)"localhost", -1, -1, 0);
    X509_set_issuer_name(x509, name);

    if (!X509_sign(x509, pkey, EVP_sha256())) return false;

    SSL_CTX_use_certificate(ctx, x509);
    SSL_CTX_use_PrivateKey(ctx, pkey);

    X509_free(x509);
    EVP_PKEY_free(pkey);
    return true;
}



} // namespace

struct StreamServer::ClientConn {
    intptr_t fd{-1};
    SSL* ssl{nullptr};
    std::thread thread;
    std::atomic<bool> active{true};
    std::string ip;
    unsigned char frame_type{0xC3};
};

StreamServer::StreamServer(int port, std::string user, std::string password, EventSink sink)
    : port_(port), user_(std::move(user)), password_(std::move(password)), sink_(std::move(sink)) {
}

StreamServer::~StreamServer() {
    stop();
}

bool StreamServer::start() {
    if (running_) return false;

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    server_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        emit_error("Failed to create server socket");
        return false;
    }

    int opt = 1;
#ifndef _WIN32
    ::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#else
    ::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#endif

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (::bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        emit_error("Failed to bind server socket to port " + std::to_string(port_));
        return false;
    }

    if (::listen(server_fd_, 10) < 0) {
        emit_error("Failed to listen on server socket");
        return false;
    }

    running_ = true;
    accept_thread_ = std::thread(&StreamServer::accept_loop, this);

    EvLogLine ev;
    ev.source = "system";
    ev.message = "MeshServer started on port " + std::to_string(port_);
    emit(ev);

    return true;
}

void StreamServer::stop() {
    if (!running_) return;
    running_ = false;

    if (server_fd_ != -1) {
#ifdef _WIN32
        ::shutdown(server_fd_, SD_BOTH);
        ::closesocket(server_fd_);
#else
        ::shutdown(server_fd_, SHUT_RDWR);
        ::close(server_fd_);
#endif
        server_fd_ = -1;
    }

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }

    std::vector<std::shared_ptr<ClientConn>> to_close;
    {
        std::lock_guard<std::mutex> lock(clients_mu_);
        to_close = clients_;
    }
    for (auto& c : to_close) {
        c->active = false;
        if (c->fd != -1) {
#ifdef _WIN32
            ::shutdown(c->fd, SD_BOTH);
#else
            ::shutdown(c->fd, SHUT_RDWR);
#endif
        }
    }
    for (auto& c : to_close) {
        if (c->thread.joinable()) {
            c->thread.join();
        }
    }
    
    std::lock_guard<std::mutex> lock(clients_mu_);
    clients_.clear();
}

void StreamServer::broadcast(const std::string& bytes, unsigned char marker) {
    if (!running_) return;
    
    if (bytes.size() > 65000) {
        LOG_ERROR() << "StreamServer: broadcast payload too large (" << bytes.size() << " bytes), dropping to avoid framing truncation";
        return;
    }
    std::string framed;
    framed.reserve(4 + bytes.size());
    framed += static_cast<char>(0x94);
    framed += static_cast<char>(marker);
    uint16_t len = static_cast<uint16_t>(bytes.size());
    framed += static_cast<char>((len >> 8) & 0xFF);
    framed += static_cast<char>(len & 0xFF);
    framed += bytes;

    std::vector<std::shared_ptr<ClientConn>> current_clients;
    {
        std::lock_guard<std::mutex> lock(clients_mu_);
        current_clients = clients_;
    }

    for (auto& c : current_clients) {
        if (!c->active || !c->ssl) continue;
        int written = SSL_write(c->ssl, framed.data(), framed.size());
        if (written <= 0) {
            c->active = false;
        }
    }
}

void StreamServer::emit(MeshEvent ev) {
    if (sink_) sink_(std::move(ev));
}

void StreamServer::emit_error(std::string msg) {
    EvError ev;
    ev.message = std::move(msg);
    emit(ev);
}

void StreamServer::accept_loop() {
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        emit_error("Failed to create SSL_CTX");
        return;
    }

    // Harden TLS
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_cipher_list(ctx, "HIGH:!aNULL:!kRSA:!PSK:!SRP:!MD5:!RC4");

    if (!generate_temp_cert(ctx)) {
        emit_error("Failed to generate temporary SSL certificate");
        SSL_CTX_free(ctx);
        return;
    }

    while (running_) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        intptr_t client_fd = ::accept(server_fd_, (struct sockaddr*)&client_addr, &client_len);
        
        if (client_fd < 0) {
            if (running_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(clients_mu_);
            // Cleanup dead clients
            for (auto it = clients_.begin(); it != clients_.end(); ) {
                if (!(*it)->active) {
                    if ((*it)->thread.joinable()) {
                        (*it)->thread.join();
                    }
                    it = clients_.erase(it);
                } else {
                    ++it;
                }
            }
            
            if (clients_.size() >= 50) {
                // Too many connections
                EvLogLine ev_log;
                ev_log.source = "system";
                ev_log.message = "MeshServer: Rejecting connection, limit reached";
                emit(ev_log);
#ifdef _WIN32
                ::closesocket(client_fd);
#else
                ::close(client_fd);
#endif
                continue;
            }
        }

        // Set socket timeouts to mitigate Slowloris
#ifdef _WIN32
        DWORD timeout = 5000;
        ::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        ::setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
#else
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        ::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

        auto conn = std::make_shared<ClientConn>();
        conn->fd = client_fd;
        char ip_buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(client_addr.sin_addr), ip_buf, INET_ADDRSTRLEN);
        conn->ip = ip_buf;

        conn->ssl = SSL_new(ctx);
        SSL_set_fd(conn->ssl, static_cast<int>(client_fd));

        {
            std::lock_guard<std::mutex> lock(clients_mu_);
            clients_.push_back(conn);
        }

        conn->thread = std::thread([this, conn]() {
            if (SSL_accept(conn->ssl) <= 0) {
                conn->active = false;
            } else {
                // Authentication handshake
                char auth_buf[256];
                int auth_len = SSL_read(conn->ssl, auth_buf, sizeof(auth_buf) - 1);
                bool auth_ok = false;
                if (auth_len > 0) {
                    auth_buf[auth_len] = '\0';
                    std::string auth_str(auth_buf);
                    if (!auth_str.empty() && auth_str.back() == '\n') auth_str.pop_back();
                    if (!auth_str.empty() && auth_str.back() == '\r') auth_str.pop_back();
                    
                    std::string expected = "AUTH " + user_ + " " + password_;
                    if (auth_str.size() == expected.size() &&
                        CRYPTO_memcmp(auth_str.data(), expected.data(), expected.size()) == 0) {
                        auth_ok = true;
                        SSL_write(conn->ssl, "OK\n", 3);
                    }
                }
                
                if (!auth_ok) {
                    // Artificial delay for failed authentication to prevent brute force
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                    SSL_write(conn->ssl, "ERR\n", 4);
                    conn->active = false;
                } else {
                    EvLogLine ev_log;
                    ev_log.source = "system";
                    ev_log.message = "MeshServer: Client authenticated from " + conn->ip;
                    emit(ev_log);

                    // Remove timeouts for normal stream reading
#ifdef _WIN32
                    DWORD timeout_zero = 0;
                    ::setsockopt(conn->fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_zero, sizeof(timeout_zero));
                    ::setsockopt(conn->fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout_zero, sizeof(timeout_zero));
#else
                    struct timeval tv_zero;
                    tv_zero.tv_sec = 0;
                    tv_zero.tv_usec = 0;
                    ::setsockopt(conn->fd, SOL_SOCKET, SO_RCVTIMEO, &tv_zero, sizeof(tv_zero));
                    ::setsockopt(conn->fd, SOL_SOCKET, SO_SNDTIMEO, &tv_zero, sizeof(tv_zero));
#endif

                    // Connection established and authenticated, read framed stream
                    std::string buf;
                    buf.reserve(4096);
                    char read_buf[4096];
                    enum State { WaitStart1, WaitStart2, ReadLenHi, ReadLenLo, ReadPayload };
                    State state = WaitStart1;
                    uint16_t payload_len = 0;

                    while (conn->active && running_) {
                        int read_len = SSL_read(conn->ssl, read_buf, sizeof(read_buf));
                        if (read_len <= 0) {
                            conn->active = false;
                            break;
                        }
                        
                        buf.append(read_buf, read_len);
                        
                        // Parse framed chunks
                        while (running_ && buf.size() >= 4) {
                            if (state == WaitStart1) {
                                auto pos = buf.find(static_cast<char>(0x94));
                                if (pos == std::string::npos) {
                                    if (buf.size() > 1 && buf.size() <= 65536)
                                        buf = buf.substr(buf.size() - 1);
                                    else if (buf.size() > 65536) {
                                        buf.clear();
                                        state = WaitStart1;
                                    }
                                    break;
                                }
                                buf = buf.substr(pos);
                                state = WaitStart2;
                            }
                            if (state == WaitStart2 && buf.size() >= 2) {
                                unsigned char s2 = static_cast<unsigned char>(buf[1]);
                                if (s2 != 0xC3 && s2 != 0xD0) {
                                    buf = buf.substr(1);
                                    state = WaitStart1;
                                    continue;
                                }
                                conn->frame_type = s2;
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

                                if (conn->frame_type == 0xC3) {
                                    EvSendRawToRadio ev;
                                    ev.bytes = payload; // unframed payload!
                                    emit(ev);
                                } else if (conn->frame_type == 0xD0) {
                                    EvDbSyncPayload ev;
                                    ev.device = "meshserver"; // or something to identify it? We don't have device IDs per client yet.
                                    // Actually, StreamServer's emit goes to MeshService. 
                                    // It needs to be broadcasted to other clients or handled locally.
                                    ev.device = "meshserver:" + conn->ip; // unique enough
                                    ev.payload = payload;
                                    emit(ev);
                                }
                            } else if (state == ReadPayload) {
                                break;
                            }
                        }
                    }
                }
            }

            // Cleanup
            conn->active = false;
            if (conn->ssl) {
                SSL_shutdown(conn->ssl);
                SSL_free(conn->ssl);
                conn->ssl = nullptr;
            }
#ifdef _WIN32
            ::closesocket(conn->fd);
#else
            ::close(conn->fd);
#endif
            conn->fd = -1;
        });
    }

    SSL_CTX_free(ctx);
}

} // namespace meshcli

#endif
