// ============================================================
//  SemVer 实现
// ============================================================

#include "mod/SemVer.h"

#include <cctype>
#include <sstream>

namespace hao {

namespace {

std::string trim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool parseIntPart(const std::string& s, size_t& i, int& out) {
    if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    long v = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        v = v * 10 + (s[i] - '0');
        if (v > 2000000000L) return false;
        ++i;
    }
    out = static_cast<int>(v);
    return true;
}

} // namespace

bool parseSemVer(const std::string& s, SemVer& out) {
    std::string t = trim(s);
    if (t.empty()) return false;
    if (t[0] == 'v' || t[0] == 'V') t = t.substr(1);
    size_t i = 0;
    SemVer v;
    if (!parseIntPart(t, i, v.major)) return false;
    if (i >= t.size() || t[i] != '.') return false;
    ++i;
    if (!parseIntPart(t, i, v.minor)) return false;
    if (i >= t.size() || t[i] != '.') return false;
    ++i;
    if (!parseIntPart(t, i, v.patch)) return false;
    if (i != t.size()) return false; // 拒预发布后缀等
    out = v;
    return true;
}

std::string formatSemVer(const SemVer& v) {
    return std::to_string(v.major) + "." + std::to_string(v.minor) + "." +
           std::to_string(v.patch);
}

int compareSemVer(const SemVer& a, const SemVer& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

bool parseVersionConstraint(const std::string& s, VersionConstraint& out, std::string& errorOut) {
    errorOut.clear();
    std::string t = trim(s);
    if (t.empty()) {
        errorOut = "版本约束为空";
        return false;
    }
    if (t.find('*') != std::string::npos) {
        errorOut = "不支持通配符 *";
        return false;
    }
    if (t.find(' ') != std::string::npos || t.find(',') != std::string::npos) {
        errorOut = "不支持复合版本区间";
        return false;
    }

    VersionConstraint c;
    c.original = t;
    std::string body = t;

    if (body[0] == '^') {
        c.kind = ConstraintKind::Caret;
        body = body.substr(1);
    } else if (body[0] == '~') {
        c.kind = ConstraintKind::Tilde;
        body = body.substr(1);
    } else if (body.rfind(">=", 0) == 0) {
        c.kind = ConstraintKind::GreaterEq;
        body = body.substr(2);
    } else if (body[0] == '=') {
        c.kind = ConstraintKind::Exact;
        body = body.substr(1);
    } else if (body[0] == '<' || body[0] == '>') {
        errorOut = "不支持该比较运算符（仅允许 ^、~、>=、= 与精确版本）";
        return false;
    } else {
        c.kind = ConstraintKind::Exact;
    }

    body = trim(body);
    if (!parseSemVer(body, c.base)) {
        errorOut = "无法解析语义版本: " + t;
        return false;
    }
    out = c;
    return true;
}

bool satisfiesConstraint(const SemVer& ver, const VersionConstraint& c) {
    switch (c.kind) {
    case ConstraintKind::Exact:
        return compareSemVer(ver, c.base) == 0;
    case ConstraintKind::GreaterEq:
        return compareSemVer(ver, c.base) >= 0;
    case ConstraintKind::Tilde: {
        // >=base < major.(minor+1).0
        if (compareSemVer(ver, c.base) < 0) return false;
        SemVer upper{c.base.major, c.base.minor + 1, 0};
        return compareSemVer(ver, upper) < 0;
    }
    case ConstraintKind::Caret: {
        if (compareSemVer(ver, c.base) < 0) return false;
        SemVer upper;
        if (c.base.major > 0) {
            upper = {c.base.major + 1, 0, 0};
        } else if (c.base.minor > 0) {
            upper = {0, c.base.minor + 1, 0};
        } else {
            upper = {0, 0, c.base.patch + 1};
        }
        return compareSemVer(ver, upper) < 0;
    }
    }
    return false;
}

bool selectHighestMatching(const std::vector<SemVer>& candidates, const VersionConstraint& c,
                           SemVer& chosen) {
    bool found = false;
    SemVer best;
    for (const auto& v : candidates) {
        if (!satisfiesConstraint(v, c)) continue;
        if (!found || compareSemVer(v, best) > 0) {
            best = v;
            found = true;
        }
    }
    if (!found) return false;
    chosen = best;
    return true;
}

} // namespace hao
