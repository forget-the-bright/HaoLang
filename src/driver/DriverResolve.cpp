// ============================================================
//  HaoLang 编译驱动 —— 包 / import 解析实现
// ============================================================

#include "driver/DriverResolve.h"

#include "HaoLangLexer.h"
#include "HaoLangParser.h"
#include "HaoVersion.h"
#include "antlr4-runtime.h"
#include "util/FileUtil.h"
#include "util/PathUtil.h"

#include <algorithm>
#include <filesystem>
#include <set>

namespace hao {

namespace {

class CollectErrors : public antlr4::BaseErrorListener {
public:
    explicit CollectErrors(DiagnosticEngine& d, const std::string& f)
        : diags_(d), file_(f) {}
    void syntaxError(antlr4::Recognizer*, antlr4::Token*,
                     size_t line, size_t col, const std::string& msg,
                     std::exception_ptr) override {
        diags_.error(file_, line, col, msg);
    }
private:
    DiagnosticEngine& diags_;
    std::string file_;
};

// P2：无源码行时仍带文件路径（空则 hao），禁止 ?:0:0 误伤
void resolveError(DiagnosticEngine& diags, const std::string& file,
                  const std::string& msg) {
    diags.error(file.empty() ? "hao" : file, 0, 0, msg);
}

// 取语法树的 package 名；无 packageDecl 返回 ""
std::string packageNameOf(HaoLangParser::CompilationUnitContext* tree) {
    if (auto* pd = tree->packageDecl())
        return pd->qualifiedName()->getText();
    return "";
}

} // namespace

void PackageResolver::addSearchRoot(const std::string& dir) {
    if (dir.empty()) return;
    for (const auto& r : searchRoots_)
        if (r == dir) return;
    searchRoots_.push_back(dir);
}

std::string PackageResolver::stdlibSrcDir() {
    // 与 findRuntimeLib 同样的相对路径优先，保证绿色分发后仍指向自带 stdlib。
    std::string dir = exeDir();
    std::vector<std::string> cands = {
        dir + "/../stdlib/src",
        dir + "/stdlib/src",
    };
    // 发行包布局（旁边有自带 clang）时，禁止回退到编译期绝对路径
    // HAO_STDLIB_DIR（如 D:/buildLang/stdlib）——否则本机测包装会「碰巧成功」，
    // 别人机器没有该目录才报「找不到包 net」。
    bool portableDist =
        fileExists(dir + "/../lib/llvm/bin/clang.exe") ||
        fileExists(dir + "/lib/llvm/bin/clang.exe");
    if (!portableDist && std::string(HAO_STDLIB_DIR).size()) {
        cands.push_back(std::string(HAO_STDLIB_DIR) + "/src");
    }
    for (const auto& c : cands)
        if (isDirectory(c)) return c;
    // 返回默认位置（即便不存在，调用方据此报错）
    return cands[0];
}

bool PackageResolver::parseFile(ParsedFile& pf, DiagnosticEngine& diags) {
    if (!readFile(pf.path, pf.source)) {
        diags.error(pf.path, 0, 0, "无法读取源文件");
        return false;
    }

    pf.input = std::make_unique<antlr4::ANTLRInputStream>(pf.source);
    pf.lexer = std::make_unique<HaoLangLexer>(pf.input.get());
    pf.lexer->removeErrorListeners();
    CollectErrors lexErr(diags, pf.path);
    pf.lexer->addErrorListener(&lexErr);

    pf.tokens = std::make_unique<antlr4::CommonTokenStream>(pf.lexer.get());
    pf.parser = std::make_unique<HaoLangParser>(pf.tokens.get());
    pf.parser->removeErrorListeners();
    CollectErrors parseErr(diags, pf.path);
    pf.parser->addErrorListener(&parseErr);

    pf.tree = pf.parser->compilationUnit();
    if (diags.hasErrors()) return false;

    pf.packageName = packageNameOf(pf.tree);
    return true;
}

bool PackageResolver::loadPackageDir(const std::string& dir,
                                     const std::string& importPath,
                                     const std::string& fromFile,
                                     DiagnosticEngine& diags,
                                     bool includeTestFiles) {
    std::vector<std::string> haoFiles;
    if (!listHaoFiles(dir, haoFiles) || haoFiles.empty()) {
        resolveError(diags, fromFile,
            "找不到包 '" + importPath + "'（目录 " + dir + " 中没有 .hao 文件）");
        return false;
    }
    if (!includeTestFiles) {
        std::vector<std::string> filtered;
        for (const auto& f : haoFiles)
            if (!isHaoTestFile(f)) filtered.push_back(f);
        haoFiles.swap(filtered);
        if (haoFiles.empty()) {
            resolveError(diags, fromFile,
                "找不到包 '" + importPath + "'（目录仅有 *_test.hao，普通构建已排除）");
            return false;
        }
    }
    return loadPackageFiles(haoFiles, dir, importPath, fromFile, diags, includeTestFiles);
}

// 加载一组明确的文件作为一个包，并递归加载它们的 import。
// dirForDedup 用于去重（已加载过的目录/文件集不再重复）。
bool PackageResolver::loadPackageFiles(const std::vector<std::string>& haoFiles,
                                       const std::string& dirForDedup,
                                       const std::string& importPath,
                                       const std::string& fromFile,
                                       DiagnosticEngine& diags,
                                       bool includeTestFiles) {
    std::string key = dirForDedup;
    if (key.empty()) {
        // 显式文件列表：用排序后的文件列表作为去重键
        std::vector<std::string> sorted = haoFiles;
        std::sort(sorted.begin(), sorted.end());
        for (auto& f : sorted) { key += f; key += "|"; }
    }
    for (const auto& k : loadedDirs_) if (k == key) return true;
    loadedDirs_.push_back(key);

    // 先解析所有文件，校验 package 名一致
    std::vector<std::string> files = haoFiles;
    if (!includeTestFiles) {
        std::vector<std::string> filtered;
        for (const auto& f : files)
            if (!isHaoTestFile(f)) filtered.push_back(f);
        files.swap(filtered);
        if (files.empty()) {
            resolveError(diags, fromFile.empty() ? dirForDedup : fromFile,
                "没有可编译的 .hao 文件（*_test.hao 仅由 hao test 编译）");
            return false;
        }
    }

    std::vector<ParsedFile*> added;
    std::string pkgName;
    for (const auto& f : files) {
        auto pf = std::make_unique<ParsedFile>();
        pf->path = f;
        pf->importPath = importPath;
        if (!parseFile(*pf, diags)) return false;

        if (pkgName.empty()) pkgName = pf->packageName;
        else if (pf->packageName != pkgName) {
            resolveError(diags, f,
                "包 '" + importPath + "' 的文件包名不一致：'" +
                pkgName + "' 与 '" + pf->packageName + "'");
            return false;
        }
        added.push_back(pf.get());
        files_.push_back(std::move(pf));
    }

    // 非 main 包要求声明了 package
    if (!importPath.empty() && pkgName.empty()) {
        resolveError(diags, files.empty() ? fromFile : files[0],
            "包 '" + importPath + "' 中的文件缺少 package 声明");
        return false;
    }

    // 递归加载依赖
    std::set<std::string> seen;
    for (ParsedFile* pf : added) {
        for (auto* imp : pf->tree->importDecl()) {
            std::string dotted = imp->qualifiedName()->getText();
            if (!seen.insert(dotted).second) continue;
            std::string subdir = locatePackage(dotted, pf->path);
            if (subdir.empty()) {
                auto* tok = imp->getStart();
                std::string fsRel;
                for (char c : dotted) fsRel += (c == '.' ? '/' : c);
                std::string hint = "已搜索: ";
                if (!entryDir_.empty())
                    hint += joinPath(entryDir_, fsRel) + "；";
                for (const auto& root : searchRoots_)
                    hint += joinPath(root, fsRel) + "；";
                hint += joinPath(stdlibSrcDir(), fsRel);
                hint += "（请保持 bin/ 与 stdlib/src/ 同级，勿只拷贝 hao.exe）";
                diags.error(pf->path, tok->getLine(), tok->getCharPositionInLine(),
                            "找不到包 '" + dotted + "'\n      " + hint);
                return false;
            }
            // 与 IRGen 一致：importPath 存斜杠形式（demo.web → demo/web），
            // 嵌套包前缀才是 demo$web$ 而非错误的 demo.web$
            std::string slashPath = dotted;
            for (char& c : slashPath) if (c == '.') c = '/';
            // 依赖包永不编入其 *_test.hao（对齐 go test）
            if (!loadPackageDir(subdir, slashPath, pf->path, diags, false)) return false;
        }
    }
    return true;
}

std::string PackageResolver::locatePackage(const std::string& dottedImport,
                                          const std::string& fromFile) {
    (void)fromFile;
    std::string fsRel;
    for (char c : dottedImport) fsRel += (c == '.' ? '/' : c);

    // 1. 相对于入口文件所在目录
    if (!entryDir_.empty()) {
        std::string cand = joinPath(entryDir_, fsRel);
        if (isDirectory(cand)) return cand;
    }
    // 2. 本地项目引用根（haoproject.json → localReferences）
    for (const auto& root : searchRoots_) {
        std::string cand = joinPath(root, fsRel);
        if (isDirectory(cand)) return cand;
    }
    // 3. 编译器自带标准库（永不走远程仓库）
    {
        std::string cand = joinPath(stdlibSrcDir(), fsRel);
        if (isDirectory(cand)) return cand;
    }
    return "";
}

bool PackageResolver::resolve(const std::vector<std::string>& inputs,
                              DiagnosticEngine& diags) {
    if (inputs.empty()) {
        resolveError(diags, "hao", "没有输入文件");
        return false;
    }

    // P2：后续无文件上下文的回退用入口路径
    diags.setDefaultFile(inputs[0]);

    std::vector<std::string> explicitFiles;
    std::string dirInput;
    for (const auto& in : inputs) {
        if (isDirectory(in)) {
            if (!dirInput.empty()) {
                resolveError(diags, in, "一次只能指定一个入口目录");
                return false;
            }
            dirInput = in;
        } else {
            explicitFiles.push_back(in);
        }
    }

    if (!dirInput.empty() && !explicitFiles.empty()) {
        resolveError(diags, inputs[0], "不能同时指定目录和文件作为入口");
        return false;
    }

    // 仅入口被测包在 testMode 下纳入 *_test.hao；依赖/stdlib 永不编入测试文件
    const bool entryTests = testMode_;

    if (!dirInput.empty()) {
        entryDir_ = dirInput;
        if (!loadPackageDir(dirInput, "", "", diags, entryTests)) return false;
    } else {
        // 显式文件入口：只编译列出的文件（同包），不扫描整个目录。
        entryDir_ = dirName(explicitFiles[0]);
        if (!loadPackageFiles(explicitFiles, entryDir_, "", "", diags, entryTests))
            return false;
    }

    // 隐式加载标准库 fmt 包（v0.9.0 起 fmt 是 .hao 包，无需显式 import 即可用）。
    {
        std::string fmtDir = joinPath(stdlibSrcDir(), "fmt");
        if (isDirectory(fmtDir))
            loadPackageDir(fmtDir, "fmt", "", diags, false);
    }
    {
        std::string objDir = joinPath(stdlibSrcDir(), "object");
        if (isDirectory(objDir))
            loadPackageDir(objDir, "object", "", diags, false);
    }
    {
        std::string langDir = joinPath(stdlibSrcDir(), "lang");
        if (isDirectory(langDir))
            loadPackageDir(langDir, "lang", "", diags, false);
    }
    // v0.42：testMode 隐式加载 testing（源码仍须 import testing 才能写类型名）
    if (testMode_) {
        std::string testingDir = joinPath(stdlibSrcDir(), "testing");
        if (isDirectory(testingDir))
            loadPackageDir(testingDir, "testing", "", diags, false);
    }
    return true;
}

std::vector<IRGen::SourceUnit> PackageResolver::sourceUnits() {
    std::vector<IRGen::SourceUnit> result;
    for (const auto& pf : files_) {
        IRGen::SourceUnit u;
        u.path = pf->path;
        u.package = pf->packageName;
        u.importPath = pf->importPath;
        u.tree = pf->tree;
        result.push_back(std::move(u));
    }
    return result;
}

} // namespace hao
