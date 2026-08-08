// ============================================================
//  包管理 HTTP(S) 拉取（工具链侧，非 runtime）
// ============================================================
#pragma once

#include <string>
#include <vector>

namespace hao {

// GET url → 写入 destFile。可选 Bearer token 与代理 URL（如 http://host:port）。
bool httpDownloadToFile(const std::string& url, const std::string& destFile,
                        const std::string& bearerToken, const std::string& proxyUrl,
                        std::string& errorOut);

// GET url → 内存字符串（小 JSON）。
bool httpDownloadToString(const std::string& url, std::string& out,
                          const std::string& bearerToken, const std::string& proxyUrl,
                          std::string& errorOut);

// 解压 zip 到 destDir（Windows 用 tar -xf）。
bool extractZipArchive(const std::string& zipPath, const std::string& destDir,
                       std::string& errorOut);

// 读环境变量 HAO_TOKEN / HAO_PROXY（可空）。
std::string envHaoToken();
std::string envHaoProxy();

// 解析 versions.json：["1.0.0","1.1.0"] → 版本字符串列表。
bool parseVersionsJson(const std::string& json, std::vector<std::string>& out,
                       std::string& errorOut);

} // namespace hao
