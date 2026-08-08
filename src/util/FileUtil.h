// ============================================================
//  HaoLang —— 文件读写工具
// ------------------------------------------------------------
//  集中处理编译器与驱动中重复出现的文件读取逻辑：一次性读入整个文件、
//  自动跳过 UTF-8 BOM（Windows 记事本等会写入）、二进制读入避免换行符
//  被转换。供 main.cpp 与 driver/Driver.cpp 共用，消除重复实现。
// ============================================================
#pragma once

#include <string>
#include <vector>

namespace hao {

// 读取整个文件到 out（二进制模式，保留原始字节）。
// 若文件存在 UTF-8 BOM（EF BB BF）则自动剔除。
// 成功返回 true；无法打开返回 false（out 内容未定义）。
bool readFile(const std::string& path, std::string& out);

// 以二进制写入整个文件（覆盖）。成功返回 true。
bool writeFile(const std::string& path, const std::string& data);

// 列出目录下所有 .hao 文件（不含子目录），按文件名排序。
// 目录不存在或不可读时返回 false，out 不被修改。
bool listHaoFiles(const std::string& dir, std::vector<std::string>& out);

// 递归列出目录下所有 .hao 文件，按完整路径排序。
bool listHaoFilesRecursive(const std::string& dir, std::vector<std::string>& out);

// 文件名是否以 `_test.hao` 结尾（Go 式测试文件；普通 build 排除）
bool isHaoTestFile(const std::string& path);

// 路径是否为目录。
bool isDirectory(const std::string& path);

} // namespace hao
