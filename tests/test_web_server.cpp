#include "minitest.h"
#include "web/http_server.h"
#include "web/web_service.h"
#include "mesh/mesh_service.h"

#include <chrono>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

std::string http_client_request(int port, const std::string& raw_req) {
    socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalidSocket) return "";

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        meshcli::net::close_socket(s);
        return "";
    }

#ifdef _WIN32
    DWORD timeout_ms = 2000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif

    ::send(s, raw_req.c_str(), static_cast<int>(raw_req.size()), 0);

    std::string response;
    char buf[1024];
    while (true) {
        int n = ::recv(s, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = '\0';
        response.append(buf, static_cast<size_t>(n));
    }

    meshcli::net::close_socket(s);
    return response;
}

} // namespace

TEST(HttpServer, StartStop) {
    meshcli::HttpServer srv;
    EXPECT_TRUE(srv.start("127.0.0.1", 0)); // bind to ephemeral port
    EXPECT_TRUE(srv.is_running());
    EXPECT_GT(srv.bound_port(), 0);
    srv.stop();
    EXPECT_FALSE(srv.is_running());
}

TEST(HttpServer, GetAndPostRoutes) {
    meshcli::HttpServer srv;
    srv.get("/api/test", [](const meshcli::HttpRequest& req) {
        EXPECT_EQ(req.get_query("foo"), "bar");
        return meshcli::HttpResponse::json(200, {{"status", "ok"}, {"query", req.get_query("foo")}});
    });

    srv.post("/api/echo", [](const meshcli::HttpRequest& req) {
        auto j = nlohmann::json::parse(req.body);
        return meshcli::HttpResponse::json(200, {{"echo", j["msg"]}});
    });

    EXPECT_TRUE(srv.start("127.0.0.1", 0));
    int port = srv.bound_port();

    // Test GET
    std::string get_req = "GET /api/test?foo=bar HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    std::string res1 = http_client_request(port, get_req);
    EXPECT_NE(res1.find("200 OK"), std::string::npos);
    EXPECT_NE(res1.find("\"status\": \"ok\""), std::string::npos);

    // Test POST
    std::string post_body = "{\"msg\":\"hello mesh\"}";
    std::string post_req = "POST /api/echo HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: " +
                           std::to_string(post_body.size()) + "\r\n\r\n" + post_body;
    std::string res2 = http_client_request(port, post_req);
    EXPECT_NE(res2.find("200 OK"), std::string::npos);
    EXPECT_NE(res2.find("\"echo\": \"hello mesh\""), std::string::npos);

    // Test OPTIONS (CORS preflight)
    std::string opt_req = "OPTIONS /api/test HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    std::string res3 = http_client_request(port, opt_req);
    EXPECT_NE(res3.find("204 No Content"), std::string::npos);
    EXPECT_NE(res3.find("Access-Control-Allow-Origin: *"), std::string::npos);

    srv.stop();
}

TEST(HttpServer, SSEBroadcast) {
    meshcli::HttpServer srv;
    srv.set_sse_endpoint("/api/events");
    EXPECT_TRUE(srv.start("127.0.0.1", 0));
    int port = srv.bound_port();

    // Connect SSE client socket
    socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_NE(s, kInvalidSocket);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    EXPECT_EQ(::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

#ifdef _WIN32
    DWORD timeout_ms = 2000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif

    std::string sse_req = "GET /api/events HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    ::send(s, sse_req.c_str(), static_cast<int>(sse_req.size()), 0);

    // Give server moment to accept and upgrade to SSE
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Broadcast an event
    srv.broadcast_sse("test_event", "{\"ping\":123}");

    // Read response from socket
    char buf[1024];
    int n = ::recv(s, buf, sizeof(buf) - 1, 0);
    EXPECT_GT(n, 0);
    buf[n] = '\0';
    std::string sse_data(buf, static_cast<size_t>(n));

    EXPECT_NE(sse_data.find("text/event-stream"), std::string::npos);

    // Read subsequent broadcast packet if needed
    if (sse_data.find("test_event") == std::string::npos) {
        int n2 = ::recv(s, buf, sizeof(buf) - 1, 0);
        if (n2 > 0) {
            sse_data.append(buf, static_cast<size_t>(n2));
        }
    }
    EXPECT_NE(sse_data.find("test_event"), std::string::npos);
    EXPECT_NE(sse_data.find("{\"ping\":123}"), std::string::npos);

    meshcli::net::close_socket(s);
    srv.stop();
}

TEST(WebService, StatusAndNodes) {
    meshcli::MeshService service;
    meshcli::WebService web(service);
    EXPECT_TRUE(web.start("127.0.0.1", 0, "web"));
    int port = web.bound_port();

    std::string req = "GET /api/status HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    std::string res = http_client_request(port, req);
    EXPECT_NE(res.find("200 OK"), std::string::npos);
    EXPECT_NE(res.find("\"status\": \"online\""), std::string::npos);

    web.stop();
}

TEST(WebService, StaticFilesServing) {
    meshcli::MeshService service;
    meshcli::WebService web(service);
    EXPECT_TRUE(web.start("127.0.0.1", 0, "web"));
    int port = web.bound_port();

    std::string req = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    std::string res = http_client_request(port, req);
    EXPECT_NE(res.find("200 OK"), std::string::npos);
    EXPECT_NE(res.find("fmesh-cli | Mesh Network Dashboard"), std::string::npos);

    web.stop();
}

TEST(WebService, PacketActivityPipeline) {
    meshcli::MeshService service;
    meshcli::WebService web(service);
    EXPECT_TRUE(web.start("127.0.0.1", 0, "web"));
    int port = web.bound_port();

    // Record some mock packet activities
    meshcli::PacketActivity p1;
    p1.device = "test-device";
    p1.from_node = 0x11112222;
    p1.from_id = "!11112222";
    p1.to_node = 0x33334444;
    p1.to_id = "!33334444";
    p1.port_name = "TEXT_MESSAGE_APP";
    p1.summary = "Hello from Node 1";
    p1.rx_snr = 7.5f;
    p1.broadcast = false;
    p1.ts = 1000;
    web.record_and_broadcast_activity(p1);

    meshcli::PacketActivity p2;
    p2.device = "test-device";
    p2.from_node = 0x33334444;
    p2.from_id = "!33334444";
    p2.to_node = 0xFFFFFFFF;
    p2.to_id = "!ffffffff";
    p2.port_name = "POSITION_APP";
    p2.summary = "GPS Position";
    p2.broadcast = true;
    p2.ts = 1005;
    web.record_and_broadcast_activity(p2);

    // Verify recent_packets()
    auto packets = web.recent_packets();
    EXPECT_EQ(packets.size(), 2);
    EXPECT_EQ(packets[0].from_node, 0x11112222);
    EXPECT_EQ(packets[1].broadcast, true);

    // Query /api/packets via HTTP
    std::string req = "GET /api/packets HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    std::string res = http_client_request(port, req);
    EXPECT_NE(res.find("200 OK"), std::string::npos);
    EXPECT_NE(res.find("!11112222"), std::string::npos);
    EXPECT_NE(res.find("TEXT_MESSAGE_APP"), std::string::npos);
    EXPECT_NE(res.find("POSITION_APP"), std::string::npos);

    web.stop();
}

