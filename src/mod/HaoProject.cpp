// ============================================================
//  haoproject.json 读写（最小 JSON 子集，无第三方依赖）
// ============================================================

#include "mod/HaoProject.h"

#include "driver/Driver.h"
#include "mod/ModResolve.h"
#include "util/FileUtil.h"
#include "util/PathUtil.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace hao {

namespace {

struct JsonParser {
    const std::string& s;
    size_t i = 0;

    explicit JsonParser(const std::string& src) : s(src) {}

    void skipWs() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' ||
                                s[i] == '\n'))
            ++i;
    }

    bool match(char c) {
        skipWs();
        if (i < s.size() && s[i] == c) { ++i; return true; }
        return false;
    }

    bool expect(char c, std::string& err) {
        if (!match(c)) {
            err = std::string("期望 '") + c + "'（偏移 " + std::to_string(i) + "）";
            return false;
        }
        return true;
    }

    bool parseString(std::string& out, std::string& err) {
        skipWs();
        if (i >= s.size() || s[i] != '"') {
            err = "期望字符串（偏移 " + std::to_string(i) + "）";
            return false;
        }
        ++i;
        out.clear();
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\') {
                ++i;
                if (i >= s.size()) { err = "字符串转义未结束"; return false; }
                char e = s[i++];
                switch (e) {
                case '"': case '\\': case '/': out.push_back(e); break;
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                default: out.push_back(e); break;
                }
            } else {
                out.push_back(s[i++]);
            }
        }
        if (i >= s.size() || s[i] != '"') {
            err = "字符串缺少结束引号";
            return false;
        }
        ++i;
        return true;
    }

    // 跳过任意 JSON 值（对象/数组/字符串/数字/true/false/null）
    bool skipValue(std::string& err) {
        skipWs();
        if (i >= s.size()) { err = "意外结束"; return false; }
        char c = s[i];
        if (c == '"') {
            std::string tmp;
            return parseString(tmp, err);
        }
        if (c == '{') {
            ++i;
            skipWs();
            if (match('}')) return true;
            for (;;) {
                std::string key;
                if (!parseString(key, err)) return false;
                if (!expect(':', err)) return false;
                if (!skipValue(err)) return false;
                skipWs();
                if (match('}')) return true;
                if (!expect(',', err)) return false;
            }
        }
        if (c == '[') {
            ++i;
            skipWs();
            if (match(']')) return true;
            for (;;) {
                if (!skipValue(err)) return false;
                skipWs();
                if (match(']')) return true;
                if (!expect(',', err)) return false;
            }
        }
        // 字面量 / 数字
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '-' ||
            c == 't' || c == 'f' || c == 'n') {
            while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' &&
                   !std::isspace(static_cast<unsigned char>(s[i])))
                ++i;
            return true;
        }
        err = std::string("无法识别的 JSON 值（偏移 ") + std::to_string(i) + "）";
        return false;
    }

    bool parseStringArray(std::vector<std::string>& out, std::string& err) {
        out.clear();
        if (!expect('[', err)) return false;
        skipWs();
        if (match(']')) return true;
        for (;;) {
            std::string item;
            if (!parseString(item, err)) return false;
            out.push_back(item);
            skipWs();
            if (match(']')) return true;
            if (!expect(',', err)) return false;
        }
    }

    bool parseStringMap(std::map<std::string, std::string>& out, std::string& err) {
        out.clear();
        if (!expect('{', err)) return false;
        skipWs();
        if (match('}')) return true;
        for (;;) {
            std::string key, val;
            if (!parseString(key, err)) return false;
            if (!expect(':', err)) return false;
            if (!parseString(val, err)) return false;
            out[key] = val;
            skipWs();
            if (match('}')) return true;
            if (!expect(',', err)) return false;
        }
    }

    bool parseDependencies(std::vector<DepSpec>& out, std::string& err) {
        out.clear();
        std::map<std::string, std::string> m;
        if (!parseStringMap(m, err)) return false;
        for (const auto& kv : m) {
            DepSpec d;
            d.module = kv.first;
            d.version = kv.second;
            out.push_back(d);
        }
        return true;
    }

    bool parseRegistry(HaoProjectInfo& info, std::string& err) {
        if (!expect('{', err)) return false;
        skipWs();
        if (match('}')) return true;
        for (;;) {
            std::string key;
            if (!parseString(key, err)) return false;
            if (!expect(':', err)) return false;
            if (key == "additional") {
                if (!parseStringArray(info.registryAdditional, err)) return false;
            } else if (key == "includeDefault") {
                skipWs();
                if (s.compare(i, 4, "true") == 0) {
                    info.registryIncludeDefault = true;
                    i += 4;
                } else if (s.compare(i, 5, "false") == 0) {
                    info.registryIncludeDefault = false;
                    i += 5;
                } else {
                    err = "includeDefault 须为 true/false";
                    return false;
                }
            } else {
                if (!skipValue(err)) return false;
            }
            skipWs();
            if (match('}')) return true;
            if (!expect(',', err)) return false;
        }
    }

    bool parseProjectObject(HaoProjectInfo& info, std::string& err) {
        if (!expect('{', err)) return false;
        skipWs();
        if (match('}')) return true;
        for (;;) {
            std::string key;
            if (!parseString(key, err)) return false;
            if (!expect(':', err)) return false;
            if (key == "name" || key == "module" || key == "version" ||
                key == "haoVersion" || key == "main" || key == "target" ||
                key == "output") {
                std::string val;
                if (!parseString(val, err)) return false;
                if (key == "name") info.name = val;
                else if (key == "module") info.module = val;
                else if (key == "version") info.version = val;
                else if (key == "haoVersion") info.haoVersion = val;
                else if (key == "main") info.main = val;
                else if (key == "target") info.target = val;
                else if (key == "output") info.output = val;
            } else {
                if (!skipValue(err)) return false;
            }
            skipWs();
            if (match('}')) return true;
            if (!expect(',', err)) return false;
        }
    }

    bool parseRoot(HaoProjectInfo& info, std::string& err) {
        if (!expect('{', err)) return false;
        skipWs();
        if (match('}')) return true;
        for (;;) {
            std::string key;
            if (!parseString(key, err)) return false;
            if (!expect(':', err)) return false;
            if (key == "project") {
                if (!parseProjectObject(info, err)) return false;
            } else if (key == "localReferences") {
                if (!parseStringArray(info.localReferences, err)) return false;
            } else if (key == "dependencies") {
                if (!parseDependencies(info.dependencies, err)) return false;
            } else if (key == "replace") {
                if (!parseStringMap(info.replace, err)) return false;
            } else if (key == "exclude") {
                if (!parseStringMap(info.exclude, err)) return false;
            } else if (key == "registry") {
                if (!parseRegistry(info, err)) return false;
            } else {
                if (!skipValue(err)) return false;
            }
            skipWs();
            if (match('}')) return true;
            if (!expect(',', err)) return false;
        }
    }
};

