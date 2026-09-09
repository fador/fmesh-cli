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

TEST(WebService, MessageAndConversationsApi) {
    meshcli::MeshService service;
    service.open_database(":memory:");
    auto& db = service.database();

    // Insert a channel broadcast message with empty device string
    meshcli::StoredMessage m1;
    m1.device = "";
    m1.window_kind = "channel";
    m1.window_target = 0;
    m1.direction = "in";
    m1.from_node = 0x12345678;
    m1.to_node = 0xFFFFFFFF;
    m1.channel_idx = 0;
    m1.text = "Broadcast alert on Primary";
    m1.ts = 1000;
    m1.packet_id = 9991;
    m1.rx_snr = 8.5f;
    m1.rx_rssi = -70;
    m1.hop_start = 3;
    m1.hop_limit = 3;
    m1.relay_node = 0;
    db.insert_message(m1);

    // Insert a DM with full stream device id
    meshcli::StoredMessage m2;
    m2.device = "stream:mesh:192.168.178.23:4404";
    m2.window_kind = "dm";
    m2.window_target = 0xAABBCCDD;
    m2.direction = "out";
    m2.from_node = 0x12345678;
    m2.to_node = 0xAABBCCDD;
    m2.channel_idx = 0;
    m2.text = "Hello Direct Message";
    m2.ts = 1010;
    m2.packet_id = 9992;
    db.insert_message(m2);

    meshcli::WebService web(service);
    EXPECT_TRUE(web.start("127.0.0.1", 0, ""));
    int port = web.bound_port();

    // 1. Query channel messages with device parameter - should return m1 even though m1.device is empty
    std::string req1 = "GET /api/messages?device=stream:mesh:192.168.178.23:4404&kind=channel&target=0 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    std::string res1 = http_client_request(port, req1);
    EXPECT_NE(res1.find("200 OK"), std::string::npos);
    EXPECT_NE(res1.find("Broadcast alert on Primary"), std::string::npos);
    EXPECT_NE(res1.find("\"rx_snr\": 8.5"), std::string::npos);
    EXPECT_NE(res1.find("\"rx_rssi\": -70"), std::string::npos);
    EXPECT_NE(res1.find("\"hops\": 0"), std::string::npos);

    // 2. Query DM messages
    std::string req2 = "GET /api/messages?device=stream:mesh:192.168.178.23:4404&kind=dm&target=" + std::to_string(0xAABBCCDD) + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    std::string res2 = http_client_request(port, req2);
    EXPECT_NE(res2.find("200 OK"), std::string::npos);
    EXPECT_NE(res2.find("Hello Direct Message"), std::string::npos);

    // 3. Query conversations list
    std::string req3 = "GET /api/conversations?device=stream:mesh:192.168.178.23:4404 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    std::string res3 = http_client_request(port, req3);
    EXPECT_NE(res3.find("200 OK"), std::string::npos);
    EXPECT_NE(res3.find("\"channels\""), std::string::npos);
    EXPECT_NE(res3.find("\"dms\""), std::string::npos);
    EXPECT_NE(res3.find(std::to_string(0xAABBCCDD)), std::string::npos);

    // 4. Query channel messages with completely mismatched device ID - should still return broadcast messages
    std::string req4 = "GET /api/messages?device=completely_different_device&kind=channel&target=0 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    std::string res4 = http_client_request(port, req4);
    EXPECT_NE(res4.find("200 OK"), std::string::npos);
    EXPECT_NE(res4.find("Broadcast alert on Primary"), std::string::npos);

    web.stop();
}

