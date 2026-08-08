// ============================================================
//  SemVer 解析与约束（包管理第 2 层 A1）
// ============================================================
#pragma once

#include <string>
#include <vector>

namespace hao {

struct SemVer {
    int major = 0;
    int minor = 0;
    int patch = 0;
};

enum class ConstraintKind {
    Exact,
    Caret,      // ^
    Tilde,      // ~
    GreaterEq,  // >=
};

struct VersionConstraint {
    ConstraintKind kind = ConstraintKind::Exact;
    SemVer base;
    std::string original;
};

// 解析 major.minor.patch（可带前缀 v）。成功返回 true。
bool parseSemVer(const std::string& s, SemVer& out);

// 格式化为 "major.minor.patch"
std::string formatSemVer(const SemVer& v);

// a<b → 负；a==b → 0；a>b → 正
int compareSemVer(const SemVer& a, const SemVer& b);

// 解析约束：精确 / ^ / ~ / >= / =。拒 *、<、复合区间等。
// 失败时 errorOut 为中文原因。
bool parseVersionConstraint(const std::string& s, VersionConstraint& out, std::string& errorOut);

bool satisfiesConstraint(const SemVer& ver, const VersionConstraint& c);

// 在已解析的候选版本中选最高且满足约束的；无则返回 false。
bool selectHighestMatching(const std::vector<SemVer>& candidates, const VersionConstraint& c,
                           SemVer& chosen);

} // namespace hao
