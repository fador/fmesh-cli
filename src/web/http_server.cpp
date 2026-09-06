#include "http_server.h"
#include "util/log.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

namespace meshcli {

namespace {

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

std::string url_decode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            int h1 = in[i + 1];
            int h2 = in[i + 2];
            auto hex_val = [](int c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int v1 = hex_val(h1);
            int v2 = hex_val(h2);
            if (v1 >= 0 && v2 >= 0) {
                out += static_cast<char>((v1 << 4) | v2);
                i += 2;
                continue;
            }
        } else if (in[i] == '+') {
            out += ' ';
            continue;
        }
        out += in[i];
    }
    return out;
}

std::string get_mime_type(const std::string& path) {
    size_t dot = path.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = to_lower(path.substr(dot));
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".js") return "application/javascript; charset=utf-8";
    if (ext == ".json") return "application/json; charset=utf-8";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".woff") return "font/woff";
    if (ext == ".ttf") return "font/ttf";
    return "application/octet-stream";
}

} // namespace

std::string HttpRequest::get_header(const std::string& key) const {
    std::string lower_key = to_lower(key);
    for (const auto& [k, v] : headers) {
        if (to_lower(k) == lower_key) return v;
    }
    return "";
}

std::string HttpRequest::get_query(const std::string& key, const std::string& default_val) const {
    auto it = query_params.find(key);
    if (it != query_params.end()) return it->second;
    return default_val;
}

void HttpResponse::set_header(const std::string& key, const std::string& value) {
    headers[key] = value;
}

HttpResponse HttpResponse::json(int status, const nlohmann::json& val) {
    HttpResponse res;
    res.status = status;
    res.status_text = (status == 200) ? "OK" : (status == 201) ? "Created" : "Error";
    res.set_header("Content-Type", "application/json; charset=utf-8");
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
    res.body = val.dump(2);
    return res;
}

HttpResponse HttpResponse::text(int status, const std::string& msg) {
    HttpResponse res;
    res.status = status;
    res.status_text = (status == 200) ? "OK" : "Error";
    res.set_header("Content-Type", "text/plain; charset=utf-8");
    res.set_header("Access-Control-Allow-Origin", "*");
    res.body = msg;
    return res;
}

HttpResponse HttpResponse::html(const std::string& content) {
    HttpResponse res;
    res.status = 200;
    res.status_text = "OK";
    res.set_header("Content-Type", "text/html; charset=utf-8");
    res.body = content;
    return res;
}

HttpResponse HttpResponse::not_found(const std::string& msg) {
    nlohmann::json err = {{"error", msg}, {"status", 404}};
    HttpResponse res = json(404, err);
    res.status_text = "Not Found";
    return res;
}

HttpResponse HttpResponse::bad_request(const std::string& msg) {
    nlohmann::json err = {{"error", msg}, {"status", 400}};
    HttpResponse res = json(400, err);
    res.status_text = "Bad Request";
    return res;
}

HttpResponse HttpResponse::error(const std::string& msg) {
    nlohmann::json err = {{"error", msg}, {"status", 500}};
    HttpResponse res = json(500, err);
    res.status_text = "Internal Server Error";
    return res;
}

HttpResponse HttpResponse::file(const std::string& content_type, const std::string& content) {
    HttpResponse res;
    res.status = 200;
    res.status_text = "OK";
    res.set_header("Content-Type", content_type);
    res.set_header("Cache-Control", "no-cache");
    res.body = content;
    return res;
}

// -----------------------------------------------------------------------------

HttpServer::HttpServer() = default;

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::get(const std::string& path, HttpHandler handler) {
    std::lock_guard<std::mutex> lock(routes_mu_);
    routes_[{"GET", path}] = std::move(handler);
}

void HttpServer::post(const std::string& path, HttpHandler handler) {
    std::lock_guard<std::mutex> lock(routes_mu_);
    routes_[{"POST", path}] = std::move(handler);
}

void HttpServer::options(const std::string& path, HttpHandler handler) {
    std::lock_guard<std::mutex> lock(routes_mu_);
    routes_[{"OPTIONS", path}] = std::move(handler);
}

void HttpServer::set_static_dir(const std::string& dir) {
    static_dir_ = dir;
}

void HttpServer::set_sse_endpoint(const std::string& path) {
    sse_endpoint_ = path;
}

void HttpServer::broadcast_sse(const std::string& event, const std::string& json_data) {
    std::string frame;
    if (!event.empty()) {
        frame += "event: " + event + "\n";
    }
    frame += "data: " + json_data + "\n\n";

    std::lock_guard<std::mutex> lock(conn_mu_);
    for (auto& conn : connections_) {
        if (conn && conn->state == ConnState::StreamingSse) {
            conn->out_buf += frame;
        }
    }
}