std::string jsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\t': o += "\\t"; break;
        case '\r': o += "\\r"; break;
        default:   o.push_back(c); break;
        }
    }
    return o;
}

std::string defaultNameFromModule(const std::string& module) {
    if (module.empty()) return "app";
    size_t slash = module.find_last_of("/\\");
    if (slash == std::string::npos) return module;
    return module.substr(slash + 1);
}

std::string normalizeDir(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p = fs::absolute(dir, ec);
    if (ec) return dir;
    p = p.lexically_normal();
    std::string s = p.generic_string();
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\'))
        s.pop_back();
    return s;
}

} // namespace

bool loadHaoProject(const std::string& dir, HaoProjectInfo& out, std::string& errorOut) {
    errorOut.clear();
    std::string path = joinPath(dir, kHaoProjectFile);
    if (!fileExists(path)) return false;

    std::string src;
    if (!readFile(path, src)) {
        errorOut = "无法读取 " + path;
        return false;
    }

    HaoProjectInfo info;
    JsonParser p(src);
    if (!p.parseRoot(info, errorOut)) {
        errorOut = path + ": " + errorOut;
        return false;
    }
    if (info.name.empty() && info.module.empty()) {
        errorOut = path + ": project.name 与 project.module 至少填写一个";
        return false;
    }
    if (info.name.empty()) info.name = defaultNameFromModule(info.module);
    if (info.module.empty()) info.module = info.name;

    info.projectDir = normalizeDir(dir);
    info.filePath = joinPath(info.projectDir, kHaoProjectFile);
    out = std::move(info);
    return true;
}

