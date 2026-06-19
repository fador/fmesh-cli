#include "stream/stream_client.h"
#include "mesh/mesh_codec.h"
#include "mesh/event.h"
#include "util/event_loop.h"

#include "minitest.h"

#include <cstring>
#include <thread>
#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#else
#include <winsock2.h>
#endif

using namespace meshcli;

// -- Stream framing / corrupt data tests --

TEST(StreamFraming, ValidFrameRoundtrip) {
    std::string payload = "hello world";
    StreamClient sc(0, "test", nullptr);
    auto framed = sc.frame(payload);
    EXPECT_EQ(framed.size(), 4 + payload.size());
    EXPECT_EQ(static_cast<unsigned char>(framed[0]), 0x94u);
    EXPECT_EQ(static_cast<unsigned char>(framed[1]), 0xC3u);
    uint16_t len = (static_cast<unsigned char>(framed[2]) << 8) |
                    static_cast<unsigned char>(framed[3]);
    EXPECT_EQ(len, payload.size());
    EXPECT_EQ(framed.substr(4), payload);
}

TEST(StreamFraming, EmptyPayloadFrame) {
    StreamClient sc(0, "test", nullptr);
    auto framed = sc.frame("");
    EXPECT_EQ(framed.size(), 4u);
    EXPECT_EQ(static_cast<unsigned char>(framed[0]), 0x94u);
    EXPECT_EQ(static_cast<unsigned char>(framed[1]), 0xC3u);
    EXPECT_EQ(static_cast<unsigned char>(framed[2]), 0u);
    EXPECT_EQ(static_cast<unsigned char>(framed[3]), 0u);
}

TEST(StreamFraming, LargePayload) {
    std::string payload(64000, 'x');
    StreamClient sc(0, "test", nullptr);
    auto framed = sc.frame(payload);
    EXPECT_EQ(framed.size(), 4 + payload.size());
    uint16_t len = (static_cast<unsigned char>(framed[2]) << 8) |
                    static_cast<unsigned char>(framed[3]);
    EXPECT_EQ(len, static_cast<uint16_t>(payload.size()));
}

TEST(StreamFraming, FrameTooLargeDropped) {
    std::string payload(65535, 'y');
    StreamClient sc(0, "test", nullptr);
    auto framed = sc.frame(payload);
    EXPECT_EQ(framed.size(), 0u);
}

// -- Codec robustness --

TEST(CodecRobust, DecodeEmptyBytes) {
    uint32_t cid = 0;
    auto ev = MeshCodec::decode_from_radio("", "dev", cid);
    EXPECT_FALSE(ev.has_value());
}

TEST(CodecRobust, DecodeGarbage) {
    uint32_t cid = 0;
    std::string garbage(100, '\xFF');
    auto ev = MeshCodec::decode_from_radio(garbage, "dev", cid);
    EXPECT_FALSE(ev.has_value());
}

TEST(CodecRobust, HexDumpEmpty) {
    auto s = MeshCodec::hex_dump("");
    EXPECT_EQ(s, "(empty)");
}

TEST(CodecRobust, HexDumpSingleByte) {
    auto s = MeshCodec::hex_dump("A");
    EXPECT_FALSE(s.empty());
}

TEST(CodecRobust, FromRadioSummaryGarbage) {
    auto s = MeshCodec::from_radio_summary("NOT A PROTOBUF");
    EXPECT_EQ(s, "parse failed");
}

TEST(CodecRobust, FromRadioSummaryEmpty) {
    auto s = MeshCodec::from_radio_summary("");
    // Empty serialized protobuf is a valid empty message (type=0).
    EXPECT_NE(s.find("type="), std::string::npos);
    EXPECT_EQ(s, "type=0");
}

#ifndef _WIN32
// -- EventFd robustness --

TEST(EventFdRobust, NotifyAndDrain) {
    EventFd ef;
    EXPECT_GT(ef.fd(), -1);
    ef.notify();
    ef.notify();
    ef.notify();
    ef.drain(); // should not hang
}

TEST(EventFdRobust, MoveSemantics) {
    EventFd ef1;
    EXPECT_GT(ef1.fd(), -1);
    int fd1 = ef1.fd();
    EventFd ef2(std::move(ef1));
    EXPECT_EQ(ef2.fd(), fd1);
    EXPECT_EQ(ef1.fd(), -1);
    // Drain moved-from should be a no-op (fd == -1).
    ef1.drain();
    ef2.drain();
}

TEST(EventFdRobust, DrainWithoutNotify) {
    EventFd ef;
    ef.drain(); // should not hang or crash
}

#endif // _WIN32

// -- ConcurrentQueue tests --

TEST(ConcurrentQueue, PushPopSingle) {
    ConcurrentQueue<int> q;
    q.push(42);
    int out = 0;
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, 42);
    EXPECT_FALSE(q.try_pop(out));
}

TEST(ConcurrentQueue, DrainAll) {
    ConcurrentQueue<int> q;
    for (int i = 0; i < 100; ++i) q.push(i);
    auto all = q.drain_all();
    EXPECT_EQ(all.size(), 100u);
    for (int i = 0; i < 100; ++i)
        EXPECT_EQ(all[i], i);
    EXPECT_TRUE(q.empty());
}

TEST(ConcurrentQueue, EmptyStart) {
    ConcurrentQueue<int> q;
    EXPECT_TRUE(q.empty());
    auto all = q.drain_all();
    EXPECT_TRUE(all.empty());
    int out = 0;
    EXPECT_FALSE(q.try_pop(out));
}

TEST(ConcurrentQueue, ConcurrentPushPop) {
    ConcurrentQueue<int> q;
    std::atomic<int> sum{0};
    constexpr int kNumPerThread = 5000;
    constexpr int kNumProducers = 4;

    std::vector<std::thread> producers;
    for (int t = 0; t < kNumProducers; ++t) {
        producers.emplace_back([&q, t, kNumPerThread] {
            for (int i = 0; i < kNumPerThread; ++i)
                q.push(t * 10000 + i);
        });
    }

    std::thread consumer([&q, &sum, kNumPerThread, kNumProducers] {
        int drained = 0;
        while (drained < kNumPerThread * kNumProducers) {
            auto batch = q.drain_all();
            for (int v : batch) { sum.fetch_add(v); ++drained; }
            if (batch.empty()) std::this_thread::yield();
        }
    });

    for (auto& t : producers) t.join();
    consumer.join();

    // Expected sum: each producer pushes 0..4999 with offset t*10000.
    int expected = 0;
    for (int t = 0; t < kNumProducers; ++t)
        for (int i = 0; i < kNumPerThread; ++i)
            expected += t * 10000 + i;
    EXPECT_EQ(sum.load(), expected);
    EXPECT_TRUE(q.empty());
}

// -- TcpConnect/SeriaOpen error paths --

TEST(TransportErrors, TcpConnectInvalidHost) {
    int fd = tcp_connect("doesnotexist.invalid", 12345);
    EXPECT_EQ(fd, -1);
}

TEST(TransportErrors, SerialOpenInvalidDevice) {
    int fd = serial_open("/dev/NO_SUCH_DEVICE_XYZ", 115200);
    EXPECT_EQ(fd, -1);
}
