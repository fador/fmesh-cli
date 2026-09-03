#include "util/compression.h"
#include "minitest.h"
#include <random>

using namespace meshcli;

TEST(CompressionTest, EmptyString) {
    auto comp = Compression::compress("");
    EXPECT_TRUE(comp.has_value());
    EXPECT_TRUE(comp->empty());

    auto decomp = Compression::decompress("");
    EXPECT_TRUE(decomp.has_value());
    EXPECT_TRUE(decomp->empty());
}

TEST(CompressionTest, RoundTrip) {
    std::string sample = R"({"type":"data","messages":[{"channel_idx":0,"device":"stream:127.0.0.1:4403","direction":"in","from_node":1234,"packet_id":999,"text":"Hello world over zstd compressed mesh stream!","to_node":4294967295,"ts":1700000000,"window_kind":"channel","window_target":0}]})";

    auto comp = Compression::compress(sample);
    EXPECT_TRUE(comp.has_value());
    EXPECT_TRUE(comp->size() < sample.size());

    auto decomp = Compression::decompress(*comp);
    EXPECT_TRUE(decomp.has_value());
    EXPECT_EQ(*decomp, sample);
}

TEST(CompressionTest, SignificantSavingsOnJson) {
    // Generate a repetitive realistic sync batch
    std::string batch = R"({"type":"data","messages":[)";
    for (int i = 0; i < 20; ++i) {
        if (i > 0) batch += ",";
        batch += R"({"channel_idx":0,"device":"stream:192.168.1.100:4403","direction":"in","from_node":)";
        batch += std::to_string(1000 + i);
        batch += R"(,"packet_id":)";
        batch += std::to_string(5000 + i);
        batch += R"(,"text":"Message test content number )";
        batch += std::to_string(i);
        batch += R"(","to_node":4294967295,"ts":1700000000,"window_kind":"channel","window_target":0})";
    }
    batch += R"(]})";

    auto comp = Compression::compress(batch);
    EXPECT_TRUE(comp.has_value());

    // Expect at least 60% compression on repetitive JSON records
    double ratio = static_cast<double>(comp->size()) / static_cast<double>(batch.size());
    EXPECT_TRUE(ratio < 0.40); // > 60% reduction

    auto decomp = Compression::decompress(*comp);
    EXPECT_TRUE(decomp.has_value());
    EXPECT_EQ(*decomp, batch);
}

TEST(CompressionTest, CorruptedData) {
    std::string corrupted = "\x28\xb5\x2f\xfd\x00\x00\xff\xff\xee\xdd";
    auto decomp = Compression::decompress(corrupted);
    EXPECT_FALSE(decomp.has_value());
}

TEST(CompressionTest, BombGuard) {
    // Large repetitive payload
    std::string large(100000, 'A');
    auto comp = Compression::compress(large);
    EXPECT_TRUE(comp.has_value());

    // Decompress with limit smaller than decompressed size
    auto decomp = Compression::decompress(*comp, 50000);
    EXPECT_FALSE(decomp.has_value());

    // Decompress with adequate limit
    auto decomp_ok = Compression::decompress(*comp, 150000);
    EXPECT_TRUE(decomp_ok.has_value());
    EXPECT_EQ(*decomp_ok, large);
}

TEST(CompressionTest, AdaptiveCompression) {
    unsigned char marker = 0;

    // 1. Small string below min_size (64 bytes)
    std::string small_str = "hello small";
    std::string res1 = Compression::compress_adaptive(small_str, marker, 0xD0, 0xD1, 64);
    EXPECT_EQ(marker, 0xD0);
    EXPECT_EQ(res1, small_str);

    // 2. Large JSON string above min_size
    std::string json_str = R"({"type":"inventory","max_message_ts":1700000000,"max_location_ts":1700000000,"extra_padding_for_compression_check":"abcdefabcdefabcdef"})";
    std::string res2 = Compression::compress_adaptive(json_str, marker, 0xD0, 0xD1, 64);
    EXPECT_EQ(marker, 0xD1);
    EXPECT_TRUE(res2.size() < json_str.size());

    // Decompressing res2 should yield json_str
    auto decomp = Compression::decompress(res2);
    EXPECT_TRUE(decomp.has_value());
    EXPECT_EQ(*decomp, json_str);

    // 3. Random incompressible bytes
    std::mt19937 rng(42);
    std::string random_bytes(100, '\0');
    for (char& b : random_bytes) b = static_cast<char>(rng() & 0xFF);

    std::string res3 = Compression::compress_adaptive(random_bytes, marker, 0xD0, 0xD1, 64);
    // If compression expands or doesn't beat random bytes, fallback to 0xD0
    if (res3.size() >= random_bytes.size()) {
        EXPECT_EQ(marker, 0xD0);
        EXPECT_EQ(res3, random_bytes);
    }
}
