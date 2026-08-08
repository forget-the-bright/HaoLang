// ============================================================
//  HaoLang —— 字符串工具
// ------------------------------------------------------------
//  集中编译器多处重复的字符串处理：去引号并处理转义、去数字下划线。
//  原本散落在 irgen/IRGenClass.cpp 与 IRGenLiteral.cpp 的匿名命名空间里
//  （unquot==unquoteStr、stripUnderscore==noUnderscore，逐字符相同）。
// ============================================================
#pragma once

#include <string>

namespace hao {

namespace StringUtil {

// 去掉整型/浮点字面量里的下划线分隔符（"1_000" -> "1000"）。
std::string stripUnderscores(const std::string& s);

// 去掉字符串字面量的引号并处理转义（"a\nb" -> "a<LF>b"）。
// 支持 \n \t \r \\ \" \' \$ \uXXXX；其余转义原样保留反斜杠后的字符。
std::string unescapeStringLiteral(const std::string& raw);

// 逐字字符串：@"..." → 内容；"" → "；不处理反斜杠转义。
std::string unescapeVerbatimString(const std::string& raw);

// 字符字面量 '…' → Unicode 码点。
uint32_t decodeCharLiteral(const std::string& raw);

} // namespace StringUtil

} // namespace hao