TEST(WebService, RfLinksApi) {
    meshcli::MeshService service;
    const std::string dev = "test_dev";
    uint32_t my_node = 0x11111111;

    auto rt = std::make_shared<meshcli::DeviceRuntime>();
    rt->db = std::make_unique<meshcli::NodeDb>();
    rt->display_name = dev;
    rt->my_node_num = my_node;
    rt->db->set_my_node_num(my_node);

    // Add direct neighbor (0 hops away)
    meshcli::Node n_direct;
    n_direct.node_num = 0x22222222;
    n_direct.hops_away = 0;
    n_direct.snr = 8.5f;
    n_direct.latitude = 60.1;
    n_direct.longitude = 24.9;
    rt->db->upsert_node(n_direct);

    // Add indirect node (2 hops away via repeaters)
    meshcli::Node n_indirect;
    n_indirect.node_num = 0x33333333;
    n_indirect.hops_away = 2;
    n_indirect.snr = -2.0f;
    n_indirect.latitude = 60.2;
    n_indirect.longitude = 25.0;
    rt->db->upsert_node(n_indirect);

    {
        std::lock_guard<std::mutex> lock(service.devices_mu_for_test());
        service.devices_for_test()[dev] = rt;
    }

    meshcli::WebService web(service);
    EXPECT_TRUE(web.start("127.0.0.1", 0, ""));
    int port = web.bound_port();

    // 1. Initial links should only have direct neighbor to my_node, not indirect
    auto links1 = web.get_links(dev);
    EXPECT_EQ(links1.size(), 1u);
    EXPECT_EQ(links1[0].from_node, my_node);
    EXPECT_EQ(links1[0].to_node, 0x22222222);
    EXPECT_EQ(links1[0].source, "direct");

    // 2. Simulate traceroute discovery to 0x33333333 via 0x22222222
    meshcli::EvTracerouteReceived tr;
    tr.device = dev;
    tr.from_node = 0x33333333;
    tr.to_node = my_node;
    tr.route = { 0x22222222 };
    tr.snr_towards = { 8.5f, 3.2f };
    service.dispatch_to_ui(tr);

    // 3. Now links should contain both real hops: my_node <-> 0x22222222 and 0x22222222 <-> 0x33333333
    auto links2 = web.get_links(dev);
    EXPECT_EQ(links2.size(), 2u);

    // Verify via HTTP GET /api/links
    std::string req = "GET /api/links?device=" + dev + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    std::string res = http_client_request(port, req);
    EXPECT_NE(res.find("200 OK"), std::string::npos);
    EXPECT_NE(res.find("!22222222"), std::string::npos);
    EXPECT_NE(res.find("!33333333"), std::string::npos);
    EXPECT_NE(res.find("traceroute"), std::string::npos);

    web.stop();
}

TEST(WebService, TelemetryApi) {
    meshcli::MeshService service;
    service.open_database(":memory:");
    auto& db = service.database();

    meshcli::Database::TelemetryRow r1;
    r1.device = "dev1";
    r1.node_num = 0x12345678;
    r1.ts = 1000;
    r1.temperature = 21.5f;
    r1.relative_humidity = 50.0f;
    r1.barometric_pressure = 1012.0f;
    r1.battery_level = 95;
    r1.voltage = 4.1f;
    db.insert_telemetry(r1);

    meshcli::Database::TelemetryRow r2;
    r2.device = "dev1";
    r2.node_num = 0x87654321;
    r2.ts = 1010;
    r2.temperature = 19.8f;
    r2.battery_level = 80;
    db.insert_telemetry(r2);

    meshcli::WebService web(service);
    EXPECT_TRUE(web.start("127.0.0.1", 0, ""));
    int port = web.bound_port();

    // Query telemetry for node !12345678
    std::string req1 = "GET /api/telemetry?node_num=!12345678 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    std::string res1 = http_client_request(port, req1);
    EXPECT_NE(res1.find("200 OK"), std::string::npos);
    EXPECT_NE(res1.find("!12345678"), std::string::npos);
    EXPECT_NE(res1.find("21.5"), std::string::npos);
    EXPECT_NE(res1.find("1012"), std::string::npos);
    EXPECT_EQ(res1.find("!87654321"), std::string::npos);

    // Query all telemetry
    std::string req2 = "GET /api/telemetry HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    std::string res2 = http_client_request(port, req2);
    EXPECT_NE(res2.find("200 OK"), std::string::npos);
    EXPECT_NE(res2.find("!12345678"), std::string::npos);
    EXPECT_NE(res2.find("!87654321"), std::string::npos);

    web.stop();
}

