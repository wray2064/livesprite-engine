#pragma once
// ls_png.h — a dependency-free PNG writer for the testbed.
//
// Uses stored (uncompressed) deflate blocks inside a valid zlib stream. Sprite
// rasters are tiny, so the size cost is irrelevant and the encoder stays short
// and deterministic: the same raster always produces the same file bytes.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace lstestbed {

inline uint32_t crc32Of(const uint8_t* data, size_t length, uint32_t seed = 0xffffffffu) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        ready = true;
    }
    uint32_t crc = seed;
    for (size_t i = 0; i < length; ++i) {
        crc = table[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
    }
    return crc;
}

inline uint32_t adler32Of(const uint8_t* data, size_t length) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < length; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

inline void pushBigEndian(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
    out.push_back(static_cast<uint8_t>(value & 0xffu));
}

inline void pushChunk(std::vector<uint8_t>& out, const char tag[4],
                      const std::vector<uint8_t>& payload) {
    pushBigEndian(out, static_cast<uint32_t>(payload.size()));
    const size_t start = out.size();
    out.insert(out.end(), tag, tag + 4);
    out.insert(out.end(), payload.begin(), payload.end());
    const uint32_t crc = crc32Of(out.data() + start, out.size() - start) ^ 0xffffffffu;
    pushBigEndian(out, crc);
}

// rgba is width * height * 4 bytes, row-major. Returns the PNG file bytes.
inline std::vector<uint8_t> encodePng(uint32_t width, uint32_t height,
                                      const std::vector<uint8_t>& rgba) {
    if (width == 0 || height == 0 || rgba.size() < static_cast<size_t>(width) * height * 4) {
        return {};
    }

    // Raw image data: every scanline carries filter byte 0 (no filtering).
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(height) * (1 + static_cast<size_t>(width) * 4));
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        const uint8_t* row = rgba.data() + static_cast<size_t>(y) * width * 4;
        raw.insert(raw.end(), row, row + static_cast<size_t>(width) * 4);
    }

    // zlib stream: 0x78 0x01 header, stored deflate blocks, adler32 trailer.
    std::vector<uint8_t> zlib;
    zlib.push_back(0x78);
    zlib.push_back(0x01);
    const size_t kMaxBlock = 65535;
    for (size_t offset = 0; offset < raw.size(); offset += kMaxBlock) {
        const size_t chunk = std::min(kMaxBlock, raw.size() - offset);
        const bool last = offset + chunk >= raw.size();
        zlib.push_back(last ? 1 : 0);
        zlib.push_back(static_cast<uint8_t>(chunk & 0xffu));
        zlib.push_back(static_cast<uint8_t>((chunk >> 8) & 0xffu));
        const uint16_t inverted = static_cast<uint16_t>(~static_cast<uint16_t>(chunk));
        zlib.push_back(static_cast<uint8_t>(inverted & 0xffu));
        zlib.push_back(static_cast<uint8_t>((inverted >> 8) & 0xffu));
        zlib.insert(zlib.end(), raw.begin() + offset, raw.begin() + offset + chunk);
    }
    pushBigEndian(zlib, adler32Of(raw.data(), raw.size()));

    std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };

    std::vector<uint8_t> ihdr;
    pushBigEndian(ihdr, width);
    pushBigEndian(ihdr, height);
    ihdr.push_back(8);    // bit depth
    ihdr.push_back(6);    // colour type: RGBA
    ihdr.push_back(0);    // deflate
    ihdr.push_back(0);    // adaptive filtering
    ihdr.push_back(0);    // no interlace
    pushChunk(png, "IHDR", ihdr);
    pushChunk(png, "IDAT", zlib);
    pushChunk(png, "IEND", {});
    return png;
}

inline bool writePng(const std::string& path, uint32_t width, uint32_t height,
                     const std::vector<uint8_t>& rgba) {
    const std::vector<uint8_t> png = encodePng(width, height, rgba);
    if (png.empty()) {
        return false;
    }
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    const size_t written = std::fwrite(png.data(), 1, png.size(), file);
    std::fclose(file);
    return written == png.size();
}

// Base64 for embedding a PNG straight into the gallery page, so the page is one
// self-contained file that renders anywhere.
inline std::string base64(const std::vector<uint8_t>& bytes) {
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const uint32_t b0 = bytes[i];
        const uint32_t b1 = i + 1 < bytes.size() ? bytes[i + 1] : 0;
        const uint32_t b2 = i + 2 < bytes.size() ? bytes[i + 2] : 0;
        const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(kAlphabet[(triple >> 18) & 0x3f]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3f]);
        out.push_back(i + 1 < bytes.size() ? kAlphabet[(triple >> 6) & 0x3f] : char(61));
        out.push_back(i + 2 < bytes.size() ? kAlphabet[triple & 0x3f] : char(61));
    }
    return out;
}

} // namespace lstestbed
