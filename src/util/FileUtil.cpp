// ============================================================
//  HaoLang —— 文件读写工具实现
// ============================================================

#include "util/FileUtil.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace hao {

bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    // 跳过 UTF-8 BOM：不去掉会被词法器当作非法字符报错。
    if (out.size() >= 3 &&
        static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB &&
        static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);
    }
    return true;
}

bool writeFile(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}

bool isDirectory(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
}

bool listHaoFiles(const std::string& dir, std::vector<std::string>& out) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return false;

    std::vector<std::string> names;
    for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) return false;
        if (!e.is_regular_file(ec)) continue;
        auto p = e.path();
        if (p.extension() == ".hao")
            names.push_back(p.filename().string());
    }
    std::sort(names.begin(), names.end());
    for (auto& n : names) {
        out.push_back(dir);
        out.back() += "/";
        out.back() += n;
    }
    return true;
}

bool listHaoFilesRecursive(const std::string& dir, std::vector<std::string>& out) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return false;

    std::vector<std::string> paths;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (ec) return false;
        if (!it->is_regular_file(ec)) continue;
        auto p = it->path();
        if (p.extension() == ".hao")
            paths.push_back(p.generic_string());
    }
    std::sort(paths.begin(), paths.end());
    for (auto& p : paths) out.push_back(p);
    return true;
}

bool isHaoTestFile(const std::string& path) {
    // 匹配 …_test.hao（大小写敏感，与 Go 的 *_test.go 一致）
    static const char kSuf[] = "_test.hao";
    constexpr size_t kLen = sizeof(kSuf) - 1;
    if (path.size() < kLen) return false;
    return path.compare(path.size() - kLen, kLen, kSuf) == 0;
}

} // namespace hao