TEST(WebService, ExcludeLocalNodeFromPacketActivity) {
    meshcli::MeshService service;
    service.open_database(":memory:");

    std::string dev = "test_dev";
    auto rt = std::make_shared<meshcli::DeviceRuntime>();
    rt->my_node_num = 0x11112222;
    rt->db = std::make_unique<meshcli::NodeDb>();
    rt->db->set_my_node_num(0x11112222);

    {
        std::lock_guard<std::mutex> lock(service.devices_mu_for_test());
        service.devices_for_test()[dev] = rt;
    }

    meshcli::WebService web(service);
    EXPECT_TRUE(web.start("127.0.0.1", 0, ""));

    // 1. Simulate local node keepalive / telemetry event
    meshcli::EvNodeUpdated local_ev;
    local_ev.device = dev;
    local_ev.node.node_num = 0x11112222;
    local_ev.node.node_id = "!11112222";
    local_ev.node.battery_level = 99;
    local_ev.node.voltage = 4.15f;
    local_ev.is_new = false;
    service.dispatch_to_ui(local_ev);

    // 2. Simulate local node position update
    meshcli::EvPositionReceived local_pos;
    local_pos.device = dev;
    local_pos.from_node = 0x11112222;
    local_pos.latitude = 60.1;
    local_pos.longitude = 24.9;
    service.dispatch_to_ui(local_pos);

    // Verify recent_packets() is STILL EMPTY because local node events are excluded
    auto packets = web.recent_packets();
    EXPECT_EQ(packets.size(), 0u);

    // 3. Simulate remote node heard over the RF mesh
    meshcli::EvNodeUpdated remote_ev;
    remote_ev.device = dev;
    remote_ev.node.node_num = 0x99998888;
    remote_ev.node.node_id = "!99998888";
    remote_ev.node.long_name = "Remote Node";
    remote_ev.node.battery_level = 75;
    remote_ev.is_new = true;
    service.dispatch_to_ui(remote_ev);

    // Verify recent_packets() now contains the remote RF packet
    packets = web.recent_packets();
    ASSERT_EQ(packets.size(), 1u);
    EXPECT_EQ(packets[0].from_node, 0x99998888);
    EXPECT_EQ(packets[0].port_name, "NODEINFO_APP");

    web.stop();
}

TEST(WebService, StatsApi) {
    meshcli::MeshService service;
    EXPECT_TRUE(service.open_database(":memory:"));

    std::string dev = "test-device";
    auto rt = std::make_shared<meshcli::DeviceRuntime>();
    rt->my_node_num = 0x11112222;
    rt->db = std::make_unique<meshcli::NodeDb>();
    rt->db->set_my_node_num(0x11112222);

    meshcli::Node remote_node;
    remote_node.node_num = 0x22223333;
    remote_node.node_id = "!22223333";
    remote_node.long_name = "Remote Node Alpha";
    remote_node.short_name = "ALPH";
    remote_node.hw_model = "TBEAM";
    remote_node.role = "ROUTER";
    rt->db->upsert_node(remote_node);

    {
        std::lock_guard<std::mutex> lock(service.devices_mu_for_test());
        service.devices_for_test()[dev] = rt;
    }

    meshcli::WebService web(service);
    EXPECT_TRUE(web.start("127.0.0.1", 0, ""));
    int port = web.bound_port();

    // Broadcast a remote packet activity
    meshcli::PacketActivity act;
    act.device = dev;
    act.from_node = 0x22223333;
    act.to_node = 0xFFFFFFFF;
    act.port_name = "TEXT_MESSAGE_APP";
    act.summary = "Hello world";
    act.rx_snr = 9.5f;
    act.broadcast = true;
    act.ts = static_cast<uint64_t>(std::time(nullptr));
    web.record_and_broadcast_activity(act);

    // Call GET /api/stats?range=1h
    std::string get_req = "GET /api/stats?range=1h HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    std::string res = http_client_request(port, get_req);
    EXPECT_NE(res.find("200 OK"), std::string::npos);
    EXPECT_NE(res.find("\"total_packets\": 1"), std::string::npos);
    EXPECT_NE(res.find("\"msg_count\": 1"), std::string::npos);
    EXPECT_NE(res.find("\"TEXT_MESSAGE_APP\": 1"), std::string::npos);
    EXPECT_NE(res.find("Remote Node Alpha"), std::string::npos);

    // Call with node filter
    std::string get_req_node = "GET /api/stats?range=all&node=0x22223333 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    std::string res_node = http_client_request(port, get_req_node);
    EXPECT_NE(res_node.find("200 OK"), std::string::npos);
    EXPECT_NE(res_node.find("\"total_packets\": 1"), std::string::npos);

    // Call with non-matching node filter
    std::string get_req_nomatch = "GET /api/stats?range=all&node=0x99999999 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    std::string res_nomatch = http_client_request(port, get_req_nomatch);
    EXPECT_NE(res_nomatch.find("200 OK"), std::string::npos);
    EXPECT_NE(res_nomatch.find("\"total_packets\": 0"), std::string::npos);

    web.stop();
}

