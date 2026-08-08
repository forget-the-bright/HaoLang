// ============================================================
//  HaoLang 诊断信息收集
// ============================================================
#pragma once

#include <iostream>
#include <string>
#include <vector>

namespace hao {

enum class DiagLevel { Error, Warning };

struct Diagnostic {
    DiagLevel level = DiagLevel::Error;
    size_t line = 0;
    size_t column = 0;
    std::string message;
};

// 收集编译过程中的全部错误与警告，统一输出
class DiagnosticEngine {
public:
    void error(size_t line, size_t col, const std::string& msg) {
        diags_.push_back({DiagLevel::Error, line, col, msg});
        ++errorCount_;
    }

    void warning(size_t line, size_t col, const std::string& msg) {
        diags_.push_back({DiagLevel::Warning, line, col, msg});
    }

    bool hasErrors() const { return errorCount_ > 0; }
    size_t errorCount() const { return errorCount_; }
    const std::vector<Diagnostic>& diags() const { return diags_; }

    // 按 文件:行:列: 级别: 消息 的格式打印
    void print(const std::string& file, std::ostream& os = std::cerr) const {
        for (const auto& d : diags_) {
            os << file << ":" << d.line << ":" << d.column << ": "
               << (d.level == DiagLevel::Error ? "错误: " : "警告: ")
               << d.message << "\n";
        }
        if (errorCount_ > 0) {
            os << "\n共 " << errorCount_ << " 个错误。\n";
        }
    }

    void clear() {
        diags_.clear();
        errorCount_ = 0;
    }

private:
    std::vector<Diagnostic> diags_;
    size_t errorCount_ = 0;
};

} // namespace hao
