// ============================================================
//  最小 SHA-256（包校验用，无外部依赖）
// ============================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hao {

// 计算数据的 SHA-256，返回 64 位小写十六进制。
std::string sha256Hex(const uint8_t* data, size_t len);
std::string sha256Hex(const std::string& data);

// 目录树：按相对路径排序后拼接「路径\\0长度\\0内容」再哈希（稳定可复现）。
std::string sha256DirTree(const std::string& dir, std::string& errorOut);

} // namespace hao