TEST(WebService, ConfigApiAndDeviceConsolidation) {
    meshcli::MeshService service;
    service.open_database(":memory:");

    std::string mac = "AA:BB:CC:DD:EE:FF";
    std::string bluez_alias = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF";

    auto rt = std::make_shared<meshcli::DeviceRuntime>();
    rt->id = mac;
    rt->spec.address = mac;
    rt->spec.name = "Heltec V3";
    rt->display_name = "Heltec V3 (AA:BB:CC:DD:EE:FF)";
    rt->aliases.push_back(bluez_alias);
    rt->aliases.push_back("Heltec V3");
    rt->my_node_num = 0x12345678;
    rt->db = std::make_unique<meshcli::NodeDb>();

    {
        std::lock_guard<std::mutex> lock(service.devices_mu_for_test());
        service.devices_for_test()[mac] = rt;
    }

    // Pre-populate some config in SQLite
    service.database().upsert_device_config(mac, "lora", "region", "EU_868");
    service.database().upsert_device_config(mac, "lora", "hop_limit", "3");
    service.database().upsert_device_config(mac, "device", "role", "ROUTER");

    meshcli::WebService web(service);
    EXPECT_TRUE(web.start("127.0.0.1", 0, ""));
    int port = web.bound_port();

    // 1. GET /api/devices: should only return 1 unified device
    std::string dev_req = "GET /api/devices HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    std::string dev_res = http_client_request(port, dev_req);
    EXPECT_NE(dev_res.find("200 OK"), std::string::npos);
    EXPECT_NE(dev_res.find("AA:BB:CC:DD:EE:FF"), std::string::npos);
    EXPECT_EQ(dev_res.find("/org/bluez"), std::string::npos); // Bluez internal path not leaked as device

    // 2. GET /api/config using bluez alias: should resolve and return structured sections
    std::string cfg_req = "GET /api/config?device=" + bluez_alias + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    std::string cfg_res = http_client_request(port, cfg_req);
    EXPECT_NE(cfg_res.find("200 OK"), std::string::npos);
    EXPECT_NE(cfg_res.find("\"sections\""), std::string::npos);
    EXPECT_NE(cfg_res.find("\"lora\""), std::string::npos);
    EXPECT_NE(cfg_res.find("\"EU_868\""), std::string::npos);
    EXPECT_NE(cfg_res.find("\"ROUTER\""), std::string::npos);

    // 3. POST /api/config: update single setting
    std::string post_body1 = "{\"device\":\"" + mac + "\",\"key\":\"lora.hop_limit\",\"value\":\"5\"}";
    std::string post_req1 = "POST /api/config HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " +
                           std::to_string(post_body1.size()) + "\r\n\r\n" + post_body1;
    std::string post_res1 = http_client_request(port, post_req1);
    EXPECT_NE(post_res1.find("200 OK"), std::string::npos);

    // Verify persisted
    auto lines_after = service.database().load_device_config(mac);
    bool found_hop5 = false;
    for (const auto& l : lines_after) {
        if (l.section == "lora" && l.key == "hop_limit" && l.value == "5") found_hop5 = true;
    }
    EXPECT_TRUE(found_hop5);

    // 4. POST /api/config: batch update via settings object
    std::string post_body2 = "{\"device\":\"" + bluez_alias + "\",\"settings\":{\"lora.tx_power\":\"22\",\"device.role\":\"CLIENT\"}}";
    std::string post_req2 = "POST /api/config HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " +
                           std::to_string(post_body2.size()) + "\r\n\r\n" + post_body2;
    std::string post_res2 = http_client_request(port, post_req2);
    EXPECT_NE(post_res2.find("200 OK"), std::string::npos);

    // Verify batch persisted
    auto lines_batch = service.database().load_device_config(mac);
    bool found_tx22 = false, found_client = false;
    for (const auto& l : lines_batch) {
        if (l.key == "tx_power" && l.value == "22") found_tx22 = true;
        if (l.key == "role" && l.value == "CLIENT") found_client = true;
    }
    EXPECT_TRUE(found_tx22);
    EXPECT_TRUE(found_client);

    web.stop();
}




