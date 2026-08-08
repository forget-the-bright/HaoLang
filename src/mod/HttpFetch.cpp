// ============================================================
//  HTTP(S) 拉取：WinHTTP + tar 解压（工具链侧）
// ============================================================

#include "mod/HttpFetch.h"

#include "util/FileUtil.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

namespace hao {

namespace {

#ifdef _WIN32

std::wstring toWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

bool httpGetWin(const std::string& url, std::string& body, const std::string& bearerToken,
                const std::string& proxyUrl, std::string& errorOut) {
    body.clear();
    std::wstring wurl = toWide(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[512] = {};
    wchar_t path[2048] = {};
    wchar_t extra[1024] = {};
    wchar_t scheme[32] = {};
    uc.lpszScheme = scheme;
    uc.dwSchemeLength = 31;
    uc.lpszHostName = host;
    uc.dwHostNameLength = 511;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 1023;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        errorOut = "无法解析 URL: " + url;
        return false;
    }
    bool https = (_wcsicmp(scheme, L"https") == 0);
    DWORD access = WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;
    std::wstring wproxy;
    LPCWSTR proxy = WINHTTP_NO_PROXY_NAME;
    LPCWSTR proxyBypass = WINHTTP_NO_PROXY_BYPASS;
    if (!proxyUrl.empty()) {
        access = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
        wproxy = toWide(proxyUrl);
        proxy = wproxy.c_str();
    }
    HINTERNET session =
        WinHttpOpen(L"HaoLang-mod/0.47", access, proxy, proxyBypass, 0);
    if (!session) {
        errorOut = "WinHttpOpen 失败";
        return false;
    }
    HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
    if (!conn) {
        WinHttpCloseHandle(session);
        errorOut = "WinHttpConnect 失败: " + url;
        return false;
    }
    std::wstring fullPath = path;
    if (extra[0]) fullPath += extra;
    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(conn, L"GET", fullPath.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) {
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        errorOut = "WinHttpOpenRequest 失败";
        return false;
    }
    if (!bearerToken.empty()) {
        std::wstring hdr = L"Authorization: Bearer " + toWide(bearerToken) + L"\r\n";
        WinHttpAddRequestHeaders(req, hdr.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    }
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0,
                            0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        errorOut = "HTTP 请求失败: " + url;
        return false;
    }
    DWORD status = 0, sz = sizeof(status);
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX)) {
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        errorOut = "无法读取 HTTP 状态码";
        return false;
    }
    if (status != 200) {
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        errorOut = "HTTP " + std::to_string(status) + ": " + url;
        return false;
    }
    char buf[8192];
    DWORD read = 0;
    while (WinHttpReadData(req, buf, sizeof(buf), &read) && read > 0) {
        body.append(buf, read);
        read = 0;
    }
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return true;
}

#else

bool httpGetWin(const std::string&, std::string&, const std::string&, const std::string&,
                std::string& errorOut) {
    errorOut = "当前宿主未实现 HTTP 拉取";
    return false;
}

#endif

} // namespace

std::string envHaoToken() {
    if (const char* e = std::getenv("HAO_TOKEN")) return e;
    return {};
}

std::string envHaoProxy() {
    if (const char* e = std::getenv("HAO_PROXY")) return e;
    return {};
}

bool httpDownloadToString(const std::string& url, std::string& out, const std::string& bearerToken,
                          const std::string& proxyUrl, std::string& errorOut) {
    return httpGetWin(url, out, bearerToken, proxyUrl, errorOut);
}

bool httpDownloadToFile(const std::string& url, const std::string& destFile,
                        const std::string& bearerToken, const std::string& proxyUrl,
                        std::string& errorOut) {
    std::string body;
    if (!httpGetWin(url, body, bearerToken, proxyUrl, errorOut)) return false;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(destFile).parent_path(), ec);
    if (!writeFile(destFile, body)) {
        errorOut = "无法写入 " + destFile;
        return false;
    }
    return true;
}

bool extractZipArchive(const std::string& zipPath, const std::string& destDir,
                       std::string& errorOut) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(destDir, ec);
    if (ec) {
        errorOut = "无法创建解压目录: " + destDir;
        return false;
    }
#ifdef _WIN32
    std::string cmd = "tar -xf \"" + zipPath + "\" -C \"" + destDir + "\"";
#else
    std::string cmd = "unzip -oq \"" + zipPath + "\" -d \"" + destDir + "\"";
#endif
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        errorOut = "解压失败（退出码 " + std::to_string(rc) + "）: " + zipPath;
        return false;
    }
    return true;
}

bool parseVersionsJson(const std::string& json, std::vector<std::string>& out,
                       std::string& errorOut) {
    out.clear();
    size_t i = 0;
    auto skipWs = [&] {
        while (i < json.size() &&
               (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n'))
            ++i;
    };
    skipWs();
    if (i >= json.size() || json[i] != '[') {
        errorOut = "versions.json 须为字符串数组";
        return false;
    }
    ++i;
    skipWs();
    if (i < json.size() && json[i] == ']') return true;
    for (;;) {
        skipWs();
        if (i >= json.size() || json[i] != '"') {
            errorOut = "versions.json 项须为字符串";
            return false;
        }
        ++i;
        std::string s;
        while (i < json.size() && json[i] != '"') {
            if (json[i] == '\\' && i + 1 < json.size()) {
                s.push_back(json[i + 1]);
                i += 2;
            } else
                s.push_back(json[i++]);
        }
        if (i >= json.size()) {
            errorOut = "versions.json 字符串未结束";
            return false;
        }
        ++i;
        out.push_back(s);
        skipWs();
        if (i < json.size() && json[i] == ']') break;
        if (i >= json.size() || json[i] != ',') {
            errorOut = "versions.json 语法错";
            return false;
        }
        ++i;
    }
    return true;
}

} // namespace hao