bool HttpServer::start(const std::string& host, int port) {
    if (running_) return false;

    if (!net::init_networking()) {
        LOG_ERROR() << "HttpServer: failed to initialize network stack";
        return false;
    }

    host_ = host;
    requested_port_ = port;

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ == kInvalidSocket) {
        LOG_ERROR() << "HttpServer: failed to create listen socket";
        return false;
    }

    net::set_reuse_addr(listen_fd_);
    net::set_non_blocking(listen_fd_);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(requested_port_));
    if (host_.empty() || host_ == "0.0.0.0") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        inet_pton(AF_INET, host_.c_str(), &addr.sin_addr);
    }

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        LOG_ERROR() << "HttpServer: failed to bind to " << host_ << ":" << requested_port_;
        net::close_socket(listen_fd_);
        listen_fd_ = kInvalidSocket;
        return false;
    }

    if (::listen(listen_fd_, 64) != 0) {
        LOG_ERROR() << "HttpServer: failed to listen on socket";
        net::close_socket(listen_fd_);
        listen_fd_ = kInvalidSocket;
        return false;
    }

    // Determine bound port (especially if requested_port_ was 0)
    sockaddr_in bound_addr{};
#ifdef _WIN32
    int len = sizeof(bound_addr);
#else
    socklen_t len = sizeof(bound_addr);
#endif
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound_addr), &len) == 0) {
        bound_port_ = ntohs(bound_addr.sin_port);
    } else {
        bound_port_ = requested_port_;
    }

    running_ = true;
    worker_thread_ = std::thread(&HttpServer::worker_loop, this);

    LOG_INFO() << "HttpServer listening on http://" << host_ << ":" << bound_port_;
    return true;
}

void HttpServer::stop() {
    if (!running_.exchange(false)) return;

    if (listen_fd_ != kInvalidSocket) {
        net::close_socket(listen_fd_);
        listen_fd_ = kInvalidSocket;
    }

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(conn_mu_);
        for (auto& conn : connections_) {
            if (conn && conn->fd != kInvalidSocket) {
                net::close_socket(conn->fd);
                conn->fd = kInvalidSocket;
                conn->state = ConnState::Closed;
            }
        }
        connections_.clear();
    }

    LOG_INFO() << "HttpServer stopped";
}

void HttpServer::worker_loop() {
    while (running_) {
        std::vector<net::PollFd> poll_fds;
        std::vector<std::shared_ptr<Connection>> active_conns;

        // Add listening socket
        poll_fds.push_back({listen_fd_, net::kPollIn, 0});

        {
            std::lock_guard<std::mutex> lock(conn_mu_);
            // Cleanup closed connections
            connections_.erase(
                std::remove_if(connections_.begin(), connections_.end(),
                    [](const std::shared_ptr<Connection>& c) {
                        return !c || c->state == ConnState::Closed || c->fd == kInvalidSocket;
                    }),
                connections_.end());

            for (auto& conn : connections_) {
                if (!conn || conn->fd == kInvalidSocket) continue;

                short events = 0;
                if (conn->state == ConnState::ReadingRequest) {
                    events |= net::kPollIn;
                } else if (conn->state == ConnState::WritingResponse) {
                    events |= net::kPollOut;
                } else if (conn->state == ConnState::StreamingSse) {
                    // In SSE, check for socket errors / close via POLLIN, and POLLOUT if pending data
                    events |= net::kPollIn;
                    if (conn->out_written < conn->out_buf.size()) {
                        events |= net::kPollOut;
                    }
                }

                poll_fds.push_back({conn->fd, events, 0});
                active_conns.push_back(conn);
            }
        }

        int rc = net::poll_sockets(poll_fds, 50); // 50ms timeout
        if (rc < 0) {
            if (!running_) break;
            continue;
        }

        // Check listen socket
        if (poll_fds[0].revents & net::kPollIn) {
            handle_accept();
        }

        // Check client sockets
        for (size_t i = 1; i < poll_fds.size(); ++i) {
            auto& pfd = poll_fds[i];
            auto conn = active_conns[i - 1];

            if (pfd.revents & (net::kPollErr | net::kPollHup)) {
                conn->state = ConnState::Closed;
                net::close_socket(conn->fd);
                conn->fd = kInvalidSocket;
                continue;
            }

            if (pfd.revents & net::kPollIn) {
                handle_read(*conn);
            }

            if ((pfd.revents & net::kPollOut) && conn->state != ConnState::Closed) {
                handle_write(*conn);
            }
        }
    }
}

