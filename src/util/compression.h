#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace meshcli {

class Compression {
public:
    static constexpr size_t kMinCompressSize = 64;
    static constexpr size_t kMaxDecompressedSize = 10 * 1024 * 1024; // 10 MB limit
    static constexpr int kDefaultLevel = 3;

    // Compresses data using zstd. Returns nullopt on failure.
    static std::optional<std::string> compress(std::string_view data, int level = kDefaultLevel);

    // Decompresses zstd-compressed data. Returns nullopt on error or if decompressed size exceeds max_uncompressed_bytes.
    static std::optional<std::string> decompress(std::string_view compressed, size_t max_uncompressed_bytes = kMaxDecompressedSize);

    // Adaptively chooses whether to compress.
    // If data.size() < min_size or compression does not reduce size,
    // returns data as-is and sets out_marker = raw_marker.
    // Otherwise returns compressed data and sets out_marker = comp_marker.
    static std::string compress_adaptive(
        std::string_view data,
        unsigned char& out_marker,
        unsigned char raw_marker = 0xD0,
        unsigned char comp_marker = 0xD1,
        size_t min_size = kMinCompressSize,
        int level = kDefaultLevel);
};

} // namespace meshcli
