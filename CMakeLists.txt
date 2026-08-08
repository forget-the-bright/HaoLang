# ============================================================
#  HaoLang 编译器构建配置
# ------------------------------------------------------------
#  工具链说明（关键，勿随意改动）：
#    主编译器 : lib/llvm/bin/clang++.exe (LLVM 22.1.8)
#    目标三元 : x86_64-pc-windows-msvc（clang 默认）
#    标准库   : MSVC 14.39 STL —— 必须用它，不能用 MinGW 的 libstdc++
#    链接器   : lld-link（clang 自动选用）
#
#  为何不用 MinGW：
#    MinGW libstdc++（win32 线程模型）无 std::mutex / std::thread 等，
#    ANTLR4 runtime 编不过。C++ 标准库用本机 VS 的 MSVC STL
#    （clang 自动探测 VC\Tools\MSVC\…，不是 LLVM 自带）；
#    链接用户程序 CRT 用 lib/sysroot/win-*/lib（fetch_winlibs）。
#    开发机不要依赖 MinGW。
#
#  配置命令示例：
#    cmake -S . -B build -G Ninja `
#      -DCMAKE_BUILD_TYPE=Release `
#      -DCMAKE_C_COMPILER=D:/buildLang/lib/llvm/bin/clang.exe `
#      -DCMAKE_CXX_COMPILER=D:/buildLang/lib/llvm/bin/clang++.exe `
#      -DCMAKE_RC_COMPILER=D:/buildLang/lib/llvm/bin/llvm-rc.exe
# ============================================================

cmake_minimum_required(VERSION 3.20)
project(HaoLang CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ------------------------------------------------------------
#  版本号
# ------------------------------------------------------------
#  VERSION 文件是版本号的唯一来源，避免在多处重复维护。
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/VERSION HAO_VERSION_RAW)
string(STRIP "${HAO_VERSION_RAW}" HAO_VERSION)
set(HAO_ANTLR_VERSION "4.13.2")
set(HAO_LLVM_VERSION  "22.1.8")
message(STATUS "HaoLang 版本: ${HAO_VERSION}")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ------------------------------------------------------------
#  运行时库：静态 CRT
# ------------------------------------------------------------
#  必须与 ANTLR4 静态库保持一致（该库编译时为 MT_StaticRelease）。
#  不一致会触发 lld-link 的 /failifmismatch RuntimeLibrary 错误。
#  同时静态 CRT 让 hao.exe 不依赖 msvcrt.dll，符合绿色分发目标。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ------------------------------------------------------------
#  Windows 系统库：过滤 CMake 默认的“全家桶”
# ------------------------------------------------------------
#  未覆盖时 CMake 会给 clang/MSVC 目标自动追加：
#    -lkernel32 -luser32 -lgdi32 -lwinspool -lshell32 -lole32
#    -loleaut32 -luuid -lcomdlg32 -ladvapi32 -loldnames
#  hao 是纯 CLI，导入表实际只要 KERNEL32.dll；其余库无符号引用，
#  只增加对 SDK/导入库路径的依赖。显式收成最小集。
#  用户程序链接不走本变量（见 DriverLink），仍由 libcmt DEFAULTLIB
#  + lib/sysroot/win-*/lib CRT 最小集解决。
if(WIN32)
    set(CMAKE_CXX_STANDARD_LIBRARIES "-lkernel32 -loldnames" CACHE STRING
        "Windows 系统库最小集（过滤 CMake 默认全家桶）" FORCE)
    set(CMAKE_C_STANDARD_LIBRARIES   "-lkernel32 -loldnames" CACHE STRING
        "Windows 系统库最小集（过滤 CMake 默认全家桶）" FORCE)
endif()

# ------------------------------------------------------------
#  依赖路径
# ------------------------------------------------------------
set(HAO_ROOT        ${CMAKE_CURRENT_SOURCE_DIR})
set(ANTLR_INSTALL   ${HAO_ROOT}/lib/antlr4/install)
set(ANTLR_INCLUDE   ${ANTLR_INSTALL}/include/antlr4-runtime)
set(ANTLR_LIB       ${ANTLR_INSTALL}/lib/libantlr4-runtime.a)

