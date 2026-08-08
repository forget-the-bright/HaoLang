// ============================================================
//  hao fmt —— 源码格式化（空白 + 4 空格缩进；非完整 pretty-print）
// ============================================================
#pragma once

#include <string>
#include <vector>

namespace hao {

struct FmtOptions {
    bool write = false;   // -w：写回文件
    bool check = false;   // --check：有差异则失败（不写）
    bool verbose = false;
};

// 规范化：去行尾空白、CRLF→LF、文件末恰好一个换行、按花括号深度 4 空格缩进。
// 不断行重排、不改运算符旁空格。
std::string formatHaoSource(const std::string& src);

// 语法是否可解析（fmt 前闸门；失败不改正文）。
bool canParseHaoFile(const std::string& path, std::string& errorOut);

// 处理路径列表（文件或目录；目录递归扫 .hao）。
// 返回退出码：0 成功；1 有错误/check 未通过。
int runFmt(const std::vector<std::string>& paths, const FmtOptions& opts);

} // namespace hao
