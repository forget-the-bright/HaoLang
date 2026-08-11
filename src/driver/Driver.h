// ============================================================
//  HaoLang 编译驱动
// ------------------------------------------------------------
//  负责串起完整流程：
//    .hao --解析--> 语法树 --IRGen--> .ll --clang--> .exe
//
//  工具链路径在编译期由 CMake 注入（HAO_LLVM_DIR / HAO_STDLIB_DIR），
//  并在运行时按 exe 所在位置回退，保证绿色分发后依然可用。
// ============================================================
#pragma once

#include <string>
#include <vector>

namespace hao {

struct BuildOptions {
    std::string sourceFile;      // 入口 .hao（兼容单文件）
    std::vector<std::string> sourceFiles;  // 多文件/目录输入（为空时用 sourceFile）
    std::string outputFile;      // 输出可执行文件（空则由源文件名推导）
    std::string target;          // 交叉编译目标，如 linux-amd64；空则用宿主平台
    bool keepIR = false;         // 保留中间 .ll
    bool emitIROnly = false;     // 只生成 .ll，不编译
    bool emitDebug = false;      // I0/I3/I4：.ll 挂 !dbg；链接传 clang -g
    bool verbose = false;        // 打印执行的外部命令
    bool quiet = false;          // 抑制「编译成功」等提示（hao test 用）
    bool testMode = false;       // v0.42：纳入 *_test.hao；跳过业务 main；加载 testing

    // ---- 外部 C 库链接（v0.10.0）----
    std::vector<std::string> linkLibs;    // 库名，追加 -l<name>（如 ws2_32）
    std::vector<std::string> linkDirs;    // 库搜索路径，追加 -L<dir>
    std::vector<std::string> linkFiles;   // 直接链接的文件（.lib/.a/.o/.c 源码）

    // ---- 程序参数（v0.19.0）----
    // `hao run a.hao -- p1 p2` 中 `--` 之后的参数，运行时拼到用户程序命令行，
    // 经 C argc/argv 传入 `func main(args: [String])`。
    std::vector<std::string> programArgs;

    // ---- 项目清单（v0.38 · haoproject.json）----
    // 由 applyHaoProjectToOptions 填充：localReferences 解析后的包搜索根。
    std::vector<std::string> packageSearchRoots;
};

class Driver {
public:
    // 编译；成功返回 0
    int build(const BuildOptions& opts);

    // 编译并立即运行，返回被执行程序的退出码
    int run(const BuildOptions& opts);

    // 供 hao env 使用
    static std::string findClang();
    static std::string findRuntimeLib(const std::string& target = "");
    static std::string findSysroot(const std::string& target);
    // Windows CRT 最小集目录（lib/sysroot/win-*/lib）；非 Windows 返回空
    static std::string findWinCrtLibDir(const std::string& target = "");

private:
    // 生成 .ll；成功返回 true，llPath 为产物路径。
    // outExternLibs（可选）：收集所有 extern 函数 @link(...) 声明的外部链接库。
    bool compileToIR(const BuildOptions& opts, std::string& llPath,
                     std::vector<std::string>* outExternLibs = nullptr);

    // 调 clang 把 .ll 链接成可执行文件
    bool linkExecutable(const BuildOptions& opts,
                        const std::string& llPath,
                        const std::string& exePath);
};

} // namespace hao
