// ============================================================
//  HaoLang 编译器入口 —— hao 命令
// ============================================================

#include "antlr4-runtime.h"
#include "HaoLangLexer.h"
#include "HaoLangParser.h"
#include "HaoVersion.h"
#include "driver/Driver.h"
#include "driver/DriverResolve.h"
#include "mod/HaoProject.h"
#include "mod/ModResolve.h"
#include "tool/HaoFmt.h"
#include "tool/HaoTest.h"
#include "util/ConsoleUtil.h"
#include "util/FileUtil.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>


using namespace antlr4;

namespace {


// 收集词法/语法错误，取代 ANTLR 默认的"打印后继续"行为
class HaoErrorListener : public BaseErrorListener {
public:
    struct Diag { size_t line, column; std::string message; };
    std::vector<Diag> diags;

    void syntaxError(Recognizer*, Token*, size_t line, size_t col,
                     const std::string& msg, std::exception_ptr) override {
        diags.push_back({line, col, msg});
    }
};

void printUsage() {
    std::cout <<
        "HaoLang 编译器 v" << hao::version() << "\n"
        "\n"
        "用法: hao <命令> [参数]\n"
        "\n"
        "命令:\n"
        "  build <file或目录...>  编译为可执行文件\n"
        "  run   <file或目录...>  编译并立即运行\n"
        "  emit  <file或目录...>  只生成 LLVM IR (.ll)\n"
        "  parse <file.hao>       打印语法树\n"
        "  tokens <file.hao>      打印词法记号流\n"
        "  mod   <子命令>         项目清单（haoproject.json）\n"
        "  test  [路径...]        跑 *_test.hao 中的 TestXxx（对标 go test）\n"
        "  fmt   [选项] <路径...> 空白规范化（-w 写回 / --check）\n"
        "  clean                清理构建产物\n"
        "  env                  显示工具链环境\n"
        "  version              显示版本\n"
        "\n"
        "选项:\n"
        "  -o <file>            指定输出文件\n"
        "  --target <平台>      交叉编译目标（win-amd64 / linux-amd64）\n"
        "  --keep-ir            保留中间 .ll 文件\n"
        "  -v, --verbose        显示执行的外部命令\n"
        "  -l<name>             链接外部库（如 -lws2_32，可重复）\n"
        "  -L<dir>              库搜索路径（可重复）\n"
        "  --link <file>        直接链接的文件（.lib/.a/.o/.c 源码，可重复）\n";
}

void printModUsage() {
    std::cout <<
        "用法: hao mod <子命令>\n"
        "\n"
        "子命令:\n"
        "  init [模块路径]           在当前目录创建 haoproject.json\n"
        "  tidy [项目目录]           解析依赖图，填充全局缓存并写 lock\n"
        "  why <模块> [项目目录]     显示某模块为何进入 lock（requiredBy）\n"
        "\n"
        "说明（见记忆文档 5.15）：\n"
        "  - 清单：haoproject.json（不用 hao.mod）\n"
        "  - dependencies：精确版或 ^/~/>=；传递依赖读 haopkg.json\n"
        "  - exclude：从图中排除模块；冲突硬失败\n"
        "  - 源仓库：HAO_REGISTRY（http / file；拉取后写入本地仓）\n"
        "  - 本地仓：HAO_REPO（默认 ~/.hao/repo；布局 module/version）\n";
}

int cmdMod(int argc, char** argv) {
    if (argc < 3) {
        printModUsage();
        return 1;
    }
    std::string sub = argv[2];
    if (sub == "help" || sub == "-h" || sub == "--help") {
        printModUsage();
        return 0;
    }
    if (sub == "init") {
        std::string module;
        if (argc >= 4) module = argv[3];
        std::string err;
        if (!hao::initHaoProject(".", module, err)) {
            std::cerr << "错误: " << err << "\n";
            return 1;
        }
        std::cout << "已创建 " << hao::kHaoProjectFile;
        if (!module.empty()) std::cout << "（module=" << module << "）";
        std::cout << "\n";
        return 0;
    }
    if (sub == "tidy") {
        std::string dir = ".";
        if (argc >= 4) dir = argv[3];
        std::string err;
        if (!hao::modTidy(dir, err, true)) {
            std::cerr << "错误: " << err << "\n";
            return 1;
        }
        std::cout << "tidy 完成\n";
        return 0;
    }
    if (sub == "why") {
        if (argc < 4) {
            std::cerr << "错误: hao mod why 需要模块路径\n";
            printModUsage();
            return 1;
        }
        std::string module = argv[3];
        std::string dir = ".";
        if (argc >= 5) dir = argv[4];
        std::string err;
        if (!hao::modWhy(dir, module, err)) {
            std::cerr << "错误: " << err << "\n";
            return 1;
        }
        return 0;
    }
    std::cerr << "错误: 未知 mod 子命令 '" << sub << "'\n\n";
    printModUsage();
    return 1;
}

void printFmtUsage() {
    std::cout <<
        "用法: hao fmt [选项] <文件或目录...>\n"
        "\n"
        "选项:\n"
        "  -w, --write     写回文件\n"
        "  --check         若需格式化则退出码 1（不写）\n"
        "  -v, --verbose   显示未改动的文件\n"
        "\n"
        "说明：去行尾空白、CRLF→LF、末换行、4 空格花括号缩进；先语法校验。\n"
        "  不断行/运算符旁空格重排。\n";
}

int cmdFmt(int argc, char** argv) {
    hao::FmtOptions opts;
    std::vector<std::string> paths;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { printFmtUsage(); return 0; }
        if (a == "-w" || a == "--write") { opts.write = true; continue; }
        if (a == "--check") { opts.check = true; continue; }
        if (a == "-v" || a == "--verbose") { opts.verbose = true; continue; }
        if (!a.empty() && a[0] == '-') {
            std::cerr << "错误: 未知选项 '" << a << "'\n";
            printFmtUsage();
            return 1;
        }
        paths.push_back(a);
    }
    if (opts.write && opts.check) {
        std::cerr << "错误: -w 与 --check 不能同时使用\n";
        return 1;
    }
    return hao::runFmt(paths, opts);
}

