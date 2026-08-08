// ============================================================
//  包管理：依赖解析 / 本地仓 / lock / 源仓库（第 1～2 层）
// ============================================================
#pragma once

#include "mod/HaoProject.h"

#include <string>
#include <vector>

namespace hao {

inline constexpr const char* kHaoLockFile = "haoproject.lock.json";
inline constexpr const char* kDefaultRegistry = "https://pkg.haolang.org";

struct LockEntry {
    std::string module;
    std::string version;    // 解析后的精确版本
    std::string registry;   // 逻辑来源（源仓 URL/路径，或 "local"）
    std::string sha256;
    bool replaced = false;  // replace 到本地路径时 true（无 sha / 不进本地仓）
    std::string localPath;  // replace 本地路径（绝对）
    std::vector<std::string> requiredBy; // 谁依赖了它（"<root>" 或 module@version）
};

// 本地仓根：HAO_REPO，否则 ~/.hao/repo
std::string localRepoDir();

// 合并后的**源**仓库列表（高→低）：HAO_REGISTRY > 项目 additional > 官方 URL
// 本地仓不在此列；包落盘与编译读取见 localRepoDir / repoPkgPath。
std::vector<std::string> resolveRegistries(const HaoProjectInfo& proj);

// 解析依赖并填充本地仓，写入 haoproject.lock.json。
bool modTidy(const std::string& projectDir, std::string& errorOut, bool verbose = false);

// 读 lock；不存在返回 false 且 errorOut 空。
bool loadLockFile(const std::string& projectDir, std::vector<LockEntry>& out,
                  std::string& errorOut);

// 确保依赖已解析：有 dependencies 时若无 lock 或本地仓缺失则自动 tidy。
// 成功后把包搜索根追加到 roots（以 lock 全量为准，含传递依赖；路径均在本地仓）。
bool ensureDepsSearchRoots(const HaoProjectInfo& proj,
                           std::vector<std::string>& roots,
                           std::string& errorOut);

// 打印 module 为何进入依赖图（读 lock 的 requiredBy）。
bool modWhy(const std::string& projectDir, const std::string& module,
            std::string& errorOut);

} // namespace hao
