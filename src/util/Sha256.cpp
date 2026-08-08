// ============================================================
//  SHA-256 实现（FIPS 180-4 精简版）+ 目录树哈希
// ============================================================

#include "util/Sha256.h"

#include "util/FileUtil.h"

#include <algorithm>
#include <filesystem>

namespace hao {

namespace {

uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

void sha256Compress(uint32_t h[8], const uint8_t block[64]) {
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
               (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

std::string toHex(const uint8_t* dig, size_t n) {
    static const char* hex = "0123456789abcdef";
    std::string o;
    o.resize(n * 2);
    for (size_t i = 0; i < n; ++i) {
        o[i * 2] = hex[dig[i] >> 4];
        o[i * 2 + 1] = hex[dig[i] & 0xf];
    }
    return o;
}

} // namespace

std::string sha256Hex(const uint8_t* data, size_t len) {
    uint32_t h[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    uint8_t block[64];
    size_t off = 0;
    while (off + 64 <= len) {
        sha256Compress(h, data + off);
        off += 64;
    }
    size_t rem = len - off;
    for (size_t i = 0; i < rem; ++i) block[i] = data[off + i];
    block[rem++] = 0x80;
    if (rem > 56) {
        while (rem < 64) block[rem++] = 0;
        sha256Compress(h, block);
        rem = 0;
    }
    while (rem < 56) block[rem++] = 0;
    uint64_t bits = uint64_t(len) * 8;
    for (int i = 0; i < 8; ++i)
        block[56 + i] = uint8_t((bits >> (56 - i * 8)) & 0xff);
    sha256Compress(h, block);

    uint8_t dig[32];
    for (int i = 0; i < 8; ++i) {
        dig[i * 4] = uint8_t(h[i] >> 24);
        dig[i * 4 + 1] = uint8_t(h[i] >> 16);
        dig[i * 4 + 2] = uint8_t(h[i] >> 8);
        dig[i * 4 + 3] = uint8_t(h[i]);
    }
    return toHex(dig, 32);
}

std::string sha256Hex(const std::string& data) {
    return sha256Hex(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

std::string sha256DirTree(const std::string& dir, std::string& errorOut) {
    errorOut.clear();
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        errorOut = "不是目录: " + dir;
        return "";
    }
    std::vector<std::string> rels;
    fs::path root = fs::path(dir);
    for (auto it = fs::recursive_directory_iterator(root, ec);
         it != fs::recursive_directory_iterator(); ++it) {
        if (ec) { errorOut = "遍历失败: " + dir; return ""; }
        if (!it->is_regular_file(ec)) continue;
        fs::path rel = fs::relative(it->path(), root, ec);
        if (ec) continue;
        rels.push_back(rel.generic_string());
    }
    std::sort(rels.begin(), rels.end());
    std::string acc;
    for (const auto& rel : rels) {
        std::string body;
        if (!readFile((fs::path(dir) / rel).generic_string(), body)) {
            errorOut = "无法读取 " + rel;
            return "";
        }
        acc += rel;
        acc.push_back('\0');
        acc += std::to_string(body.size());
        acc.push_back('\0');
        acc += body;
    }
    return sha256Hex(acc);
}

} // namespace hao
