// ============================================================
//  包管理第 1 层实现
// ============================================================

#include "mod/ModResolve.h"

#include "mod/HttpFetch.h"
#include "mod/SemVer.h"
#include "util/FileUtil.h"
#include "util/PathUtil.h"
#include "util/Sha256.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <queue>
#include <set>
#include <sstream>

namespace hao {

namespace {

namespace fs = std::filesystem;

std::string normalizePath(const std::string& p) {
    std::error_code ec;
    fs::path path = fs::weakly_canonical(p, ec);
    if (ec) path = fs::absolute(p, ec);
    std::string s = path.generic_string();
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    return s;
}

bool looksLikeLocalPath(const std::string& s) {
    if (s.empty()) return false;
    if (s.rfind("./", 0) == 0 || s.rfind("../", 0) == 0) return true;
    if (s.rfind(".\\", 0) == 0 || s.rfind("..\\", 0) == 0) return true;
    if (s.size() >= 2 && std::isalpha(static_cast<unsigned char>(s[0])) && s[1] == ':')
        return true;
    if (s[0] == '/' || s[0] == '\\') return true;
    return false;
}

std::string stripFileUrl(const std::string& reg) {
    // file:///D:/x 或 file://D:/x 或 file:/D:/x
    if (reg.rfind("file:", 0) != 0) return reg;
    std::string rest = reg.substr(5);
    while (!rest.empty() && rest[0] == '/') {
        // file:///C:/... → 保留 /C: 再去掉一个导致 C:
        if (rest.size() >= 3 && rest[0] == '/' && std::isalpha(static_cast<unsigned char>(rest[1])) &&
            rest[2] == ':') {
            rest = rest.substr(1);
            break;
        }
        rest = rest.substr(1);
        if (rest.size() >= 2 && std::isalpha(static_cast<unsigned char>(rest[0])) &&
            rest[1] == ':')
            break;
        if (rest.empty() || rest[0] != '/') break;
    }
    return rest;
}

bool isHttpRegistry(const std::string& reg) {
    return reg.rfind("http://", 0) == 0 || reg.rfind("https://", 0) == 0;
}

// 本地仓包路径：<HAO_REPO>/<module>/<version>/（与远程布局一致）
std::string repoPkgPath(const std::string& module, const std::string& version) {
    return joinPath(joinPath(localRepoDir(), module), version);
}

bool sameNormalizedPath(const std::string& a, const std::string& b) {
    return normalizePath(a) == normalizePath(b);
}

std::string findInFileRegistry(const std::string& regRoot, const std::string& module,
                               const std::string& version) {
    std::string root = stripFileUrl(regRoot);
    std::string cand = joinPath(joinPath(root, module), version);
    if (isDirectory(cand)) return normalizePath(cand);
    return "";
}

// 列出本地仓中某模块的全部语义版本目录名。
std::vector<SemVer> listVersionsInFileRegistry(const std::string& regRoot,
                                               const std::string& module) {
    std::vector<SemVer> vers;
    std::string root = stripFileUrl(regRoot);
    std::string modDir = joinPath(root, module);
    std::error_code ec;
    if (!fs::is_directory(modDir, ec)) return vers;
    for (auto& e : fs::directory_iterator(modDir, ec)) {
        if (ec) break;
        if (!e.is_directory(ec)) continue;
        SemVer v;
        if (parseSemVer(e.path().filename().string(), v))
            vers.push_back(v);
    }
    std::sort(vers.begin(), vers.end(),
              [](const SemVer& a, const SemVer& b) { return compareSemVer(a, b) < 0; });
    return vers;
}

// 在 registries 中按约束选最高版本，返回源目录与精确版本字符串。
bool resolveFromFileRegistries(const std::vector<std::string>& regs, const std::string& module,
                               const VersionConstraint& constraint, std::string& srcDirOut,
                               std::string& versionOut, std::string& registryOut,
                               std::string& errorOut) {
    bool anyLocal = false;
    SemVer best;
    bool found = false;
    std::string bestSrc;
    std::string bestReg;
    for (const auto& reg : regs) {
        if (isHttpRegistry(reg)) continue;
        anyLocal = true;
        auto cands = listVersionsInFileRegistry(reg, module);
        SemVer chosen;
        if (!selectHighestMatching(cands, constraint, chosen)) continue;
        if (!found || compareSemVer(chosen, best) > 0) {
            std::string src = findInFileRegistry(reg, module, formatSemVer(chosen));
            if (src.empty()) continue;
            best = chosen;
            bestSrc = src;
            bestReg = reg;
            found = true;
        }
    }
    if (!found) {
        if (!anyLocal) {
            errorOut = "依赖 " + module + "@" + constraint.original +
                       " 需要仓库，但当前仅支持本地 file 仓库。\n"
                       "      请设置 HAO_REGISTRY=本地目录 或 project.registry.additional";
        } else {
            errorOut = "本地仓库中无满足约束的版本: " + module + "@" + constraint.original;
        }
        return false;
    }
    srcDirOut = bestSrc;
    versionOut = formatSemVer(best);
    registryOut = bestReg;
    return true;
}

std::string joinUrl(const std::string& base, const std::string& rel) {
    std::string b = base;
    while (!b.empty() && (b.back() == '/' || b.back() == '\\')) b.pop_back();
    std::string r = rel;
    while (!r.empty() && r[0] == '/') r.erase(0, 1);
    return b + "/" + r;
}

// 从 HTTP 仓拉取 module@version.zip 到临时目录（供随后 copyTree 进本地仓）。
bool fetchHttpPackage(const std::string& reg, const std::string& module, const std::string& version,
                      std::string& destOut, std::string& errorOut) {
    std::string token = envHaoToken();
    std::string proxy = envHaoProxy();
    std::string zipUrl = joinUrl(reg, module + "/" + version + ".zip");
    std::string tmpRoot = joinPath(localRepoDir(), "_fetch");
    std::string key = std::to_string(std::hash<std::string>{}(module + "@" + version));
    std::string zipPath = joinPath(tmpRoot, key + ".zip");
    std::string stage = joinPath(tmpRoot, key + "_src");
    std::error_code ec;
    fs::create_directories(tmpRoot, ec);
    if (fs::exists(stage, ec)) fs::remove_all(stage, ec);
    if (!httpDownloadToFile(zipUrl, zipPath, token, proxy, errorOut)) return false;
    fs::create_directories(stage, ec);
    if (!extractZipArchive(zipPath, stage, errorOut)) return false;
    fs::remove(zipPath, ec);
    destOut = normalizePath(stage);
    return true;
}

std::vector<SemVer> listVersionsHttpRegistry(const std::string& reg, const std::string& module) {
    std::vector<SemVer> vers;
    std::string token = envHaoToken();
    std::string proxy = envHaoProxy();
    std::string url = joinUrl(reg, module + "/versions.json");
    std::string body, err;
    if (!httpDownloadToString(url, body, token, proxy, err)) return vers;
    std::vector<std::string> names;
    if (!parseVersionsJson(body, names, err)) return vers;
    for (const auto& n : names) {
        SemVer v;
        if (parseSemVer(n, v)) vers.push_back(v);
    }
    std::sort(vers.begin(), vers.end(),
              [](const SemVer& a, const SemVer& b) { return compareSemVer(a, b) < 0; });
    return vers;
}

// 选同时满足多条约束的最高版本（本地 file 优先，其次 HTTP zip）。
bool resolveFromFileRegistriesAll(const std::vector<std::string>& regs, const std::string& module,
                                  const std::vector<VersionConstraint>& constraints,
                                  std::string& srcDirOut, std::string& versionOut,
                                  std::string& registryOut, std::string& errorOut) {
    if (constraints.empty()) {
        errorOut = "内部错误: 空约束集 " + module;
        return false;
    }
    bool anyLocal = false;
    bool anyHttp = false;
    SemVer best;
    bool found = false;
    std::string bestSrc, bestReg;
    bool bestIsHttp = false;

    for (const auto& reg : regs) {
        if (isHttpRegistry(reg)) continue;
        anyLocal = true;
        auto cands = listVersionsInFileRegistry(reg, module);
        for (auto it = cands.rbegin(); it != cands.rend(); ++it) {
            bool ok = true;
            for (const auto& c : constraints) {
                if (!satisfiesConstraint(*it, c)) { ok = false; break; }
            }
            if (!ok) continue;
            if (!found || compareSemVer(*it, best) > 0) {
                std::string src = findInFileRegistry(reg, module, formatSemVer(*it));
                if (src.empty()) continue;
                best = *it;
                bestSrc = src;
                bestReg = reg;
                bestIsHttp = false;
                found = true;
            }
            break;
        }
    }

    // 本地未命中时再试 HTTP（versions.json + {ver}.zip）
    if (!found) {
        for (const auto& reg : regs) {
            if (!isHttpRegistry(reg)) continue;
            anyHttp = true;
            auto cands = listVersionsHttpRegistry(reg, module);
            // 无 versions.json 时：若全部约束为同一精确版，直接试该版
            if (cands.empty()) {
                bool allExact = true;
                SemVer exact{};
                bool have = false;
                for (const auto& c : constraints) {
                    if (c.kind != ConstraintKind::Exact) { allExact = false; break; }
                    if (!have) { exact = c.base; have = true; }
                    else if (compareSemVer(exact, c.base) != 0) { allExact = false; break; }
                }
                if (allExact && have) cands.push_back(exact);
            }
            for (auto it = cands.rbegin(); it != cands.rend(); ++it) {
                bool ok = true;
                for (const auto& c : constraints) {
                    if (!satisfiesConstraint(*it, c)) { ok = false; break; }
                }
                if (!ok) continue;
                if (!found || compareSemVer(*it, best) > 0) {
                    best = *it;
                    bestReg = reg;
                    bestIsHttp = true;
                    found = true;
                }
                break;
            }
        }
    }

    if (!found) {
        std::ostringstream os;
        os << "依赖冲突或无满足版本: " << module;
        for (const auto& c : constraints)
            os << "\n      需要 " << c.original;
        if (!anyLocal && !anyHttp)
            os << "\n      （无可用仓库；请设置 HAO_REGISTRY）";
        else if (!anyLocal && anyHttp)
            os << "\n      （HTTP 仓无 versions.json 或对应 .zip）";
        errorOut = os.str();
        return false;
    }

    if (bestIsHttp) {
        std::string dest;
        if (!fetchHttpPackage(bestReg, module, formatSemVer(best), dest, errorOut))
            return false;
        srcDirOut = dest;
    } else {
        srcDirOut = bestSrc;
    }
    versionOut = formatSemVer(best);
    registryOut = bestReg;
    return true;
}

// 从包目录读 haopkg.json 的 dependencies（无文件或无字段 → 空）。
bool loadPkgDependencies(const std::string& pkgDir, std::vector<DepSpec>& out, std::string& err) {
    out.clear();
    std::string path = joinPath(pkgDir, "haopkg.json");
    if (!fileExists(path)) return true;
    std::string src;
    if (!readFile(path, src)) { err = "无法读取 " + path; return false; }
    // 找 "dependencies" : { ... }
    size_t pos = src.find("\"dependencies\"");
    if (pos == std::string::npos) return true;
    pos = src.find('{', pos);
    if (pos == std::string::npos) { err = path + ": dependencies 须为对象"; return false; }
    size_t i = pos + 1;
    auto skipWs = [&] {
        while (i < src.size() && (src[i] == ' ' || src[i] == '\t' || src[i] == '\r' ||
                                  src[i] == '\n'))
            ++i;
    };
    auto parseStr = [&](std::string& s) -> bool {
        skipWs();
        if (i >= src.size() || src[i] != '"') return false;
        ++i;
        s.clear();
        while (i < src.size() && src[i] != '"') {
            if (src[i] == '\\' && i + 1 < src.size()) { s.push_back(src[i + 1]); i += 2; }
            else s.push_back(src[i++]);
        }
        if (i >= src.size()) return false;
        ++i;
        return true;
    };
    skipWs();
    if (i < src.size() && src[i] == '}') return true;
    for (;;) {
        std::string key, val;
        if (!parseStr(key)) { err = path + ": dependencies 键须为字符串"; return false; }
        skipWs();
        if (i >= src.size() || src[i] != ':') { err = path + ": dependencies 期望 ':'"; return false; }
        ++i;
        if (!parseStr(val)) { err = path + ": dependencies 值须为字符串"; return false; }
        DepSpec d;
        d.module = key;
        d.version = val;
        out.push_back(d);
        skipWs();
        if (i < src.size() && src[i] == '}') break;
        if (i >= src.size() || src[i] != ',') { err = path + ": dependencies 语法错"; return false; }
        ++i;
    }
    return true;
}

void addRequiredBy(LockEntry& e, const std::string& from) {
    for (const auto& x : e.requiredBy)
        if (x == from) return;
    e.requiredBy.push_back(from);
}

bool copyTree(const std::string& from, const std::string& to, std::string& err) {
    std::error_code ec;
    fs::create_directories(to, ec);
    if (ec) { err = "无法创建本地仓目录: " + to; return false; }
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) { err = "复制到本地仓失败: " + from + " → " + to + " (" + ec.message() + ")"; return false; }
    return true;
}

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        default:   o.push_back(c); break;
        }
    }
    return o;
}