bool loadHaoProjectForInput(const std::string& inputPath, HaoProjectInfo& out,
                            std::string& errorOut) {
    errorOut.clear();
    std::string dir = isDirectory(inputPath) ? inputPath : dirName(inputPath);
    return loadHaoProject(dir, out, errorOut);
}

bool initHaoProject(const std::string& dir, const std::string& moduleOrName,
                    std::string& errorOut) {
    errorOut.clear();
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        errorOut = "目录不存在: " + dir;
        return false;
    }
    std::string path = joinPath(dir, kHaoProjectFile);
    if (fileExists(path)) {
        errorOut = "已存在 " + path;
        return false;
    }

    std::string module = moduleOrName.empty() ? "app" : moduleOrName;
    std::string name = defaultNameFromModule(module);

    std::ostringstream ss;
    ss << "{\n"
       << "  \"project\": {\n"
       << "    \"name\": \"" << jsonEscape(name) << "\",\n"
       << "    \"module\": \"" << jsonEscape(module) << "\",\n"
       << "    \"version\": \"0.1.0\"\n"
       << "  },\n"
       << "  \"localReferences\": []\n"
       << "}\n";

    std::ofstream f(path, std::ios::binary);
    if (!f) {
        errorOut = "无法写入 " + path;
        return false;
    }
    f << ss.str();
    return true;
}

std::vector<std::string> resolveLocalReferenceRoots(const HaoProjectInfo& proj,
                                                    std::vector<std::string>* missing) {
    std::vector<std::string> roots;
    namespace fs = std::filesystem;
    for (const auto& ref : proj.localReferences) {
        if (ref.empty()) continue;
        fs::path p(ref);
        if (p.is_relative())
            p = fs::path(proj.projectDir) / p;
        std::error_code ec;
        p = fs::weakly_canonical(p, ec);
        if (ec) p = fs::absolute(fs::path(proj.projectDir) / ref, ec);
        std::string s = p.generic_string();
        while (s.size() > 1 && (s.back() == '/' || s.back() == '\\'))
            s.pop_back();
        if (!isDirectory(s)) {
            if (missing) missing->push_back(ref);
            continue;
        }
        roots.push_back(s);
    }
    return roots;
}

bool applyHaoProjectToOptions(BuildOptions& opts, std::string& errorOut) {
    errorOut.clear();
    std::string entry = opts.sourceFiles.empty() ? opts.sourceFile
                                                 : opts.sourceFiles[0];
    if (entry.empty()) return true;

    HaoProjectInfo proj;
    if (!loadHaoProjectForInput(entry, proj, errorOut)) {
        // errorOut 非空 = 清单存在但非法；空 = 无清单
        return errorOut.empty();
    }

    if (opts.target.empty() && !proj.target.empty())
        opts.target = proj.target;
    if (opts.outputFile.empty() && !proj.output.empty())
        opts.outputFile = proj.output;

    if (!proj.main.empty()) {
        bool dirEntry = opts.sourceFiles.size() == 1 && isDirectory(opts.sourceFiles[0]);
        if (!dirEntry && opts.sourceFiles.empty() && isDirectory(opts.sourceFile))
            dirEntry = true;
        if (dirEntry) {
            std::string mainPath = joinPath(proj.projectDir, proj.main);
            if (!fileExists(mainPath)) {
                errorOut = "haoproject.json 的 project.main 不存在: " + mainPath;
                return false;
            }
            opts.sourceFile = mainPath;
            opts.sourceFiles = {mainPath};
        }
    }

    std::vector<std::string> missing;
    opts.packageSearchRoots = resolveLocalReferenceRoots(proj, &missing);
    if (!missing.empty()) {
        errorOut = "localReferences 路径不存在:";
        for (const auto& m : missing) errorOut += " " + m;
        return false;
    }

    // 第 1 层：dependencies → 全局 cache / replace 本地路径
    if (!ensureDepsSearchRoots(proj, opts.packageSearchRoots, errorOut))
        return false;
    return true;
}

} // namespace hao
