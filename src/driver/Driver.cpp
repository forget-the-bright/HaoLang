// ============================================================
//  HaoLang 编译驱动实现
// ------------------------------------------------------------
//  按职责拆分到多个文件：
//    DriverPaths.cpp    工具链/运行时/sysroot 定位
//    DriverCompile.cpp  .hao -> .ll
//    DriverLink.cpp     .ll -> .exe
//    Driver.cpp         build / run 入口编排
//  公共路径/文件工具在 src/util/。
// ============================================================

#include "driver/Driver.h"

#include "HaoVersion.h"
#include "mod/HaoProject.h"
#include "sema/Diagnostic.h"
#include "util/FileUtil.h"
#include "util/PathUtil.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace hao {

namespace {
// 由输入（文件或目录）推导可执行文件基础名（不含扩展名）
std::string outputBase(const BuildOptions& opts) {
    if (!opts.outputFile.empty()) return opts.outputFile;
    std::string in = opts.sourceFile;
    if (isDirectory(in)) {
        while (in.size() > 1 && (in.back() == '/' || in.back() == '\\'))
            in.pop_back();
        size_t slash = in.find_last_of("/\\");
        return (slash == std::string::npos) ? in : in.substr(slash + 1);
    }
    return stripExt(in);
}

// @link(...) 声明的依赖是否应视为"文件路径"而非"库名"：
// 含路径分隔符（/、\）或以常见库/源码扩展名结尾 → 文件路径；否则库名（-l）。
bool isLibraryPath(const std::string& s) {
    if (s.find('/') != std::string::npos || s.find('\\') != std::string::npos)
        return true;
    static const char* exts[] = {".lib", ".a", ".o", ".c", ".obj", ".cpp", ".dll"};
    for (auto* e : exts)
        if (s.size() > std::strlen(e) &&
            s.compare(s.size() - std::strlen(e), std::strlen(e), e) == 0)
            return true;
    return false;
}

// 把 extern @link(...) 声明的依赖并入链接参数：
//   文件路径 → 校验存在（找不到报错）后进 linkFiles；
//   库名     → 去重后进 linkLibs（交给 clang 按默认搜索路径 -l）。
// 返回 false 表示文件路径不存在（已报错）。
bool mergeExternLibs(BuildOptions& linkOpts,
                     const std::vector<std::string>& externLibs) {
    for (auto& lb : externLibs) {
        if (isLibraryPath(lb)) {
            if (!fileExists(lb)) {
                DiagnosticEngine::toolError(
                    "未能找到 @link 声明的文件 '" + lb + "'");
                return false;
            }
            if (std::find(linkOpts.linkFiles.begin(), linkOpts.linkFiles.end(), lb)
                == linkOpts.linkFiles.end())
                linkOpts.linkFiles.push_back(lb);
        } else {
            if (std::find(linkOpts.linkLibs.begin(), linkOpts.linkLibs.end(), lb)
                == linkOpts.linkLibs.end())
                linkOpts.linkLibs.push_back(lb);
        }
    }
    return true;
}
}

// ============================================================
//  hao build
// ============================================================

int Driver::build(const BuildOptions& opts) {
    BuildOptions o = opts;
    std::string projErr;
    if (!applyHaoProjectToOptions(o, projErr)) {
        DiagnosticEngine::toolError(projErr);
        return 1;
    }

    std::string llPath;
    std::vector<std::string> externLibs;
    if (!compileToIR(o, llPath, &externLibs)) return 1;

    if (o.emitIROnly) {
        std::cout << llPath << "\n";
        return 0;
    }

    auto p = o.target.empty() ? hostPlatform() : platformFromName(o.target);
    std::string ext = (p.exeExt ? p.exeExt : "");

    // 显式 -o 原样使用；否则按输入名推导并追加平台扩展名
    std::string exePath = o.outputFile.empty()
        ? outputBase(o) + ext
        : o.outputFile;

    // 把 extern @link(...) 声明的依赖并入链接参数（文件/库名分流，去重）
    BuildOptions linkOpts = o;
    if (!mergeExternLibs(linkOpts, externLibs)) return 1;

    if (!linkExecutable(linkOpts, llPath, exePath)) return 1;

    if (!o.keepIR) std::remove(llPath.c_str());

    if (!o.quiet) {
        std::cout << "编译成功: " << exePath;
        if (!o.target.empty()) std::cout << "  (" << o.target << ")";
        std::cout << "\n";
    }
    return 0;
}

// ============================================================
//  hao run
// ============================================================

int Driver::run(const BuildOptions& opts) {
    BuildOptions o = opts;
    std::string projErr;
    if (!applyHaoProjectToOptions(o, projErr)) {
        DiagnosticEngine::toolError(projErr);
        return 1;
    }

    // 为其他平台编译的产物无法在本机执行
    if (!o.target.empty() && o.target != platformName(hostPlatform())) {
        DiagnosticEngine::toolError(
            "无法运行为 " + o.target + " 编译的程序（当前平台 " +
            platformName(hostPlatform()) + "）；请改用 hao build");
        return 1;
    }

    std::string llPath;
    std::vector<std::string> externLibs;
    if (!compileToIR(o, llPath, &externLibs)) return 1;

    // 与源文件同目录生成临时可执行文件（避免跨盘），运行后删除，
    // 语义与 go run 一致：不留下构建产物。
    std::string exePath = stripExt(o.sourceFile) + ".hao-run"
                        + hostPlatform().exeExt;

    BuildOptions linkOpts = o;
    if (!mergeExternLibs(linkOpts, externLibs)) return 1;

    if (!linkExecutable(linkOpts, llPath, exePath)) return 1;
    if (!o.keepIR) std::remove(llPath.c_str());

    if (o.verbose) std::cout << "[hao] 运行 " << exePath << "\n";
    std::cout.flush();

    // 相对路径需加 .\ 前缀，cmd.exe 才会在当前目录查找
    std::string winExe = toWinPath(exePath);
    if (winExe.find('\\') == std::string::npos)
        winExe = ".\\" + winExe;

    // 追加 `--` 之后的程序参数（传给用户 main(args)）
    std::string cmdline = quote(winExe);
    for (const auto& pa : o.programArgs)
        cmdline += " " + quote(pa);

    int rc = std::system(cmdline.c_str());

    std::remove(exePath.c_str());
    return rc;
}

} // namespace hao