bool writeLockFile(const std::string& projectDir, const std::vector<LockEntry>& entries,
                   std::string& err) {
    std::string path = joinPath(projectDir, kHaoLockFile);
    std::ostringstream ss;
    ss << "{\n";
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        ss << "  \"" << jsonEscape(e.module) << "\": {\n"
           << "    \"version\": \"" << jsonEscape(e.version) << "\",\n"
           << "    \"registry\": \"" << jsonEscape(e.registry) << "\",\n"
           << "    \"sha256\": \"" << jsonEscape(e.sha256) << "\"";
        if (e.replaced)
            ss << ",\n    \"replaced\": true";
        if (!e.requiredBy.empty()) {
            ss << ",\n    \"requiredBy\": [";
            for (size_t j = 0; j < e.requiredBy.size(); ++j) {
                if (j) ss << ", ";
                ss << "\"" << jsonEscape(e.requiredBy[j]) << "\"";
            }
            ss << "]";
        }
        ss << "\n  }";
        if (i + 1 < entries.size()) ss << ",";
        ss << "\n";
    }
    ss << "}\n";
    std::ofstream f(path, std::ios::binary);
    if (!f) { err = "无法写入 " + path; return false; }
    f << ss.str();
    return true;
}

// 极简 lock 解析：只取顶层对象的 version/registry/sha256/replaced
bool parseLockJson(const std::string& src, std::vector<LockEntry>& out, std::string& err) {
    // 复用思路：逐模块扫描 "key": { ... }
    size_t i = 0;
    auto skipWs = [&] {
        while (i < src.size() && (src[i] == ' ' || src[i] == '\t' || src[i] == '\r' ||
                                  src[i] == '\n'))
            ++i;
    };
    auto parseStr = [&](std::string& s) -> bool {
        skipWs();
        if (i >= src.size() || src[i] != '"') return false;
        ++i;
        s.clear();
        while (i < src.size() && src[i] != '"') {
            if (src[i] == '\\' && i + 1 < src.size()) { s.push_back(src[i + 1]); i += 2; }
            else s.push_back(src[i++]);
        }
        if (i >= src.size()) return false;
        ++i;
        return true;
    };
    skipWs();
    if (i >= src.size() || src[i] != '{') { err = "lock 须为 JSON 对象"; return false; }
    ++i;
    skipWs();
    if (i < src.size() && src[i] == '}') return true;
    for (;;) {
        std::string mod;
        if (!parseStr(mod)) { err = "lock: 期望模块名"; return false; }
        skipWs();
        if (i >= src.size() || src[i] != ':') { err = "lock: 期望 ':'"; return false; }
        ++i;
        skipWs();
        if (i >= src.size() || src[i] != '{') { err = "lock: 期望条目对象"; return false; }
        ++i;
        LockEntry e;
        e.module = mod;
        skipWs();
        if (i < src.size() && src[i] != '}') {
            for (;;) {
                std::string key;
                if (!parseStr(key)) { err = "lock: 期望字段名"; return false; }
                skipWs();
                if (i >= src.size() || src[i] != ':') { err = "lock: 期望 ':'"; return false; }
                ++i;
                skipWs();
                if (key == "replaced") {
                    if (src.compare(i, 4, "true") == 0) { e.replaced = true; i += 4; }
                    else if (src.compare(i, 5, "false") == 0) { e.replaced = false; i += 5; }
                    else { err = "lock: replaced 须为布尔"; return false; }
                } else if (key == "requiredBy") {
                    skipWs();
                    if (i >= src.size() || src[i] != '[') { err = "lock: requiredBy 须为数组"; return false; }
                    ++i;
                    skipWs();
                    if (i < src.size() && src[i] != ']') {
                        for (;;) {
                            std::string item;
                            if (!parseStr(item)) { err = "lock: requiredBy 项须为字符串"; return false; }
                            e.requiredBy.push_back(item);
                            skipWs();
                            if (i < src.size() && src[i] == ']') { ++i; break; }
                            if (i >= src.size() || src[i] != ',') { err = "lock: requiredBy 语法错"; return false; }
                            ++i;
                        }
                    } else if (i < src.size() && src[i] == ']') {
                        ++i;
                    }
                } else {
                    std::string val;
                    if (!parseStr(val)) { err = "lock: 期望字符串值"; return false; }
                    if (key == "version") e.version = val;
                    else if (key == "registry") e.registry = val;
                    else if (key == "sha256") e.sha256 = val;
                }
                skipWs();
                if (i < src.size() && src[i] == '}') { ++i; break; }
                if (i >= src.size() || src[i] != ',') { err = "lock: 条目内语法错"; return false; }
                ++i;
            }
        } else if (i < src.size() && src[i] == '}') {
            ++i;
        }
        out.push_back(e);
        skipWs();
        if (i < src.size() && src[i] == '}') break;
        if (i >= src.size() || src[i] != ',') { err = "lock: 顶层语法错"; return false; }
        ++i;
    }
    return true;
}

} // namespace