int cmdTest(int argc, char** argv) {
    hao::TestOptions opts;
    std::vector<std::string> paths;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            std::cout << "用法: hao test [路径...] [-v] [-run regexp]\n"
                         "  发现同包 *_test.hao 中的 func TestXxx(t: testing.T)。\n"
                         "  无参数：有 haoproject.json 或当前目录含 *_test.hao → `.`。\n"
                         "  不跑业务 main；产物在 target/test/hao-test/，跑完删除。\n";
            return 0;
        }
        if (a == "-v" || a == "--verbose") { opts.verbose = true; continue; }
        if (a == "-run" && i + 1 < argc) {
            opts.runPattern = argv[++i];
            continue;
        }
        if (a == "--") {
            // 保留兼容：忽略其后参数（测试不传业务 main）
            break;
        }
        if (!a.empty() && a[0] == '-') {
            std::cerr << "错误: 未知选项 '" << a << "'\n";
            return 1;
        }
        paths.push_back(a);
    }
    return hao::runTests(paths, opts);
}


void printTree(tree::ParseTree* node, HaoLangParser& parser, int depth = 0) {
    std::string indent(depth * 2, ' ');

    if (auto* term = dynamic_cast<tree::TerminalNode*>(node)) {
        Token* tok = term->getSymbol();
        if (tok->getType() == Token::EOF) return;
        std::string sym(parser.getVocabulary().getSymbolicName(tok->getType()));
        std::cout << indent << "'" << tok->getText() << "'  <" << sym << ">\n";
        return;
    }

    auto* ctx = dynamic_cast<ParserRuleContext*>(node);
    if (!ctx) return;

    std::cout << indent << parser.getRuleNames()[ctx->getRuleIndex()] << "\n";
    for (auto* child : ctx->children) printTree(child, parser, depth + 1);
}

