// ============================================================
//  HaoLang 编译驱动 —— 工具链与依赖定位
// ------------------------------------------------------------
//  查找自带的 clang、运行时静态库、交叉编译 sysroot。
//  优先级：可执行文件相对路径 > 编译期注入路径 > PATH / 提示信息。
//  相对路径优先是为了保证整个目录拷到别的机器后仍指向自带工具链。
// ============================================================

#include "driver/Driver.h"
#include "HaoVersion.h"
#include "util/PathUtil.h"

#include <string>
#include <vector>

// CMake 注入的编译期路径（缺失时留空，运行时再回退推导）
#ifndef HAO_LLVM_DIR
#define HAO_LLVM_DIR ""
#endif
#ifndef HAO_STDLIB_DIR
#define HAO_STDLIB_DIR ""
#endif
#ifndef HAO_ROOT_DIR
#define HAO_ROOT_DIR ""
#endif

namespace hao {

std::string Driver::findClang() {
    std::vector<std::string> cands;

    // 优先级：exe 相对路径 > 编译期注入路径 > PATH
    std::string dir = exeDir();
    cands.push_back(dir + "/../lib/llvm/bin/clang.exe");
    cands.push_back(dir + "/lib/llvm/bin/clang.exe");
    cands.push_back(dir + "/clang.exe");

    if (std::string(HAO_LLVM_DIR).size())
        cands.push_back(std::string(HAO_LLVM_DIR) + "/bin/clang.exe");

    for (const auto& c : cands)
        if (fileExists(c)) return c;

    return "clang";   // 交给 PATH 解析
}

std::string Driver::findRuntimeLib(const std::string& target) {
    // 交叉编译时需要目标平台的运行时库：libhaort-linux-amd64.a
    std::string libName = target.empty()
        ? "libhaort.a"
        : "libhaort-" + target + ".a";

    std::vector<std::string> cands;
    std::string dir = exeDir();
    cands.push_back(dir + "/../stdlib/" + libName);
    cands.push_back(dir + "/stdlib/" + libName);
    cands.push_back(dir + "/" + libName);

    // 同 stdlibSrcDir：发行包布局下不用编译期绝对路径兜底，避免本机掩盖缺库
    bool portableDist =
        fileExists(dir + "/../lib/llvm/bin/clang.exe") ||
        fileExists(dir + "/lib/llvm/bin/clang.exe");
    if (!portableDist && std::string(HAO_STDLIB_DIR).size())
        cands.push_back(std::string(HAO_STDLIB_DIR) + "/" + libName);

    for (const auto& c : cands)
        if (fileExists(c)) return c;

    return "";
}

// 交叉编译到非 Windows 目标需要系统 libc 头文件与库（crt*.o、libc.a、libgcc.a），
// 这些不属于 LLVM，需单独准备 sysroot，放在 lib/sysroot/<target>。
// Windows 目标不走本函数（头文件不随包）；链接用 CRT 最小集见 findWinCrtLibDir。
std::string Driver::findSysroot(const std::string& target) {
    if (target.empty()) return "";

    std::vector<std::string> cands;
    std::string dir = exeDir();
    cands.push_back(dir + "/../lib/sysroot/" + target);
    cands.push_back(dir + "/lib/sysroot/" + target);

    if (std::string(HAO_ROOT_DIR).size())
        cands.push_back(std::string(HAO_ROOT_DIR) + "/lib/sysroot/" + target);

    for (const auto& c : cands) {
        // musl sysroot 头文件在 include/，glibc 风格在 usr/include/，两者都支持
        if (fileExists(c + "/include/stdio.h") ||
            fileExists(c + "/usr/include/stdio.h"))
            return c;
    }
    return "";
}

// Windows MSVC 链接所需 CRT 最小集目录：lib/sysroot/win-amd64/lib 等。
// 内含 libcmt / libvcruntime / libucrt / kernel32 / oldnames（见 fetch_winlibs.ps1）。
// 有 VS 的机器上 clang 仍会自动找到系统路径；本目录保证无 VS 也能链。
std::string Driver::findWinCrtLibDir(const std::string& target) {
    std::string t = target;
    if (t.empty()) {
        Platform host = hostPlatform();
        if (!host.os || std::string(host.os) != "win") return "";
        t = std::string("win-") + host.arch;
    }
    if (t.rfind("win-", 0) != 0) return "";

    std::vector<std::string> cands;
    std::string dir = exeDir();
    cands.push_back(dir + "/../lib/sysroot/" + t + "/lib");
    cands.push_back(dir + "/lib/sysroot/" + t + "/lib");

    // 发行包布局下不用开发树绝对路径兜底（同 stdlibSrcDir）
    bool portableDist =
        fileExists(dir + "/../lib/llvm/bin/clang.exe") ||
        fileExists(dir + "/lib/llvm/bin/clang.exe");
    if (!portableDist && std::string(HAO_ROOT_DIR).size())
        cands.push_back(std::string(HAO_ROOT_DIR) + "/lib/sysroot/" + t + "/lib");

    for (const auto& c : cands)
        if (fileExists(c + "/libcmt.lib") && fileExists(c + "/kernel32.lib"))
            return c;
    return "";
}

} // namespace hao