void HttpServer::handle_accept() {
    sockaddr_in client_addr{};
#ifdef _WIN32
    int len = sizeof(client_addr);
#else
    socklen_t len = sizeof(client_addr);
#endif
    socket_t client_fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &len);
    if (client_fd == kInvalidSocket) return;

    net::set_non_blocking(client_fd);
    net::set_tcp_nodelay(client_fd);

    auto conn = std::make_shared<Connection>();
    conn->fd = client_fd;
    conn->state = ConnState::ReadingRequest;

    std::lock_guard<std::mutex> lock(conn_mu_);
    connections_.push_back(conn);
}

void HttpServer::handle_read(Connection& conn) {
    char buf[4096];
    int n = ::recv(conn.fd, buf, sizeof(buf), 0);

    if (n <= 0) {
        if (n < 0 && net::is_would_block()) return;
        // Client closed connection
        conn.state = ConnState::Closed;
        net::close_socket(conn.fd);
        conn.fd = kInvalidSocket;
        return;
    }

    if (conn.state == ConnState::StreamingSse) {
        // SSE clients should not send payload; receiving data or close means disconnect
        conn.state = ConnState::Closed;
        net::close_socket(conn.fd);
        conn.fd = kInvalidSocket;
        return;
    }

    conn.in_buf.append(buf, static_cast<size_t>(n));

    // Check if headers complete
    if (!conn.headers_complete) {
        size_t header_end = conn.in_buf.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            if (conn.in_buf.size() > 65536) {
                // Header too large
                conn.state = ConnState::Closed;
                net::close_socket(conn.fd);
                conn.fd = kInvalidSocket;
            }
            return;
        }

        std::string raw_headers = conn.in_buf.substr(0, header_end);
        std::istringstream stream(raw_headers);
        std::string request_line;
        if (!std::getline(stream, request_line)) {
            conn.state = ConnState::Closed;
            net::close_socket(conn.fd);
            conn.fd = kInvalidSocket;
            return;
        }

        // Parse Request-Line: METHOD PATH HTTP/1.1
        std::istringstream line_stream(request_line);
        std::string method, full_path, proto;
        line_stream >> method >> full_path >> proto;

        conn.request.method = method;
        size_t qpos = full_path.find('?');
        if (qpos != std::string::npos) {
            conn.request.path = full_path.substr(0, qpos);
            conn.request.query_string = full_path.substr(qpos + 1);

            std::istringstream qstream(conn.request.query_string);
            std::string pair;
            while (std::getline(qstream, pair, '&')) {
                size_t eq = pair.find('=');
                if (eq != std::string::npos) {
                    conn.request.query_params[url_decode(pair.substr(0, eq))] =
                        url_decode(pair.substr(eq + 1));
                } else if (!pair.empty()) {
                    conn.request.query_params[url_decode(pair)] = "";
                }
            }
        } else {
            conn.request.path = full_path;
        }

        // Parse headers
        std::string header_line;
        while (std::getline(stream, header_line)) {
            size_t colon = header_line.find(':');
            if (colon != std::string::npos) {
                std::string k = trim(header_line.substr(0, colon));
                std::string v = trim(header_line.substr(colon + 1));
                conn.request.headers[k] = v;
            }
        }

        conn.headers_complete = true;

        std::string cl_str = conn.request.get_header("Content-Length");
        if (!cl_str.empty()) {
            try {
                conn.expected_content_length = std::stoul(cl_str);
            } catch (...) {
                conn.expected_content_length = 0;
            }
        }

        conn.in_buf = conn.in_buf.substr(header_end + 4);
    }

    // Check body completion
    if (conn.in_buf.size() >= conn.expected_content_length) {
        conn.request.body = conn.in_buf.substr(0, conn.expected_content_length);
        conn.in_buf.erase(0, conn.expected_content_length);
        process_request(conn);
    }
}

