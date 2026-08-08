// ============================================================
//  hao test —— 发现 TestXxx、生成 harness、testMode 编译运行
// ============================================================

#include "tool/HaoTest.h"

#include "HaoLangLexer.h"
#include "HaoLangParser.h"
#include "HaoVersion.h"
#include "antlr4-runtime.h"
#include "driver/Driver.h"
#include "mod/HaoProject.h"
#include "util/FileUtil.h"
#include "util/PathUtil.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>

namespace hao {

namespace {

namespace fs = std::filesystem;

class SilentErr : public antlr4::BaseErrorListener {
public:
    void syntaxError(antlr4::Recognizer*, antlr4::Token*, size_t, size_t,
                     const std::string&, std::exception_ptr) override {}
};

std::string sanitizeName(const std::string& path) {
    std::string s;
    for (char c : path) {
        if (c == '/' || c == '\\' || c == ':' || c == ' ' || c == '.')
            s.push_back('_');
        else
            s.push_back(c);
    }
    if (s.empty()) s = "test";
    if (s.size() > 80) s = s.substr(s.size() - 80);
    return s;
}

bool dirHasTestFile(const std::string& dir) {
    std::vector<std::string> files;
    if (!listHaoFiles(dir, files)) return false;
    for (const auto& f : files)
        if (isHaoTestFile(f)) return true;
    return false;
}

std::vector<std::string> defaultTestPaths() {
    if (fileExists(kHaoProjectFile) || fileExists(std::string("./") + kHaoProjectFile))
        return {"."};
    if (dirHasTestFile("."))
        return {"."};
    return {};
}

// Test 后须大写字母（对齐 Go TestXxx）
bool isTestFuncName(const std::string& name) {
    if (name.size() < 5 || name.compare(0, 4, "Test") != 0) return false;
    char c = name[4];
    return c >= 'A' && c <= 'Z';
}

bool isTestingTType(HaoLangParser::TypeContext* t) {
    if (!t || !t->baseType()) return false;
    auto* named = dynamic_cast<HaoLangParser::NamedTypeContext*>(t->baseType());
    if (!named) return false;
    std::string text = named->qualifiedName()->getText();
    return text == "testing.T" || text == "T";
}

struct TestCase {
    std::string name;
};

// 从单个 *_test.hao 收集合法 TestXxx
bool collectTestsFromFile(const std::string& path, std::vector<TestCase>& out,
                          std::string& err) {
    std::string src;
    if (!readFile(path, src)) {
        err = "无法读取 " + path;
        return false;
    }
    antlr4::ANTLRInputStream input(src);
    HaoLangLexer lexer(&input);
    SilentErr quiet;
    lexer.removeErrorListeners();
    lexer.addErrorListener(&quiet);
    antlr4::CommonTokenStream tokens(&lexer);
    HaoLangParser parser(&tokens);
    parser.removeErrorListeners();
    parser.addErrorListener(&quiet);
    auto* tree = parser.compilationUnit();
    if (!tree) {
        err = path + ": 解析失败";
        return false;
    }
    for (auto* decl : tree->topLevelDecl()) {
        auto* fn = decl->funcDecl();
        if (!fn) continue;
        std::string name = fn->IDENT()->getText();
        if (!isTestFuncName(name)) continue;
        if (fn->typeParams()) {
            err = path + ": " + name + " 不能是泛型函数";
            return false;
        }
        auto* pl = fn->paramList();
        if (!pl || pl->param().size() != 1) {
            err = path + ": " + name + " 须为单参 func TestXxx(t: testing.T)";
            return false;
        }
        if (!isTestingTType(pl->param(0)->type())) {
            err = path + ": " + name + " 参数类型须为 testing.T";
            return false;
        }
        if (fn->returnType()) {
            // 允许显式 Unit；其它返回类型拒绝
            std::string rt = fn->returnType()->type()->getText();
            if (rt != "Unit") {
                err = path + ": " + name + " 不能有非 Unit 返回类型";
                return false;
            }
        }
        out.push_back({name});
    }
    return true;
}

std::string packageNameOfFile(const std::string& path) {
    std::string src;
    if (!readFile(path, src)) return "main";
    antlr4::ANTLRInputStream input(src);
    HaoLangLexer lexer(&input);
    SilentErr quiet;
    lexer.removeErrorListeners();
    lexer.addErrorListener(&quiet);
    antlr4::CommonTokenStream tokens(&lexer);
    HaoLangParser parser(&tokens);
    parser.removeErrorListeners();
    parser.addErrorListener(&quiet);
    auto* tree = parser.compilationUnit();
    if (!tree || !tree->packageDecl()) return "main";
    return tree->packageDecl()->qualifiedName()->getText();
}

bool matchRun(const std::string& name, const std::string& pattern) {
    if (pattern.empty()) return true;
    try {
        std::regex re(pattern);
        return std::regex_search(name, re);
    } catch (const std::regex_error&) {
        return name.find(pattern) != std::string::npos;
    }
}

std::string writeHarness(const std::string& outPath, const std::string& pkg,
                         const std::vector<TestCase>& cases, bool verbose) {
    std::ostringstream ss;
    ss << "// 由 hao test 自动生成 —— 勿手改\n";
    ss << "package " << pkg << ";\n\n";
    ss << "import testing;\n\n";
    ss << "func main(): Int {\n";
    ss << "    testing.setVerbose(" << (verbose ? "true" : "false") << ");\n";
    ss << "    var failed: Int = 0;\n";
    for (const auto& c : cases) {
        ss << "    failed += testing.runCase(\"" << c.name << "\", " << c.name
           << ");\n";
    }
    ss << "    if (failed == 0) {\n";
    ss << "        fmt.println(\"PASS\");\n";
    ss << "        return 0;\n";
    ss << "    }\n";
    ss << "    fmt.println(\"FAIL\");\n";
    ss << "    return 1;\n";
    ss << "}\n";

    std::ofstream f(outPath, std::ios::binary);
    if (!f) return "无法写入 " + outPath;
    f << ss.str();
    return "";
}

// 收集包目录下全部源文件（含 *_test.hao，不含 harness）
bool collectPackageFiles(const std::string& dir, std::vector<std::string>& out) {
    std::vector<std::string> files;
    if (!listHaoFiles(dir, files)) return false;
    for (const auto& f : files) {
        // 跳过残留 harness
        if (f.size() >= 18 &&
            f.compare(f.size() - 18, 18, "__hao_test_main.hao") == 0)
            continue;
        out.push_back(f);
    }
    return !out.empty();
}

int runOnePackage(const std::string& path, const TestOptions& opts) {
    std::string dir = path;
    std::vector<std::string> explicitProd;

    if (isDirectory(path)) {
        dir = path;
    } else if (fileExists(path)) {
        dir = dirName(path);
        // 单文件：若是 *_test.hao 仍编整个包；若是生产文件则整包测
    } else {
        std::cerr << "错误: 路径不存在: " << path << "\n";
        return 1;
    }

    std::vector<std::string> pkgFiles;
    if (!collectPackageFiles(dir, pkgFiles)) {
        std::cerr << "错误: 目录无 .hao 文件: " << dir << "\n";
        return 1;
    }

    std::vector<TestCase> cases;
    std::string err;
    bool sawTestFile = false;
    for (const auto& f : pkgFiles) {
        if (!isHaoTestFile(f)) continue;
        sawTestFile = true;
        if (!collectTestsFromFile(f, cases, err)) {
            std::cerr << "错误: " << err << "\n";
            return 1;
        }
    }
    if (!sawTestFile) {
        std::cerr << "错误: " << dir << " 中没有 *_test.hao（hao test 不跑业务 main）\n";
        return 1;
    }

    std::vector<TestCase> filtered;
    for (const auto& c : cases)
        if (matchRun(c.name, opts.runPattern)) filtered.push_back(c);

    if (filtered.empty()) {
        std::cerr << "错误: 没有匹配的 TestXxx";
        if (!opts.runPattern.empty())
            std::cerr << "（-run " << opts.runPattern << "）";
        std::cerr << "\n";
        return 1;
    }

    // 稳定顺序
    std::sort(filtered.begin(), filtered.end(),
              [](const TestCase& a, const TestCase& b) { return a.name < b.name; });

    std::string pkg = "main";
    for (const auto& f : pkgFiles) {
        if (isHaoTestFile(f)) {
            pkg = packageNameOfFile(f);
            break;
        }
    }

    fs::create_directories("target/test/hao-test");
    std::string base = sanitizeName(dir);
    std::string harness = joinPath("target/test/hao-test", base + "__hao_test_main.hao");
    std::string exe = joinPath("target/test/hao-test", base + hostPlatform().exeExt);
    std::string ll = stripExt(exe) + ".ll";

    err = writeHarness(harness, pkg, filtered, opts.verbose);
    if (!err.empty()) {
        std::cerr << "错误: " << err << "\n";
        return 1;
    }

    std::vector<std::string> buildFiles = pkgFiles;
    buildFiles.push_back(harness);

    BuildOptions bo;
    bo.sourceFile = harness;
    bo.sourceFiles = buildFiles;
    bo.outputFile = exe;
    bo.verbose = opts.verbose;
    bo.quiet = true;
    bo.testMode = true;

    auto t0 = std::chrono::steady_clock::now();
    Driver driver;
    if (driver.build(bo) != 0) {
        std::cout << "FAIL\t" << path << "\t(编译失败)\n";
        std::error_code ec;
        fs::remove(exe, ec);
        fs::remove(ll, ec);
        fs::remove(harness, ec);
        return 1;
    }

    std::string winExe = toWinPath(exe);
    if (winExe.find('\\') == std::string::npos)
        winExe = ".\\" + winExe;

    if (opts.verbose) {
        std::cout << "[hao test] " << quote(winExe) << "\n";
        std::cout.flush();
    }
    int rc = std::system(quote(winExe).c_str());

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count();

    std::error_code ec;
    fs::remove(exe, ec);
    fs::remove(ll, ec);
    fs::remove(harness, ec);

    if (rc != 0) {
        std::cout << "FAIL\t" << path << "\t(" << filtered.size() << " 用例, "
                  << ms << "ms)\n";
        return 1;
    }
    std::cout << "ok  \t" << path << "\t(" << filtered.size() << " 用例, " << ms
              << "ms)\n";
    return 0;
}

} // namespace

int runTests(const std::vector<std::string>& paths, const TestOptions& opts) {
    std::vector<std::string> targets = paths;
    if (targets.empty()) {
        targets = defaultTestPaths();
        if (targets.empty()) {
            std::cerr << "错误: 请指定测试路径，或在含 haoproject.json / *_test.hao 的目录运行\n";
            return 1;
        }
    }

    int fail = 0;
    for (const auto& p : targets)
        fail += runOnePackage(p, opts);

    if (fail == 0)
        std::cout << "PASS\t" << targets.size() << " 组\n";
    else
        std::cout << "FAIL\t" << fail << " / " << targets.size() << " 组失败\n";
    return fail ? 1 : 0;
}

} // namespace hao