int cmdTokens(const std::string& path) {
    std::string src;
    if (!hao::readFile(path, src)) {
        std::cerr << "错误: 无法读取文件 " << path << "\n";
        return 1;
    }

    ANTLRInputStream input(src);
    HaoLangLexer lexer(&input);
    HaoErrorListener errs;
    lexer.removeErrorListeners();
    lexer.addErrorListener(&errs);

    CommonTokenStream tokens(&lexer);
    tokens.fill();

    std::cout << "词法记号流 (" << path << "):\n";
    for (Token* tok : tokens.getTokens()) {
        if (tok->getType() == Token::EOF) { std::cout << "  <EOF>\n"; break; }
        if (tok->getChannel() != Token::DEFAULT_CHANNEL) continue;

        std::string name(lexer.getVocabulary().getSymbolicName(tok->getType()));
        printf("  %3zu:%-3zu  %-22s %s\n",
               tok->getLine(), tok->getCharPositionInLine(),
               name.c_str(), tok->getText().c_str());
    }

    for (const auto& d : errs.diags)
        std::cerr << path << ":" << d.line << ":" << d.column
                  << ": 词法错误: " << d.message << "\n";
    return errs.diags.empty() ? 0 : 1;
}

int cmdParse(const std::string& path) {
    std::string src;
    if (!hao::readFile(path, src)) {
        std::cerr << "错误: 无法读取文件 " << path << "\n";
        return 1;
    }

    ANTLRInputStream input(src);
    HaoLangLexer lexer(&input);
    HaoErrorListener lexErrs;
    lexer.removeErrorListeners();
    lexer.addErrorListener(&lexErrs);

    CommonTokenStream tokens(&lexer);
    HaoLangParser parser(&tokens);
    HaoErrorListener parseErrs;
    parser.removeErrorListeners();
    parser.addErrorListener(&parseErrs);

    auto* tree = parser.compilationUnit();

    bool failed = false;
    for (const auto& d : lexErrs.diags) {
        std::cerr << path << ":" << d.line << ":" << d.column
                  << ": 词法错误: " << d.message << "\n";
        failed = true;
    }
    for (const auto& d : parseErrs.diags) {
        std::cerr << path << ":" << d.line << ":" << d.column
                  << ": 语法错误: " << d.message << "\n";
        failed = true;
    }
    if (failed) { std::cerr << "\n解析失败。\n"; return 1; }

    std::cout << "语法树 (" << path << "):\n";
    printTree(tree, parser);
    std::cout << "\n解析成功。\n";
    return 0;
}

int cmdEnv() {
    auto host = hao::hostPlatform();
    std::cout << "HaoLang 工具链环境:\n"
              << "  版本        : " << hao::version() << "\n"
              << "  宿主平台    : " << hao::platformName(host) << "\n"
              << "  目标三元组  : " << host.triple << "\n"
              << "  分发包名    : " << hao::distName(host) << "\n"
              << "  ANTLR       : " << hao::antlrVersion() << "\n"
              << "  LLVM        : " << hao::llvmVersion() << "\n"
              << "  clang       : " << hao::Driver::findClang() << "\n";
    std::string rt = hao::Driver::findRuntimeLib();
    std::cout << "  运行时库    : " << (rt.empty() ? "(未找到 libhaort.a)" : rt) << "\n";
    std::string stdSrc = hao::PackageResolver::stdlibSrcDir();
    std::cout << "  标准库源码  : " << stdSrc << "\n";
    std::string winCrt = hao::Driver::findWinCrtLibDir();
    if (!winCrt.empty())
        std::cout << "  Win CRT 库  : " << winCrt << "\n";
    else if (std::string(host.os) == "win")
        std::cout << "  Win CRT 库  : (未找到，请执行 script\\fetch_winlibs.ps1)\n";
    std::cout << "  本地仓      : " << hao::localRepoDir() << "\n";
    if (const char* reg = std::getenv("HAO_REGISTRY")) {
        if (reg[0])
            std::cout << "  源仓库      : " << reg << "\n";
        else
            std::cout << "  源仓库      : (HAO_REGISTRY 空；includeDefault 时用官方 URL)\n";
    } else {
        std::cout << "  源仓库      : (未设 HAO_REGISTRY；includeDefault 时用 "
                  << hao::kDefaultRegistry << ")\n";
    }
    std::cout << "  官方默认源  : " << hao::kDefaultRegistry << "\n";
    return 0;
}

