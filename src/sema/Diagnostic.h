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
    std::string file;   // L0a：每条自带路径，print 永不丢文件
    size_t line = 0;
    size_t column = 0;
    std::string message;
};

// 收集编译过程中的全部错误与警告，统一输出
class DiagnosticEngine {
public:
    void setDefaultFile(const std::string& file) { defaultFile_ = file; }
    const std::string& defaultFile() const { return defaultFile_; }

    void error(const std::string& file, size_t line, size_t col,
               const std::string& msg) {
        diags_.push_back({DiagLevel::Error, file, line, col, msg});
        ++errorCount_;
    }

    void warning(const std::string& file, size_t line, size_t col,
                 const std::string& msg) {
        diags_.push_back({DiagLevel::Warning, file, line, col, msg});
    }

    // 兼容：用 defaultFile_（未设则为空串，print 时显示 "?"）
    void error(size_t line, size_t col, const std::string& msg) {
        error(defaultFile_, line, col, msg);
    }

    void warning(size_t line, size_t col, const std::string& msg) {
        warning(defaultFile_, line, col, msg);
    }

    bool hasErrors() const { return errorCount_ > 0; }
    size_t errorCount() const { return errorCount_; }
    const std::vector<Diagnostic>& diags() const { return diags_; }

    // 按 文件:行:列: 级别: 消息 的格式打印（每条用自身 file）
    void print(std::ostream& os = std::cerr) const {
        for (const auto& d : diags_) {
            const std::string& f = d.file.empty() ? defaultFile_ : d.file;
            os << (f.empty() ? "?" : f) << ":" << d.line << ":" << d.column
               << ": " << (d.level == DiagLevel::Error ? "错误: " : "警告: ")
               << d.message << "\n";
        }
        if (errorCount_ > 0) {
            os << "\n共 " << errorCount_ << " 个错误。\n";
        }
    }

    // 兼容旧调用 print(file)：仅当条目 file 空时作回退
    void print(const std::string& fallbackFile, std::ostream& os = std::cerr) const {
        for (const auto& d : diags_) {
            const std::string& f = !d.file.empty() ? d.file
                                 : (!fallbackFile.empty() ? fallbackFile
                                                          : defaultFile_);
            os << (f.empty() ? "?" : f) << ":" << d.line << ":" << d.column
               << ": " << (d.level == DiagLevel::Error ? "错误: " : "警告: ")
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

    // D1：驱动/工具链无源码位错误 → 统一 `文件:行:列: 错误:`（默认 hao:0:0）
    static void toolError(const std::string& msg,
                          const std::string& file = "hao") {
        DiagnosticEngine e;
        e.error(file, 0, 0, msg);
        e.print();
    }

private:
    std::vector<Diagnostic> diags_;
    size_t errorCount_ = 0;
    std::string defaultFile_;
};

} // namespace hao
