// ============================================================
//  HaoLang 编译驱动 —— 包 / import 解析
// ------------------------------------------------------------
//  从入口文件出发，按 Go 风格的"目录即包"模型递归解析 import：
//    - 同一目录下所有 .hao 的 package 名必须一致；
//    - import 的点分路径（如 "util.strings"）映射到目录，
//      先在入口文件所在目录下找，再找编译器自带 stdlib/src/；
//    - 整盘解析成多棵语法树，交给 IRGen 一次性生成 IR。
//
//  本类持有 ANTLR 输入流 / token 流 / parser 与语法树的生命周期，
//  IRGen 仅借用其中的 CompilationUnitContext*。
// ============================================================
#pragma once

#include "HaoLangLexer.h"
#include "HaoLangParser.h"
#include "antlr4-runtime.h"
#include "irgen/IRGen.h"
#include "sema/Diagnostic.h"

#include <memory>
#include <string>
#include <vector>

namespace hao {

struct ResolvedPackage {
    std::string importPath;      // "" 表示 main（入口）包
    std::string packageName;
    std::string dir;
    std::vector<std::string> files;
};

class PackageResolver {
public:
    // 额外包搜索根（haoproject.json 的 localReferences；在入口目录之后、stdlib 之前）。
    void addSearchRoot(const std::string& dir);

    // v0.42：testMode 时编译 *_test.hao 并隐式加载 stdlib testing；否则排除测试文件。
    void setTestMode(bool v) { testMode_ = v; }

    // 入口可为 .hao 文件或目录。解析入口包及其全部 import 闭包。
    // 成功返回 true；错误经 diags 报告。
    bool resolve(const std::vector<std::string>& inputs, DiagnosticEngine& diags);

    // 把解析结果交给 IRGen：每个 .hao 文件一个 SourceUnit。
    // 在 resolve() 成功后调用。
    std::vector<IRGen::SourceUnit> sourceUnits();

    // 编译器自带标准库源码根目录（stdlib/src），供 import 查找
    static std::string stdlibSrcDir();

private:
    struct ParsedFile {
        std::string path;
        std::string source;
        std::unique_ptr<antlr4::ANTLRInputStream> input;
        std::unique_ptr<HaoLangLexer> lexer;
        std::unique_ptr<antlr4::CommonTokenStream> tokens;
        std::unique_ptr<HaoLangParser> parser;
        HaoLangParser::CompilationUnitContext* tree = nullptr;
        std::string packageName;
        std::string importPath;
    };

    // 解析一个目录为一个包；importPath 为 "" 表示入口 main 包。
    // includeTestFiles：仅入口被测包在 testMode 下为 true；依赖/stdlib 永不编入 *_test.hao。
    bool loadPackageDir(const std::string& dir, const std::string& importPath,
                        const std::string& fromFile, DiagnosticEngine& diags,
                        bool includeTestFiles);

    // 加载一组明确文件作为一个包（显式文件入口用），并递归加载 import。
    bool loadPackageFiles(const std::vector<std::string>& haoFiles,
                          const std::string& dirForDedup,
                          const std::string& importPath,
                          const std::string& fromFile,
                          DiagnosticEngine& diags,
                          bool includeTestFiles);

    // 解析单个文件（已分配 ParsedFile），读 packageName，收集其 import 目录
    bool parseFile(ParsedFile& pf, DiagnosticEngine& diags);

    // 把点分 import 路径解析为文件系统目录；找不到返回空串
    std::string locatePackage(const std::string& dottedImport,
                              const std::string& fromFile);

    std::vector<std::unique_ptr<ParsedFile>> files_;
    std::vector<std::string> loadedDirs_;     // 已加载目录（去重）
    std::string entryDir_;                    // 第一个入口所在目录
    std::vector<std::string> searchRoots_;    // localReferences 等额外根
    bool testMode_ = false;
};

} // namespace hao