// hao clean：删除当前目录下的构建中间产物
int cmdClean() {
    const char* patterns[] = {".ll", ".hao-run.exe"};
    int removed = 0;
    // 只清理与 .hao 同名的产物，避免误删用户文件
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(".", ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        std::string name = entry.path().filename().string();
        for (const char* suf : patterns) {
            size_t sl = std::strlen(suf);
            if (name.size() > sl && name.compare(name.size() - sl, sl, suf) == 0) {
                if (fs::remove(entry.path(), ec)) {
                    std::cout << "删除 " << name << "\n";
                    ++removed;
                }
            }
        }
    }
    std::cout << (removed ? "清理完成，共删除 " + std::to_string(removed) + " 个文件\n"
                          : "没有需要清理的文件\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    hao::initConsoleUtf8();

    if (argc < 2) { printUsage(); return 1; }

    std::string cmd = argv[1];

    if (cmd == "version" || cmd == "--version" || cmd == "-V") {
        std::cout << "HaoLang 编译器 v" << hao::version()
                  << " (" << hao::platformName(hao::hostPlatform()) << ")\n"
                  << "  ANTLR " << hao::antlrVersion()
                  << " / LLVM " << hao::llvmVersion() << "\n";
        return 0;
    }
    if (cmd == "help" || cmd == "--help" || cmd == "-h") { printUsage(); return 0; }
    if (cmd == "env") return cmdEnv();
    if (cmd == "clean") return cmdClean();
    if (cmd == "mod") return cmdMod(argc, argv);
    if (cmd == "fmt") return cmdFmt(argc, argv);
    if (cmd == "test") return cmdTest(argc, argv);

    // ---- 解析选项 ----
    hao::BuildOptions opts;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--") {
            // `--` 之后全部是用户程序参数（传给 main(args)），不再视为编译选项
            for (int j = i + 1; j < argc; ++j) opts.programArgs.push_back(argv[j]);
            break;
        } else if (a == "-o" && i + 1 < argc) {
            opts.outputFile = argv[++i];
        } else if (a == "--target" && i + 1 < argc) {
            opts.target = argv[++i];
        } else if (a == "--keep-ir") {
            opts.keepIR = true;
        } else if (a == "-v" || a == "--verbose") {
            opts.verbose = true;
        } else if (a == "--link" && i + 1 < argc) {
            // 直接链接的文件（.lib/.a/.o/.c 源码），可重复
            opts.linkFiles.push_back(argv[++i]);
        } else if (a.size() > 2 && a[0] == '-' && (a[1] == 'l' || a[1] == 'L')) {
            // -l<name> 链接库名 / -L<dir> 库搜索路径（可重复）
            if (a[1] == 'l') opts.linkLibs.push_back(a.substr(2));
            else             opts.linkDirs.push_back(a.substr(2));
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "错误: 未知选项 '" << a << "'\n";
            return 1;
        } else {
            // 支持多个 .hao 文件或目录（同一包）
            if (opts.sourceFile.empty()) opts.sourceFile = a;
            opts.sourceFiles.push_back(a);
        }
    }

    if (opts.sourceFile.empty()) {
        std::cerr << "错误: 命令 '" << cmd << "' 需要源文件或目录参数\n\n";
        printUsage();
        return 1;
    }

    hao::Driver driver;

    if (cmd == "build")  return driver.build(opts);
    if (cmd == "run")    return driver.run(opts);
    if (cmd == "emit") {
        opts.emitIROnly = true;
        opts.keepIR = true;
        return driver.build(opts);
    }
    if (cmd == "parse")  return cmdParse(opts.sourceFile);
    if (cmd == "tokens") return cmdTokens(opts.sourceFile);

    std::cerr << "错误: 未知命令 '" << cmd << "'\n\n";
    printUsage();
    return 1;
}
