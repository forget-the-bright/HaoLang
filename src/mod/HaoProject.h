// ============================================================
//  HaoLang 项目清单 haoproject.json（第 0～1 层）
// ------------------------------------------------------------
//  见记忆文档 5.15。
// ============================================================
#pragma once

#include <map>
#include <string>
#include <vector>

namespace hao {

struct BuildOptions;

inline constexpr const char* kHaoProjectFile = "haoproject.json";

struct DepSpec {
    std::string module;
    std::string version; // 精确版或 semver 约束（^/~/>=/=）
};

struct HaoProjectInfo {
    std::string name;
    std::string module;
    std::string version = "0.1.0";
    std::string haoVersion;
    std::string main;
    std::string target;
    std::string output;

    std::vector<std::string> localReferences;
    std::vector<DepSpec> dependencies;
    std::map<std::string, std::string> replace; // module → 本地路径 或 module@version
    std::map<std::string, std::string> exclude; // module → 约束（键存在即从图中排除）

    std::vector<std::string> registryAdditional;
    bool registryIncludeDefault = true;

    std::string projectDir;
    std::string filePath;
};

bool loadHaoProject(const std::string& dir, HaoProjectInfo& out, std::string& errorOut);

bool loadHaoProjectForInput(const std::string& inputPath, HaoProjectInfo& out,
                            std::string& errorOut);

bool initHaoProject(const std::string& dir, const std::string& moduleOrName,
                    std::string& errorOut);

std::vector<std::string> resolveLocalReferenceRoots(const HaoProjectInfo& proj,
                                                    std::vector<std::string>* missing);

// 应用清单：localReferences + dependencies（经 lock/cache）→ packageSearchRoots
bool applyHaoProjectToOptions(BuildOptions& opts, std::string& errorOut);

} // namespace hao