std::string localRepoDir() {
    if (const char* e = std::getenv("HAO_REPO")) {
        if (e[0]) return normalizePath(e);
    }
#ifdef _WIN32
    if (const char* home = std::getenv("USERPROFILE")) {
        if (home[0]) return normalizePath(joinPath(home, ".hao/repo"));
    }
    if (const char* home = std::getenv("HOME")) {
        if (home[0]) return normalizePath(joinPath(home, ".hao/repo"));
    }
    return normalizePath(".hao/repo");
#else
    if (const char* home = std::getenv("HOME")) {
        if (home[0]) return normalizePath(joinPath(home, ".hao/repo"));
    }
    return normalizePath(".hao/repo");
#endif
}

std::vector<std::string> resolveRegistries(const HaoProjectInfo& proj) {
    std::vector<std::string> regs;
    if (const char* e = std::getenv("HAO_REGISTRY")) {
        std::string s = e;
        size_t start = 0;
        while (start <= s.size()) {
            size_t comma = s.find(',', start);
            std::string part = s.substr(start, comma == std::string::npos ? std::string::npos
                                                                           : comma - start);
            // trim
            while (!part.empty() && (part.back() == ' ' || part.back() == '\t')) part.pop_back();
            size_t b = 0;
            while (b < part.size() && (part[b] == ' ' || part[b] == '\t')) ++b;
            part = part.substr(b);
            if (!part.empty()) regs.push_back(part);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    for (const auto& a : proj.registryAdditional) {
        if (!a.empty()) regs.push_back(a);
    }
    if (proj.registryIncludeDefault) {
        bool hasDefault = false;
        for (const auto& r : regs)
            if (r == kDefaultRegistry) { hasDefault = true; break; }
        if (!hasDefault) regs.push_back(kDefaultRegistry);
    }
    return regs;
}

bool loadLockFile(const std::string& projectDir, std::vector<LockEntry>& out,
                  std::string& errorOut) {
    errorOut.clear();
    out.clear();
    std::string path = joinPath(projectDir, kHaoLockFile);
    if (!fileExists(path)) return false;
    std::string src;
    if (!readFile(path, src)) {
        errorOut = "无法读取 " + path;
        return false;
    }
    if (!parseLockJson(src, out, errorOut)) {
        errorOut = path + ": " + errorOut;
        return false;
    }
    return true;
}


bool modTidy(const std::string& projectDir, std::string& errorOut, bool verbose) {
    errorOut.clear();
    HaoProjectInfo proj;
    if (!loadHaoProject(projectDir, proj, errorOut)) {
        if (errorOut.empty()) errorOut = "目录中无 " + std::string(kHaoProjectFile);
        return false;
    }

    auto regs = resolveRegistries(proj);

    struct Edge {
        std::string module;
        std::string versionConstraint;
        std::string from;
    };

    std::queue<Edge> q;
    for (const auto& dep : proj.dependencies) {
        q.push({dep.module, dep.version, "<root>"});
    }

    std::map<std::string, std::vector<std::pair<VersionConstraint, std::string>>> cons;
    std::map<std::string, LockEntry> resolved;
    std::set<std::string> childrenFetched;

    while (!q.empty()) {
        Edge edge = q.front();
        q.pop();

        if (proj.exclude.count(edge.module)) {
            if (verbose)
                std::cout << "[hao mod] exclude 跳过 " << edge.module
                          << "（来自 " << edge.from << "）\n";
            continue;
        }

        VersionConstraint constraint;
        std::string cerr;
        if (!parseVersionConstraint(edge.versionConstraint, constraint, cerr)) {
            errorOut = "依赖 " + edge.module + " 版本约束无效（" + cerr + "）: " +
                       edge.versionConstraint + "（来自 " + edge.from + "）";
            return false;
        }

        auto rit = proj.replace.find(edge.module);
        if (rit != proj.replace.end()) {
            const std::string& to = rit->second;
            if (looksLikeLocalPath(to) || isDirectory(joinPath(proj.projectDir, to))) {
                std::string local = looksLikeLocalPath(to) && (to[0] == '/' || to[0] == '\\' ||
                    (to.size() >= 2 && to[1] == ':'))
                    ? normalizePath(to)
                    : normalizePath(joinPath(proj.projectDir, to));
                if (!isDirectory(local)) {
                    errorOut = "replace 本地路径不存在: " + to;
                    return false;
                }
                auto it = resolved.find(edge.module);
                if (it == resolved.end()) {
                    LockEntry e;
                    e.module = edge.module;
                    e.version = formatSemVer(constraint.base);
                    e.registry = "replace:" + to;
                    e.replaced = true;
                    e.localPath = local;
                    e.sha256 = "";
                    addRequiredBy(e, edge.from);
                    resolved[edge.module] = e;
                    if (verbose)
                        std::cout << "[hao mod] replace " << edge.module << " → " << local << "\n";
                    std::vector<DepSpec> kids;
                    if (!loadPkgDependencies(local, kids, errorOut)) return false;
                    std::string mark = edge.module + "@" + e.version;
                    if (!childrenFetched.count(mark)) {
                        childrenFetched.insert(mark);
                        for (const auto& k : kids)
                            q.push({k.module, k.version, mark});
                    }
                } else {
                    addRequiredBy(it->second, edge.from);
                }
                cons[edge.module].push_back({constraint, edge.from});
                continue;
            }
            auto at = to.rfind('@');
            if (at != std::string::npos) {
                edge.module = to.substr(0, at);
                edge.versionConstraint = to.substr(at + 1);
                if (!parseVersionConstraint(edge.versionConstraint, constraint, cerr)) {
                    errorOut = "replace 目标版本无效（" + cerr + "）: " + to;
                    return false;
                }
            } else {
                errorOut = "无法解析 replace: " + edge.module + " → " + to;
                return false;
            }
        }

        cons[edge.module].push_back({constraint, edge.from});
        std::vector<VersionConstraint> allC;
        for (const auto& p : cons[edge.module]) allC.push_back(p.first);

        // 1) 本地仓先查；2) 未命中再从 HAO_REGISTRY 源拉取并写入本地仓
        std::string srcDir, resolvedVer, usedReg;
        bool fromLocal = false;
        std::string localMiss;
        std::vector<std::string> localOnly{localRepoDir()};
        if (resolveFromFileRegistriesAll(localOnly, edge.module, allC, srcDir, resolvedVer, usedReg,
                                        localMiss)) {
            fromLocal = true;
            usedReg = "local";
        } else if (!resolveFromFileRegistriesAll(regs, edge.module, allC, srcDir, resolvedVer,
                                                usedReg, errorOut)) {
            errorOut += "\n      约束来源:";
            for (const auto& p : cons[edge.module])
                errorOut += "\n        " + p.second + " → " + p.first.original;
            return false;
        }

        auto it = resolved.find(edge.module);
        if (it != resolved.end()) {
            if (it->second.version != resolvedVer) {
                errorOut = "依赖冲突: " + edge.module + " 无法同时满足既有锁定版 " +
                           it->second.version + " 与新选版 " + resolvedVer;
                errorOut += "\n      约束来源:";
                for (const auto& p : cons[edge.module])
                    errorOut += "\n        " + p.second + " → " + p.first.original;
                return false;
            }
            addRequiredBy(it->second, edge.from);
            continue;
        }

        std::string dest = repoPkgPath(edge.module, resolvedVer);
        if (!fromLocal || !sameNormalizedPath(srcDir, dest)) {
            std::error_code ec;
            if (fs::exists(dest, ec)) fs::remove_all(dest, ec);
            if (!copyTree(srcDir, dest, errorOut)) return false;
        }
        std::string hashErr;
        std::string hash = sha256DirTree(dest, hashErr);
        if (hash.empty()) { errorOut = hashErr; return false; }

        LockEntry e;
        e.module = edge.module;
        e.version = resolvedVer;
        e.registry = usedReg;
        e.sha256 = hash;
        addRequiredBy(e, edge.from);
        resolved[edge.module] = e;
        if (verbose)
            std::cout << "[hao mod] " << edge.module << "@" << resolvedVer
                      << "（约束 " << edge.versionConstraint << "，来自 " << edge.from
                      << "）← " << usedReg
                      << (fromLocal ? "（本地仓命中）" : " → 本地仓") << "\n";

        std::vector<DepSpec> kids;
        if (!loadPkgDependencies(dest, kids, errorOut)) return false;
        std::string mark = edge.module + "@" + resolvedVer;
        if (!childrenFetched.count(mark)) {
            childrenFetched.insert(mark);
            for (const auto& k : kids)
                q.push({k.module, k.version, mark});
        }
    }

    std::vector<LockEntry> locks;
    locks.reserve(resolved.size());
    for (auto& kv : resolved) locks.push_back(kv.second);
    std::sort(locks.begin(), locks.end(),
              [](const LockEntry& a, const LockEntry& b) { return a.module < b.module; });

    if (!writeLockFile(proj.projectDir, locks, errorOut)) return false;
    if (verbose)
        std::cout << "[hao mod] 已写入 " << joinPath(proj.projectDir, kHaoLockFile) << "\n";
    return true;
}

bool ensureDepsSearchRoots(const HaoProjectInfo& proj,
                           std::vector<std::string>& roots,
                           std::string& errorOut) {
    errorOut.clear();
    if (proj.dependencies.empty()) return true;

    std::vector<LockEntry> locks;
    std::string lockErr;
    bool hasLock = loadLockFile(proj.projectDir, locks, lockErr);
    if (!lockErr.empty()) { errorOut = lockErr; return false; }

    bool needTidy = !hasLock;
    if (!needTidy) {
        for (const auto& e : locks) {
            if (e.replaced) continue;
            if (!isDirectory(repoPkgPath(e.module, e.version))) { needTidy = true; break; }
        }
    }
    if (!needTidy) {
        for (const auto& d : proj.dependencies) {
            if (proj.exclude.count(d.module)) continue;
            bool found = false;
            for (const auto& e : locks)
                if (e.module == d.module) { found = true; break; }
            if (!found) { needTidy = true; break; }
        }
    }
    if (needTidy) {
        if (!modTidy(proj.projectDir, errorOut, false)) return false;
        locks.clear();
        if (!loadLockFile(proj.projectDir, locks, errorOut)) {
            if (errorOut.empty()) errorOut = "tidy 后仍无 lock";
            return false;
        }
    }

    for (auto& e : locks) {
        if (e.replaced) {
            auto rit = proj.replace.find(e.module);
            if (rit == proj.replace.end()) {
                errorOut = "lock 含 replaced 但项目无 replace: " + e.module;
                return false;
            }
            const std::string& to = rit->second;
            e.localPath = normalizePath(joinPath(proj.projectDir, to));
            if (!isDirectory(e.localPath) && looksLikeLocalPath(to))
                e.localPath = normalizePath(to);
            if (!isDirectory(e.localPath)) {
                errorOut = "replace 路径不存在: " + to;
                return false;
            }
            roots.push_back(e.localPath);
            continue;
        }

        std::string dest = repoPkgPath(e.module, e.version);
        if (!isDirectory(dest)) {
            if (!modTidy(proj.projectDir, errorOut, false)) return false;
            dest = repoPkgPath(e.module, e.version);
            if (!isDirectory(dest)) {
                errorOut = "本地仓缺失: " + dest;
                return false;
            }
        }
        if (!e.sha256.empty()) {
            std::string herr;
            std::string h = sha256DirTree(dest, herr);
            if (h.empty()) { errorOut = herr; return false; }
            if (h != e.sha256) {
                errorOut = "包校验失败（sha256 不匹配）: " + e.module + "@" + e.version +
                           "\n      期望 " + e.sha256 + "\n      实际 " + h +
                           "\n      请重新 hao mod tidy";
                return false;
            }
        }
        roots.push_back(dest);
    }
    return true;
}

bool modWhy(const std::string& projectDir, const std::string& module, std::string& errorOut) {
    errorOut.clear();
    std::vector<LockEntry> locks;
    if (!loadLockFile(projectDir, locks, errorOut)) {
        if (errorOut.empty())
            errorOut = "无 " + std::string(kHaoLockFile) + "，请先 hao mod tidy";
        return false;
    }
    for (const auto& e : locks) {
        if (e.module != module) continue;
        std::cout << module << "@" << e.version << "\n";
        if (e.requiredBy.empty()) {
            std::cout << "  （lock 无 requiredBy 信息）\n";
        } else {
            for (const auto& r : e.requiredBy)
                std::cout << "  required by " << r << "\n";
        }
        return true;
    }
    errorOut = "lock 中无模块: " + module;
    return false;
}

} // namespace hao