if(NOT EXISTS ${ANTLR_LIB})
    message(FATAL_ERROR
        "未找到 ANTLR4 静态库: ${ANTLR_LIB}\n"
        "请先编译 ANTLR4 C++ runtime（见 README.md Stage 2）")
endif()

# ------------------------------------------------------------
#  源文件
# ------------------------------------------------------------
# ANTLR 生成的分析器
set(GENERATED_DIR ${HAO_ROOT}/src/ast/generated)
set(GENERATED_SRC
    ${GENERATED_DIR}/HaoLangLexer.cpp
    ${GENERATED_DIR}/HaoLangParser.cpp
    ${GENERATED_DIR}/HaoLangParserBaseVisitor.cpp
    ${GENERATED_DIR}/HaoLangParserVisitor.cpp
)

# 手写源码：各阶段目录下的所有 .cpp
file(GLOB_RECURSE HAND_SRC
    ${HAO_ROOT}/src/sema/*.cpp
    ${HAO_ROOT}/src/irgen/*.cpp
    ${HAO_ROOT}/src/visitor/*.cpp
    ${HAO_ROOT}/src/driver/*.cpp
    ${HAO_ROOT}/src/util/*.cpp
    ${HAO_ROOT}/src/mod/*.cpp
    ${HAO_ROOT}/src/tool/*.cpp
)

add_executable(hao
    ${HAO_ROOT}/src/main.cpp
    ${GENERATED_SRC}
    ${HAND_SRC}
)

# ------------------------------------------------------------
#  头文件搜索路径
# ------------------------------------------------------------
target_include_directories(hao PRIVATE
    ${ANTLR_INCLUDE}
    ${GENERATED_DIR}
    ${HAO_ROOT}/src
)

# ------------------------------------------------------------
#  编译定义
# ------------------------------------------------------------
target_compile_definitions(hao PRIVATE
    # 必须：告知 ANTLR 头文件按静态库链接，否则符号会被标记为
    # __declspec(dllimport)，导致 lld-link 报 "not an import library"
    ANTLR4CPP_STATIC

    # 工具链路径。Driver 还会按 exe 相对位置回退查找，
    # 以保证整个目录被拷走后依然可用。
    HAO_LLVM_DIR="${HAO_ROOT}/lib/llvm"
    HAO_STDLIB_DIR="${HAO_ROOT}/stdlib"

    # 版本信息（来自 VERSION 文件）
    HAO_VERSION="${HAO_VERSION}"
    HAO_ANTLR_VERSION="${HAO_ANTLR_VERSION}"
    HAO_LLVM_VERSION="${HAO_LLVM_VERSION}"

    # 项目根目录，用于定位 target/sysroot
    HAO_ROOT_DIR="${HAO_ROOT}"
)

# ------------------------------------------------------------
#  编译选项
# ------------------------------------------------------------
target_compile_options(hao PRIVATE
    # ANTLR 生成代码与其头文件会触发大量无害警告，逐项静音
    -Wno-language-extension-token   # antlr4-common.h 中的 typedef __int64 ssize_t
    -Wno-unused-parameter
    -Wno-overloaded-virtual
    -Wno-attributes
)

# ------------------------------------------------------------
#  链接
# ------------------------------------------------------------
target_link_libraries(hao PRIVATE ${ANTLR_LIB})
if (WIN32)
    target_link_libraries(hao PRIVATE winhttp)
endif()

# ------------------------------------------------------------
#  输出到 output/
# ------------------------------------------------------------
set_target_properties(hao PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${HAO_ROOT}/output
)

message(STATUS "----------------------------------------")
message(STATUS "HaoLang 编译器")
message(STATUS "  编译器   : ${CMAKE_CXX_COMPILER}")
message(STATUS "  构建类型 : ${CMAKE_BUILD_TYPE}")
message(STATUS "  ANTLR    : ${ANTLR_LIB}")
message(STATUS "  输出     : ${HAO_ROOT}/output/hao.exe")
message(STATUS "----------------------------------------")
