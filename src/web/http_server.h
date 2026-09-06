#pragma once

#include "http_socket.h"
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace meshcli {

struct HttpRequest {
    std::string method;
    std::string path;
    std::string query_string;
    std::map<std::string, std::string> query_params;
    std::map<std::string, std::string> headers;
    std::string body;

    [[nodiscard]] std::string get_header(const std::string& key) const;
    [[nodiscard]] std::string get_query(const std::string& key, const std::string& default_val = "") const;
};

struct HttpResponse {
    int status = 200;
    std::string status_text = "OK";
    std::map<std::string, std::string> headers;
    std::string body;

    void set_header(const std::string& key, const std::string& value);

    static HttpResponse json(int status, const nlohmann::json& val);
    static HttpResponse text(int status, const std::string& msg);
    static HttpResponse html(const std::string& content);
    static HttpResponse not_found(const std::string& msg = "Not Found");
    static HttpResponse bad_request(const std::string& msg = "Bad Request");
    static HttpResponse error(const std::string& msg = "Internal Server Error");
    static HttpResponse file(const std::string& content_type, const std::string& content);
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

class HttpServer {
public:
    HttpServer();
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Register routes
    void get(const std::string& path, HttpHandler handler);
    void post(const std::string& path, HttpHandler handler);
    void options(const std::string& path, HttpHandler handler);

    // Static asset directory
    void set_static_dir(const std::string& dir);

    // SSE endpoint path (default "/api/events")
    void set_sse_endpoint(const std::string& path);

    // Broadcast Server-Sent Event to all connected SSE clients
    void broadcast_sse(const std::string& event, const std::string& json_data);

    // Lifecycle
    bool start(const std::string& host, int port);
    void stop();
    [[nodiscard]] bool is_running() const { return running_; }
    [[nodiscard]] int bound_port() const { return bound_port_; }

private:
    enum class ConnState {
        ReadingRequest,
        WritingResponse,
        StreamingSse,
        Closed
    };

    struct Connection {
        socket_t fd = kInvalidSocket;
        ConnState state = ConnState::ReadingRequest;
        std::string in_buf;
        std::string out_buf;
        size_t out_written = 0;
        size_t expected_content_length = 0;
        bool headers_complete = false;
        HttpRequest request;
    };

    void worker_loop();
    void handle_accept();
    void handle_read(Connection& conn);
    void handle_write(Connection& conn);
    void process_request(Connection& conn);
    HttpResponse dispatch_route(const HttpRequest& req);
    HttpResponse serve_static(const std::string& path);

    std::string host_ = "0.0.0.0";
    int requested_port_ = 8080;
    int bound_port_ = 0;
    std::string static_dir_;
    std::string sse_endpoint_ = "/api/events";

    socket_t listen_fd_ = kInvalidSocket;
    std::atomic<bool> running_{false};
    std::thread worker_thread_;

    std::map<std::pair<std::string, std::string>, HttpHandler> routes_;
    std::mutex routes_mu_;

    std::vector<std::shared_ptr<Connection>> connections_;
    std::mutex conn_mu_;

    // Queued SSE broadcasts to write out
    std::vector<std::string> pending_sse_messages_;
    std::mutex sse_mu_;
};

} // namespace meshcli
