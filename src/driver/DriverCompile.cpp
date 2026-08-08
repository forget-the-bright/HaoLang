// ============================================================
//  HaoLang 编译驱动 —— .hao -> .ll
// ------------------------------------------------------------
//  读取源文件（经 util::readFile 统一处理 BOM）、ANTLR 词法语法分析、
//  语义分析与 IR 生成，最终写出 LLVM IR 文本。
//  链接步骤在 DriverLink.cpp。
// ============================================================

#include "driver/Driver.h"

#include "HaoVersion.h"
#include "antlr4-runtime.h"
#include "driver/DriverResolve.h"
#include "irgen/IRGen.h"
#include "sema/Diagnostic.h"
#include "util/FileUtil.h"
#include "util/PathUtil.h"

#include <fstream>
#include <iostream>
#include <string>

namespace hao {

bool Driver::compileToIR(const BuildOptions& opts, std::string& llPath,
                         std::vector<std::string>* outExternLibs) {
    DiagnosticEngine diags;

    // ---- 确定输入：多文件/目录，或单个源文件 ----
    // 调用方（build/run）应已 applyHaoProjectToOptions。
    std::vector<std::string> inputs;
    if (opts.sourceFiles.empty()) {
        inputs.push_back(opts.sourceFile);
    } else {
        inputs = opts.sourceFiles;
    }

    // ---- 解析入口包及其 import 闭包（持有 AST 生命周期）----
    PackageResolver resolver;
    resolver.setTestMode(opts.testMode);
    for (const auto& r : opts.packageSearchRoots)
        resolver.addSearchRoot(r);
    if (opts.verbose) {
        for (const auto& r : opts.packageSearchRoots)
            std::cout << "[hao] localReference: " << r << "\n";
        if (opts.testMode)
            std::cout << "[hao] testMode: 纳入 *_test.hao\n";
    }
    if (!resolver.resolve(inputs, diags)) {
        diags.print("");
        return false;
    }
    auto units = resolver.sourceUnits();

    // ---- 语义分析 + IR 生成 ----
    IRGen gen(diags);
    gen.setTestMode(opts.testMode);
    auto p = opts.target.empty() ? hostPlatform() : platformFromName(opts.target);
    if (p.triple) gen.setTargetTriple(p.triple);

    std::string ir = gen.generate(units);

    if (!diags.diags().empty()) diags.print("");
    if (diags.hasErrors()) return false;

    // 带出 extern @link(...) 声明的外部链接库，供链接阶段追加到命令。
    if (outExternLibs) *outExternLibs = gen.linkLibraries();

    // ---- 写出 .ll ----
    // 输出名：显式 -o 优先；否则入口是目录时用目录名，是文件时去扩展名
    std::string baseOut;
    if (!opts.outputFile.empty()) {
        baseOut = opts.outputFile;
    } else if (isDirectory(inputs[0])) {
        // 目录以其最后一段命名（如 ./test/multifile -> multifile）
        std::string d = inputs[0];
        while (d.size() > 1 && (d.back() == '/' || d.back() == '\\'))
            d.pop_back();
        size_t slash = d.find_last_of("/\\");
        baseOut = (slash == std::string::npos) ? d : d.substr(slash + 1);
    } else {
        baseOut = inputs[0];
    }
    llPath = stripExt(baseOut) + ".ll";
    std::ofstream out(llPath, std::ios::binary);
    if (!out) {
        std::cerr << "错误: 无法写入 " << llPath << "\n";
        return false;
    }
    out << ir;
    out.close();

    if (opts.verbose)
        std::cout << "[hao] 生成 IR: " << llPath << "\n";
    return true;
}

} // namespace hao
