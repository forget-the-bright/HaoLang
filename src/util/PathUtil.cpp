// ============================================================
//  HaoLang —— 路径与进程工具实现
// ============================================================

#include "util/PathUtil.h"

#include <fstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace hao {

std::string exeDir() {
#ifdef _WIN32
    char buf[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf, n);
    size_t slash = p.find_last_of("\\/");
    return slash == std::string::npos ? "." : p.substr(0, slash);
#else
    return ".";
#endif
}

bool fileExists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

std::string stripExt(const std::string& path) {
    size_t dot = path.find_last_of('.');
    size_t slash = path.find_last_of("\\/");
    if (dot == std::string::npos) return path;
    if (slash != std::string::npos && dot < slash) return path;
    return path.substr(0, dot);
}

std::string quote(const std::string& path) {
    if (path.find(' ') == std::string::npos) return path;
    return "\"" + path + "\"";
}

std::string toWinPath(const std::string& path) {
    std::string r = path;
    for (char& c : r) if (c == '/') c = '\\';
    return r;
}

std::string dirName(const std::string& path) {
    size_t slash = path.find_last_of("\\/");
    if (slash == std::string::npos) return ".";
    if (slash == 0) return path.substr(0, 1);   // 根目录
    return path.substr(0, slash);
}

std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    char last = a.back();
    if (last == '/' || last == '\\') return a + b;
    return a + "/" + b;
}

std::string importToFsPath(const std::string& dotted) {
    std::string r;
    r.reserve(dotted.size());
    for (char c : dotted) r += (c == '.' ? '/' : c);
    return r;
}

} // namespace hao
