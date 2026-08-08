// ============================================================
//  HaoLang —— 字符串工具实现
// ============================================================

#include "util/StringUtil.h"
#include <cstdint>

namespace hao {
namespace StringUtil {

std::string stripUnderscores(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) if (c != '_') r += c;
    return r;
}

static void appendUtf8(std::string& r, uint32_t cp) {
    if (cp <= 0x7F) {
        r += static_cast<char>(cp);
    } else if (cp <= 0x7FF) {
        r += static_cast<char>(0xC0 | (cp >> 6));
        r += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        r += static_cast<char>(0xE0 | (cp >> 12));
        r += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        r += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        if (cp > 0x10FFFF) cp = 0xFFFD;
        r += static_cast<char>(0xF0 | (cp >> 18));
        r += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        r += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        r += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

static int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string unescapeStringLiteral(const std::string& raw) {
    if (raw.size() < 2) return raw;
    std::string s = raw.substr(1, raw.size() - 2);
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char e = s[++i];
            switch (e) {
                case 'n':  r += '\n'; break;
                case 't':  r += '\t'; break;
                case 'r':  r += '\r'; break;
                case '\\': r += '\\'; break;
                case '"':  r += '"';  break;
                case 0x27: r += (char)0x27; break;
                case '$':  r += '$';  break;
                case 'u': {
                    uint32_t cp = 0;
                    int ok = 1;
                    for (int k = 0; k < 4; ++k) {
                        if (i + 1 >= s.size()) { ok = 0; break; }
                        int hv = hexVal(s[++i]);
                        if (hv < 0) { ok = 0; break; }
                        cp = (cp << 4) | (uint32_t)hv;
                    }
                    if (ok) appendUtf8(r, cp);
                    else r += 'u';
                    break;
                }
                default:   r += e; break;
            }
        } else r += s[i];
    }
    return r;
}

std::string unescapeVerbatimString(const std::string& raw) {
    // @"..." 或词法已去前缀后的内容：去掉外层引号，"" → "
    if (raw.size() < 2) return raw;
    size_t start = 0;
    // 允许传入完整 @"..." 或仅 "..."
    if (raw.size() >= 3 && raw[0] == '@' && raw[1] == '"') start = 2;
    else if (raw[0] == '"') start = 1;
    else return raw;
    size_t end = raw.size();
    if (end > start && raw[end - 1] == '"') end--;
    std::string s = raw.substr(start, end - start);
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '"' && i + 1 < s.size() && s[i + 1] == '"') {
            r += '"';
            ++i;
        } else {
            r += s[i];
        }
    }
    return r;
}

uint32_t decodeCharLiteral(const std::string& raw) {
    std::string content = unescapeStringLiteral(raw);
    if (content.empty()) return 0;
    // UTF-8 解码第一个码点
    const auto* p = reinterpret_cast<const unsigned char*>(content.data());
    size_t n = content.size();
    unsigned char c = p[0];
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0 && n >= 2)
        return ((c & 0x1F) << 6) | (p[1] & 0x3F);
    if ((c & 0xF0) == 0xE0 && n >= 3)
        return ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    if ((c & 0xF8) == 0xF0 && n >= 4)
        return ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) |
               ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    return c;
}

} // namespace StringUtil
} // namespace hao
