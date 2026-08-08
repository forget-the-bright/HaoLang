// ============================================================
//  HaoLang 版本与平台信息
// ------------------------------------------------------------
//  版本号来自项目根目录的 VERSION 文件，由 CMake 在配置阶段
//  读取并通过 HAO_VERSION 宏注入，避免版本号在多处重复维护。
// ============================================================
#pragma once

#include <string>

#ifndef HAO_VERSION
#define HAO_VERSION "0.0.0-dev"
#endif

#ifndef HAO_ANTLR_VERSION
#define HAO_ANTLR_VERSION "unknown"
#endif

#ifndef HAO_LLVM_VERSION
#define HAO_LLVM_VERSION "unknown"
#endif

namespace hao {

// ------------------------------------------------------------
//  目标平台标识
// ------------------------------------------------------------
//  分发包命名规则：{os}-{arch}-haolang-{版本}
//  例如：win-amd64-haolang-0.3.0 / linux-amd64-haolang-0.3.0
// ------------------------------------------------------------

struct Platform {
    const char* os;      // win / linux / darwin
    const char* arch;    // amd64 / arm64
    const char* triple;  // LLVM target triple
    const char* exeExt;  // 可执行文件后缀
};

// 当前编译器所在的宿主平台
inline Platform hostPlatform() {
#if defined(_WIN32)
  #if defined(_M_ARM64) || defined(__aarch64__)
    return {"win", "arm64", "aarch64-pc-windows-msvc", ".exe"};
  #else
    return {"win", "amd64", "x86_64-pc-windows-msvc", ".exe"};
  #endif
#elif defined(__linux__)
  #if defined(__aarch64__)
    return {"linux", "arm64", "aarch64-linux-musl", ""};
  #else
    return {"linux", "amd64", "x86_64-linux-musl", ""};
  #endif
#elif defined(__APPLE__)
  #if defined(__aarch64__)
    return {"darwin", "arm64", "aarch64-apple-darwin", ""};
  #else
    return {"darwin", "amd64", "x86_64-apple-darwin", ""};
  #endif
#else
    return {"unknown", "unknown", "", ""};
#endif
}

// 由 "win-amd64" 这类名字解析出平台信息；未知则 os 为 nullptr
//
// Linux 目标使用 musl 而非 glibc：musl 专为静态链接设计，
// 产出的可执行文件真正零依赖，符合绿色分发目标；
// glibc 静态链接则存在 NSS / dlopen 等已知问题。
inline Platform platformFromName(const std::string& name) {
    if (name == "win-amd64")
        return {"win", "amd64", "x86_64-pc-windows-msvc", ".exe"};
    if (name == "win-arm64")
        return {"win", "arm64", "aarch64-pc-windows-msvc", ".exe"};
    if (name == "linux-amd64")
        return {"linux", "amd64", "x86_64-linux-musl", ""};
    if (name == "linux-arm64")
        return {"linux", "arm64", "aarch64-linux-musl", ""};
    if (name == "darwin-amd64")
        return {"darwin", "amd64", "x86_64-apple-darwin", ""};
    if (name == "darwin-arm64")
        return {"darwin", "arm64", "aarch64-apple-darwin", ""};
    return {nullptr, nullptr, nullptr, nullptr};
}

inline std::string platformName(const Platform& p) {
    return std::string(p.os) + "-" + p.arch;
}

// 分发包目录名：win-amd64-haolang-0.3.0
inline std::string distName(const Platform& p) {
    return platformName(p) + "-haolang-" + HAO_VERSION;
}

inline const char* version()      { return HAO_VERSION; }
inline const char* antlrVersion() { return HAO_ANTLR_VERSION; }
inline const char* llvmVersion()  { return HAO_LLVM_VERSION; }

} // namespace hao
