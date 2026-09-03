#include "compression.h"
#include "util/log.h"
#include <zstd.h>
#include <vector>

namespace meshcli {

std::optional<std::string> Compression::compress(std::string_view data, int level) {
    if (data.empty()) {
        return std::string();
    }

    size_t bound = ZSTD_compressBound(data.size());
    std::string out;
    out.resize(bound);

    size_t csize = ZSTD_compress(out.data(), out.size(), data.data(), data.size(), level);
    if (ZSTD_isError(csize)) {
        LOG_WARN() << "Compression: ZSTD_compress failed: " << ZSTD_getErrorName(csize);
        return std::nullopt;
    }

    out.resize(csize);
    return out;
}

std::optional<std::string> Compression::decompress(std::string_view compressed, size_t max_uncompressed_bytes) {
    if (compressed.empty()) {
        return std::string();
    }

    unsigned long long content_size = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
    if (content_size == ZSTD_CONTENTSIZE_ERROR) {
        LOG_WARN() << "Compression: ZSTD_getFrameContentSize error (invalid zstd frame)";
        return std::nullopt;
    }

    if (content_size != ZSTD_CONTENTSIZE_UNKNOWN) {
        if (content_size > max_uncompressed_bytes) {
            LOG_WARN() << "Compression: decompressed content size exceeds limit: " << content_size;
            return std::nullopt;
        }

        std::string out;
        out.resize(static_cast<size_t>(content_size));
        size_t dsize = ZSTD_decompress(out.data(), out.size(), compressed.data(), compressed.size());
        if (ZSTD_isError(dsize)) {
            LOG_WARN() << "Compression: ZSTD_decompress failed: " << ZSTD_getErrorName(dsize);
            return std::nullopt;
        }
        out.resize(dsize);
        return out;
    }

    // If content size is unknown in frame header, decompress via streaming DCtx
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    if (!dctx) {
        LOG_WARN() << "Compression: failed to create ZSTD_DCtx";
        return std::nullopt;
    }

    std::string out;
    ZSTD_inBuffer input = { compressed.data(), compressed.size(), 0 };
    char chunk[8192];

    while (input.pos < input.size) {
        ZSTD_outBuffer output = { chunk, sizeof(chunk), 0 };
        size_t ret = ZSTD_decompressStream(dctx, &output, &input);
        if (ZSTD_isError(ret)) {
            LOG_WARN() << "Compression: ZSTD_decompressStream failed: " << ZSTD_getErrorName(ret);
            ZSTD_freeDCtx(dctx);
            return std::nullopt;
        }
        if (out.size() + output.pos > max_uncompressed_bytes) {
            LOG_WARN() << "Compression: streaming decompression exceeded limit: " << max_uncompressed_bytes;
            ZSTD_freeDCtx(dctx);
            return std::nullopt;
        }
        out.append(chunk, output.pos);
        if (ret == 0) break; // frame completed
    }

    ZSTD_freeDCtx(dctx);
    return out;
}

std::string Compression::compress_adaptive(
    std::string_view data,
    unsigned char& out_marker,
    unsigned char raw_marker,
    unsigned char comp_marker,
    size_t min_size,
    int level) {

    out_marker = raw_marker;
    if (data.size() < min_size) {
        return std::string(data);
    }

    auto comp = compress(data, level);
    if (comp && comp->size() < data.size()) {
        out_marker = comp_marker;
        return std::move(*comp);
    }

    return std::string(data);
}

} // namespace meshcli