void HttpServer::process_request(Connection& conn) {
    const auto& req = conn.request;

    // Handle OPTIONS CORS preflight
    if (req.method == "OPTIONS") {
        HttpResponse res;
        res.status = 204;
        res.status_text = "No Content";
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS, PUT, DELETE");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
        res.set_header("Access-Control-Max-Age", "86400");

        std::string raw = "HTTP/1.1 204 No Content\r\n";
        for (const auto& [k, v] : res.headers) raw += k + ": " + v + "\r\n";
        raw += "Content-Length: 0\r\n\r\n";

        conn.out_buf = std::move(raw);
        conn.out_written = 0;
        conn.state = ConnState::WritingResponse;
        return;
    }

    // Handle Server-Sent Events endpoint
    if (req.method == "GET" && req.path == sse_endpoint_) {
        std::string raw =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache, no-transform\r\n"
            "Connection: keep-alive\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n"
            ": connected\n\n";

        conn.out_buf = std::move(raw);
        conn.out_written = 0;
        conn.state = ConnState::StreamingSse;
        return;
    }

    // Dispatch REST route or static file
    HttpResponse res = dispatch_route(req);

    // Format HTTP response
    std::string raw = "HTTP/1.1 " + std::to_string(res.status) + " " + res.status_text + "\r\n";
    if (res.headers.find("Content-Length") == res.headers.end()) {
        res.headers["Content-Length"] = std::to_string(res.body.size());
    }
    if (res.headers.find("Access-Control-Allow-Origin") == res.headers.end()) {
        res.headers["Access-Control-Allow-Origin"] = "*";
    }
    if (res.headers.find("Connection") == res.headers.end()) {
        res.headers["Connection"] = "close";
    }

    for (const auto& [k, v] : res.headers) {
        raw += k + ": " + v + "\r\n";
    }
    raw += "\r\n";
    raw += res.body;

    conn.out_buf = std::move(raw);
    conn.out_written = 0;
    conn.state = ConnState::WritingResponse;
}

HttpResponse HttpServer::dispatch_route(const HttpRequest& req) {
    HttpHandler handler = nullptr;
    {
        std::lock_guard<std::mutex> lock(routes_mu_);
        auto it = routes_.find({req.method, req.path});
        if (it != routes_.end()) {
            handler = it->second;
        } else {
            // Check prefix/wildcard routes if any
            for (const auto& [key, h] : routes_) {
                if (key.first == req.method && key.second.back() == '*' &&
                    req.path.rfind(key.second.substr(0, key.second.size() - 1), 0) == 0) {
                    handler = h;
                    break;
                }
            }
        }
    }

    if (handler) {
        try {
            return handler(req);
        } catch (const std::exception& e) {
            LOG_ERROR() << "HttpServer: route handler exception: " << e.what();
            return HttpResponse::error(e.what());
        }
    }

    // Fall back to static files
    if (req.method == "GET" && !static_dir_.empty()) {
        return serve_static(req.path);
    }

    return HttpResponse::not_found();
}

HttpResponse HttpServer::serve_static(const std::string& path) {
    std::string clean_path = path;
    if (clean_path.empty() || clean_path == "/") {
        clean_path = "/index.html";
    }

    // Security check: prevent directory traversal
    if (clean_path.find("..") != std::string::npos) {
        return HttpResponse::not_found();
    }

    std::string full_path = static_dir_;
    if (!full_path.empty() && full_path.back() == '/') full_path.pop_back();
    full_path += clean_path;

    std::ifstream file(full_path, std::ios::binary);
    if (!file.is_open()) {
        // Fallback: for SPA routing, serve index.html if not an asset request (no dot)
        if (clean_path.find('.') == std::string::npos) {
            std::string fallback = static_dir_ + "/index.html";
            std::ifstream fb_file(fallback, std::ios::binary);
            if (fb_file.is_open()) {
                std::ostringstream ss;
                ss << fb_file.rdbuf();
                return HttpResponse::file("text/html; charset=utf-8", ss.str());
            }
        }
        return HttpResponse::not_found();
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    return HttpResponse::file(get_mime_type(clean_path), ss.str());
}

void HttpServer::handle_write(Connection& conn) {
    if (conn.out_written >= conn.out_buf.size()) {
        if (conn.state == ConnState::WritingResponse) {
            // Finished sending standard HTTP response -> close connection
            conn.state = ConnState::Closed;
            net::close_socket(conn.fd);
            conn.fd = kInvalidSocket;
        } else if (conn.state == ConnState::StreamingSse) {
            // Flushed all queued SSE messages
            conn.out_buf.clear();
            conn.out_written = 0;
        }
        return;
    }

    size_t remaining = conn.out_buf.size() - conn.out_written;
    const char* ptr = conn.out_buf.data() + conn.out_written;

    int n = ::send(conn.fd, ptr, static_cast<int>(remaining), 0);
    if (n <= 0) {
        if (n < 0 && net::is_would_block()) return;
        // Error on write -> close socket
        conn.state = ConnState::Closed;
        net::close_socket(conn.fd);
        conn.fd = kInvalidSocket;
        return;
    }

    conn.out_written += static_cast<size_t>(n);

    if (conn.out_written >= conn.out_buf.size()) {
        if (conn.state == ConnState::WritingResponse) {
            conn.state = ConnState::Closed;
            net::close_socket(conn.fd);
            conn.fd = kInvalidSocket;
        } else if (conn.state == ConnState::StreamingSse) {
            conn.out_buf.clear();
            conn.out_written = 0;
        }
    }
}

} // namespace meshcli
