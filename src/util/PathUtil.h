// ============================================================
//  HaoLang —— 路径与进程工具
// ------------------------------------------------------------
//  集中驱动层用到的平台相关小工具：可执行文件目录、文件存在性、
//  去扩展名、shell 引号、Windows 路径斜杠转换。
//  这些原本散落在 driver/Driver.cpp 的匿名命名空间里。
// ============================================================
#pragma once

#include <string>

namespace hao {

// 当前可执行文件所在目录（用于定位自带的工具链与运行时库）。
std::string exeDir();

// 文件是否存在且可读。
bool fileExists(const std::string& path);

// 去掉文件名的扩展名（foo.hao -> foo）；无扩展名或扩展名位于目录部分时原样返回。
std::string stripExt(const std::string& path);

// 路径含空格时用双引号包裹，供 std::system() 拼接命令行使用。
std::string quote(const std::string& path);

// 把正斜杠换成反斜杠。cmd.exe 不接受正斜杠路径（会当成选项分隔符），
// 在 Windows 上执行子进程前必须转换。
std::string toWinPath(const std::string& path);

// 返回路径中的目录部分（不含末尾文件名）。无目录分隔符时返回 "."。
std::string dirName(const std::string& path);

// 拼接两段路径，自动处理分隔符。
std::string joinPath(const std::string& a, const std::string& b);

// 把点分 import 路径（如 "util.strings"）转成文件系统相对路径
// （如 "util/strings"，Windows 下仍用正斜杠，由调用方按需转换）。
std::string importToFsPath(const std::string& dotted);

} // namespace hao
