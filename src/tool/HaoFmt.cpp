// ============================================================
//  hao fmt 实现
// ============================================================

#include "tool/HaoFmt.h"

#include "HaoLangLexer.h"
#include "HaoLangParser.h"
#include "antlr4-runtime.h"
#include "util/FileUtil.h"
#include "util/PathUtil.h"

#include <algorithm>
#include <iostream>
#include <vector>

namespace hao {

namespace {

constexpr int kIndentSpaces = 4;

class CollectErrors : public antlr4::BaseErrorListener {
public:
    std::vector<std::string> msgs;
    void syntaxError(antlr4::Recognizer*, antlr4::Token*,
                     size_t line, size_t col, const std::string& msg,
                     std::exception_ptr) override {
        msgs.push_back(std::to_string(line) + ":" + std::to_string(col) + ": " + msg);
    }
};

void collectPaths(const std::string& in, std::vector<std::string>& out) {
    if (isDirectory(in)) {
        std::vector<std::string> files;
        if (listHaoFilesRecursive(in, files)) {
            for (auto& f : files) out.push_back(f);
        }
        return;
    }
    out.push_back(in);
}

// 去 BOM、行尾空白、CRLF→LF、末恰好一个换行（保留行首空白，供后续缩进覆盖）。
std::string normalizeNewlinesAndTrimRight(const std::string& src) {
    std::string body = src;
    if (body.size() >= 3 &&
        static_cast<unsigned char>(body[0]) == 0xEF &&
        static_cast<unsigned char>(body[1]) == 0xBB &&
        static_cast<unsigned char>(body[2]) == 0xBF) {
        body.erase(0, 3);
    }

    std::string out;
    out.reserve(body.size() + 1);
    size_t i = 0;
    while (i < body.size()) {
        size_t lineStart = i;
        while (i < body.size() && body[i] != '\n' && body[i] != '\r') ++i;
        size_t lineEnd = i;
        while (lineEnd > lineStart &&
               (body[lineEnd - 1] == ' ' || body[lineEnd - 1] == '\t'))
            --lineEnd;
        out.append(body, lineStart, lineEnd - lineStart);
        out.push_back('\n');
        if (i < body.size() && body[i] == '\r') ++i;
        if (i < body.size() && body[i] == '\n') ++i;
    }
    while (out.size() >= 2 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n')
        out.pop_back();
    if (out.empty() || out.back() != '\n') out.push_back('\n');
    return out;
}

std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> lines;
    size_t i = 0;
    while (i < s.size()) {
        size_t start = i;
        while (i < s.size() && s[i] != '\n') ++i;
        lines.emplace_back(s, start, i - start);
        if (i < s.size() && s[i] == '\n') ++i;
    }
    // 规范化后通常以 \n 结尾 → 最后多一个空行分片；去掉
    if (!lines.empty() && lines.back().empty() && s.size() >= 1 && s.back() == '\n')
        lines.pop_back();
    return lines;
}

std::string stripLeadingWs(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    return line.substr(i);
}

bool isBlankLine(const std::string& line) {
    for (char c : line)
        if (c != ' ' && c != '\t') return false;
    return true;
}

// 按默认通道花括号深度重写行首缩进（字符串/注释内括号不计入）。
std::string applyBraceIndent(const std::string& normalized) {
    auto lines = splitLines(normalized);
    const int n = static_cast<int>(lines.size());
    if (n == 0) return "\n";

    antlr4::ANTLRInputStream input(normalized);
    HaoLangLexer lexer(&input);
    lexer.removeErrorListeners();
    auto all = lexer.getAllTokens();

    std::vector<int> startDepth(n, 0);
    std::vector<bool> startsWithClose(n, false);
    int depth = 0;
    size_t ti = 0;
    // 跳过 EOF
    while (ti < all.size() && all[ti]->getType() == antlr4::Token::EOF) ++ti;

    for (int li = 0; li < n; ++li) {
        const int lineNo = li + 1;
        startDepth[li] = depth;
        bool firstDef = true;
        while (ti < all.size()) {
            auto* t = all[ti].get();
            if (t->getType() == antlr4::Token::EOF) break;
            if (static_cast<int>(t->getLine()) > lineNo) break;
            if (static_cast<int>(t->getLine()) < lineNo) {
                ++ti;
                continue;
            }
            // 本行
            if (t->getChannel() == antlr4::Token::DEFAULT_CHANNEL) {
                if (firstDef) {
                    startsWithClose[li] = (t->getType() == HaoLangLexer::RBRACE);
                    firstDef = false;
                }
                if (t->getType() == HaoLangLexer::LBRACE ||
                    t->getType() == HaoLangLexer::TEMPLATE_INTERP_START) {
                    ++depth;
                } else if (t->getType() == HaoLangLexer::RBRACE) {
                    if (depth > 0) --depth;
                }
            }
            ++ti;
        }
    }

    std::string out;
    out.reserve(normalized.size() + static_cast<size_t>(n) * 4);
    for (int li = 0; li < n; ++li) {
        if (isBlankLine(lines[li])) {
            out.push_back('\n');
            continue;
        }
        int ind = startDepth[li];
        if (startsWithClose[li]) ind = std::max(0, ind - 1);
        out.append(static_cast<size_t>(ind * kIndentSpaces), ' ');
        out += stripLeadingWs(lines[li]);
        out.push_back('\n');
    }
    while (out.size() >= 2 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n')
        out.pop_back();
    if (out.empty() || out.back() != '\n') out.push_back('\n');
    return out;
}

} // namespace

std::string formatHaoSource(const std::string& src) {
    std::string normalized = normalizeNewlinesAndTrimRight(src);
    return applyBraceIndent(normalized);
}

bool canParseHaoFile(const std::string& path, std::string& errorOut) {
    errorOut.clear();
    std::string src;
    if (!readFile(path, src)) {
        errorOut = "无法读取 " + path;
        return false;
    }
    antlr4::ANTLRInputStream input(src);
    HaoLangLexer lexer(&input);
    CollectErrors err;
    lexer.removeErrorListeners();
    lexer.addErrorListener(&err);
    antlr4::CommonTokenStream tokens(&lexer);
    HaoLangParser parser(&tokens);
    parser.removeErrorListeners();
    parser.addErrorListener(&err);
    parser.compilationUnit();
    if (!err.msgs.empty()) {
        errorOut = path + ": " + err.msgs[0];
        return false;
    }
    return true;
}

int runFmt(const std::vector<std::string>& paths, const FmtOptions& opts) {
    if (paths.empty()) {
        std::cerr << "错误: hao fmt 需要文件或目录\n";
        return 1;
    }
    std::vector<std::string> files;
    for (const auto& p : paths) collectPaths(p, files);
    if (files.empty()) {
        std::cerr << "错误: 未找到 .hao 文件\n";
        return 1;
    }

    int fail = 0;
    int changed = 0;
    for (const auto& f : files) {
        std::string perr;
        if (!canParseHaoFile(f, perr)) {
            std::cerr << "错误: " << perr << "\n";
            ++fail;
            continue;
        }
        std::string src;
        if (!readFile(f, src)) {
            std::cerr << "错误: 无法读取 " << f << "\n";
            ++fail;
            continue;
        }
        std::string formatted = formatHaoSource(src);
        bool diff = formatted != src;
        if (diff) ++changed;

        if (opts.check) {
            if (diff) {
                std::cout << "需要格式化: " << f << "\n";
                ++fail;
            } else if (opts.verbose) {
                std::cout << "ok  " << f << "\n";
            }
            continue;
        }

        if (opts.write) {
            if (diff) {
                if (!writeFile(f, formatted)) {
                    std::cerr << "错误: 无法写入 " << f << "\n";
                    ++fail;
                    continue;
                }
                std::cout << "已格式化 " << f << "\n";
            } else if (opts.verbose) {
                std::cout << "unchanged " << f << "\n";
            }
        } else {
            if (files.size() > 1)
                std::cout << "// " << f << "\n";
            std::cout << formatted;
            if (!formatted.empty() && formatted.back() != '\n') std::cout << "\n";
        }
    }

    if (opts.check && fail == 0 && opts.verbose)
        std::cout << "全部已是规范格式（" << files.size() << " 个文件）\n";
    (void)changed;
    return fail ? 1 : 0;
}

} // namespace hao
