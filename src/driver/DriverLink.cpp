// ============================================================
//  HaoLang 编译驱动 —— .ll -> .exe
// ------------------------------------------------------------
//  定位 clang 与运行时库，拼装命令行调用 clang 把 LLVM IR 链接为
//  原生可执行文件；处理交叉编译的 --target / --sysroot / -static。
//  IR 生成在 DriverCompile.cpp，入口编排在 Driver.cpp。
// ============================================================

#include "driver/Driver.h"
#include "HaoVersion.h"
#include "util/PathUtil.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace hao {

bool Driver::linkExecutable(const BuildOptions& opts,
                            const std::string& llPath,
                            const std::string& exePath) {
    // 先校验目标平台，避免因平台名拼错而报出令人困惑的"运行时库缺失"
    Platform p = opts.target.empty() ? hostPlatform() : platformFromName(opts.target);
    if (!p.os) {
        std::cerr << "错误: 未知的目标平台 '" << opts.target << "'\n"
                  << "      支持: win-amd64, win-arm64, linux-amd64, "
                     "linux-arm64, darwin-amd64, darwin-arm64\n";
        return false;
    }

    std::string clang = findClang();
    std::string rt = findRuntimeLib(opts.target);

    if (rt.empty()) {
        std::cerr << "错误: 未找到 HaoLang 运行时库";
        if (!opts.target.empty())
            std::cerr << " libhaort-" << opts.target << ".a\n"
                      << "      交叉编译需要目标平台的运行时库，请先执行:\n"
                      << "        script\\build_runtime.ps1 -Target " << opts.target << "\n";
        else
            std::cerr << " libhaort.a\n"
                      << "      请先执行: script\\build_runtime.ps1\n";
        return false;
    }

    // -O2：用户程序默认优化。GC 的保守栈扫描依赖内联/优化后合理的栈布局；
    // -O0 下生成的大栈帧配合保守扫描可能越过 guard page。-O2 也显著提升运行速度。
    std::string cmd = quote(clang) + " " + quote(llPath) + " " + quote(rt)
                    + " -o " + quote(exePath)
                    + " -O2 -Wno-override-module";

    // ---- 外部 C 库链接（v0.10.0）：-L<dir> / --link <file> / -l<name> ----
    // 追加到命令末尾（-o 之后），clang 会按需编译 .c 源码、链接 .lib/.a/.o。

    // v0.11.0 默认库搜索路径：当前目录 / lib / 源码所在目录 / 源码目录 lib / stdlib/lib。
    // 让 @link("foo") / -lfoo 自动找到放在这些位置的库，无需手动 -L。
    // 目录不存在时用 is_directory 过滤，避免在命令里堆积无意义的 -L。
    std::vector<std::string> defLinkDirs = { ".", "./lib" };
    defLinkDirs.push_back(dirName(opts.sourceFile));
    defLinkDirs.push_back(joinPath(dirName(opts.sourceFile), "lib"));
    defLinkDirs.push_back(exeDir() + "/../stdlib/lib");
    // 去重（cwd 与源码目录相同时会有重复路径）
    std::vector<std::string> uniq;
    for (auto& d : defLinkDirs)
        if (std::find(uniq.begin(), uniq.end(), d) == uniq.end())
            uniq.push_back(d);
    for (auto& d : uniq)
        if (std::filesystem::is_directory(d))
            cmd += " -L" + quote(d);

    // Windows CRT 最小集：无 VS/SDK 时靠自带 lib/sysroot/win-*/lib 解析
    // libcmt → kernel32/libvcruntime/libucrt。有 VS 时与系统路径并存无害。
    {
        std::string winCrt = findWinCrtLibDir(opts.target);
        if (!winCrt.empty())
            cmd += " -L" + quote(winCrt);
    }

    for (auto& d : opts.linkDirs)  cmd += " -L" + quote(d);
    for (auto& f : opts.linkFiles) cmd += " " + quote(f);
    for (auto& l : opts.linkLibs)  cmd += " -l" + l;

    // v0.11.0 环境变量（对标 CGO_LDFLAGS / CGO_CFLAGS）：追加到链接命令。
    // HAO_LDFLAGS 如 "-lws2_32 -L D:/x"；HAO_CFLAGS 用于 --link .c 时的 -I 等。
    if (const char* ld = std::getenv("HAO_LDFLAGS"))
        if (*ld) cmd += " " + std::string(ld);
    if (const char* cf = std::getenv("HAO_CFLAGS"))
        if (*cf) cmd += " " + std::string(cf);

    // ---- 交叉编译 ----
    if (!opts.target.empty()) {
        cmd += " --target=" + std::string(p.triple);

        // 非 Windows 目标需要 sysroot 提供 glibc 与 crt 启动文件，
        // 以及 lld 作为链接器（宿主的 lld-link 只处理 COFF）。
        if (std::string(p.os) != "win") {
            std::string sysroot = findSysroot(opts.target);
            if (sysroot.empty()) {
                std::cerr << "错误: 交叉编译到 " << opts.target
                          << " 需要 sysroot（目标系统的 libc 头文件与库）\n"
                          << "      这些文件不属于 LLVM，需单独准备。\n"
                          << "      请执行: script\\fetch_sysroot.ps1 -Target "
                          << opts.target << "\n";
                return false;
            }
            cmd += " --sysroot=" + quote(sysroot);
            cmd += " -fuse-ld=lld";
            // 静态链接，保证产物在目标机上零依赖
            cmd += " -static";
        }
    }

    if (opts.verbose) std::cout << "[hao] " << cmd << "\n";

    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::cerr << "错误: 链接失败（clang 返回 " << rc << "）\n";
        return false;
    }
    return true;
}

} // namespace hao
