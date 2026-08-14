// ============================================================
//  HaoLang LLVM IR 文本发射器
// ------------------------------------------------------------
//  职责：
//    - 生成唯一的 SSA 寄存器名与基本块标签
//    - 管理字符串常量池（去重 + 精确长度计算 + IR 转义）
//    - 拼装最终 .ll 文本
//
//  设计说明：直接生成 IR 文本而非调用 LLVM C++ API，
//  因为官方 Windows 预编译包只提供 clang/lld 可执行文件。
//  clang 能直接编译 .ll，与 llc 等价。
// ============================================================
#pragma once

#include "sema/Type.h"

#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace hao {

class IREmitter {
public:
    IREmitter() = default;

    // 设置目标三元组，并填入匹配 datalayout（S128=栈 16B 对齐，防 CRT movdqa AV）。
    void setTargetTriple(const std::string& triple) {
        triple_ = triple;
        // 与 clang -target 同族默认 layout（省略版本后缀差异）
        if (triple_.find("windows-msvc") != std::string::npos) {
            if (triple_.rfind("aarch64", 0) == 0)
                dataLayout_ = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128";
            else
                dataLayout_ =
                    "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128";
        } else if (triple_.find("linux") != std::string::npos) {
            if (triple_.rfind("aarch64", 0) == 0)
                dataLayout_ = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128";
            else
                dataLayout_ =
                    "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128";
        } else if (triple_.find("apple-darwin") != std::string::npos) {
            if (triple_.rfind("aarch64", 0) == 0)
                dataLayout_ = "e-m:o-i64:64-i128:128-n32:64-S128";
            else
                dataLayout_ = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128";
        } else {
            dataLayout_ =
                "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128";
        }
    }
    const std::string& targetTriple() const { return triple_; }

    void setDataLayout(const std::string& dl) { dataLayout_ = dl; }

    // I3：调试元数据（仅当 debugEnabled 且有 DILocation 时 finish 才输出）
    void setDebugEnabled(bool v) { debugEnabled_ = v; }
    bool debugEnabled() const { return debugEnabled_; }
    void setDebugFile(const std::string& path) { debugFile_ = path; }
    void setDebugFileIfEmpty(const std::string& path) {
        if (debugFile_.empty()) debugFile_ = path;
    }
    // 返回 metadata 编号 N（用于 `, !dbg !N`）；未开 debug 返回 0
    unsigned internDILocation(unsigned line, unsigned col);
    // D3：薄 DILocalVariable；arg=0 局部，arg>=1 形参序号
    unsigned internDILocalVariable(const std::string& name, unsigned line,
                                   unsigned arg);
    // 空 !DIExpression() 的 metadata id（dbg.declare 第三操作数）
    unsigned diExpressionId();
    // 进入用户函数：新建 !DISubprogram，后续 DILocation 的 scope 指向它
    void beginDebugSubprogram(const std::string& name, unsigned line);
    void clearDebugSubprogram() { currentScope_ = 0; }

    // ------------------------------------------------------------
    //  唯一名字生成
    // ------------------------------------------------------------
    //  LLVM 要求同一函数内的"无名"寄存器 %0 %1 %2 ... 严格递增且连续，
    //  因此数字寄存器与带名字的寄存器必须使用独立计数器，
    //  否则命名寄存器会占用编号、导致后续数字寄存器编号倒退而报
    //  "instruction expected to be numbered '%N' or greater"。

    // 临时寄存器：%1, %2, ...
    std::string nextTemp() {
        return "%" + std::to_string(tempCounter_++);
    }

    // 带语义前缀的寄存器：%x.addr.3（使用独立计数器，不影响 %数字 序列）
    std::string nextNamed(const std::string& hint) {
        return "%" + hint + "." + std::to_string(namedCounter_++);
    }

    // 基本块标签：if.then.1（返回不带 % 的裸标签名）
    std::string nextLabel(const std::string& hint) {
        return hint + "." + std::to_string(labelCounter_++);
    }

    // ------------------------------------------------------------
    //  字符串常量池
    // ------------------------------------------------------------
    //  返回可直接用作 ptr 实参的全局名，如 @.str.0
    //  相同内容自动复用同一个全局常量
    std::string internString(const std::string& s) {
        auto it = stringPool_.find(s);
        if (it != stringPool_.end()) return it->second;

        std::string name = "@.str." + std::to_string(stringPool_.size());
        stringPool_[s] = name;

        // 长度必须是 转义后字节数 + 1（结尾 \00）。
        // 这里按原始字节数计算：IR 中 \XX 占 1 字节。
        std::ostringstream decl;
        decl << name << " = private unnamed_addr constant ["
             << (s.size() + 1) << " x i8] c\"" << escapeIR(s) << "\\00\"";
        globals_.push_back(decl.str());
        return name;
    }

    // 追加一条全局定义（虚表等）。这些必须出现在函数体之前，
    // 因此单独收集，由 finish() 统一插入到 globals 区。
    void addGlobal(const std::string& def) {
        globals_.push_back(def);
    }

    // ------------------------------------------------------------
    //  指令输出
    // ------------------------------------------------------------

    // 函数体内一条指令（自动缩进）
    void emit(const std::string& inst) {
        body_ << "  " << inst << "\n";
    }

    // 基本块标签行（不缩进）
    void emitLabel(const std::string& label) {
        body_ << label << ":\n";
        currentBlock_ = label;
    }

    // 当前正在填充的基本块名。phi 指令需要准确引用前驱块，
    // 由发射器统一维护，避免调用方自行追踪出错。
    const std::string& currentBlock() const { return currentBlock_; }

    // 顶层原样输出（函数定义头、结束花括号等）
    void emitRaw(const std::string& line) {
        body_ << line << "\n";
    }

    // 空行，便于阅读生成的 IR
    void emitBlank() { body_ << "\n"; }

    // ------------------------------------------------------------
    //  嵌套函数生成（lambda / 包装函数）
    // ------------------------------------------------------------
    //  lambda 的实现函数必须是顶层 define，不能嵌在当前函数体里。
    //  用法：pushFunctionState() 保存当前函数体并切换到空白缓冲，
    //  照常 emit 整个 define，再 popFunctionState() 取回完整文本，
    //  最后 addFunctionDef() 登记，由 finish() 统一追加到主函数体之后。
    struct SavedFunctionState {
        std::string body;
        unsigned tempCounter, namedCounter, labelCounter;
        std::string currentBlock;
        std::string entryAllocaBuf;
    };
    void pushFunctionState() {
        saved_.push_back({body_.str(), tempCounter_, namedCounter_,
                          labelCounter_, currentBlock_, entryAllocaBuf_});
        body_.str("");
        body_.clear();
        entryAllocaBuf_.clear();
        resetFunctionState();
    }
    std::string popFunctionState() {
        flushEntryAllocas();
        std::string def = body_.str();
        auto s = saved_.back();
        saved_.pop_back();
        body_.str(s.body);
        body_.clear();
        // str(s) 会把写指针放回起点，必须 seek 到末尾，否则后续写入
        // 会从头覆盖已恢复的函数体（曾导致主函数 define 头被覆盖、IR 损坏）
        body_.seekp(0, std::ios::end);
        tempCounter_ = s.tempCounter;
        namedCounter_ = s.namedCounter;
        labelCounter_ = s.labelCounter;
        currentBlock_ = s.currentBlock;
        entryAllocaBuf_ = s.entryAllocaBuf;
        return def;
    }
    /* 循环中扩 spill 池：alloca 插入当前函数 entry，保证支配所有 use */
    std::string emitEntryAllocaPtr(const std::string& hint) {
        std::string addr = nextNamed(hint);
        entryAllocaBuf_ += "  " + addr + " = alloca ptr\n";
        return addr;
    }
    void flushEntryAllocas() {
        if (entryAllocaBuf_.empty()) return;
        std::string b = body_.str();
        auto pos = b.rfind("entry:\n");
        if (pos != std::string::npos) {
            pos += 7;
            b.insert(pos, entryAllocaBuf_);
            body_.str("");
            body_.clear();
            body_ << b;
            body_.seekp(0, std::ios::end);
        }
        entryAllocaBuf_.clear();
    }
    // 登记一个独立的函数定义（lambda impl、函数值包装器），
    // 在 finish() 中输出到主函数体之后。
    void addFunctionDef(const std::string& def) { functionDefs_.push_back(def); }

    // ------------------------------------------------------------
    //  函数级状态
    // ------------------------------------------------------------
    // 每进入一个函数体重置寄存器编号，让 IR 更易读
    void resetFunctionState() {
        tempCounter_ = 1;
        namedCounter_ = 1;
        labelCounter_ = 1;
        currentBlock_ = "entry";
    }

    // ------------------------------------------------------------
    //  组装最终 .ll
    // ------------------------------------------------------------
    std::string finish() {
        flushEntryAllocas();
        std::ostringstream out;

        out << "; ============================================\n"
            << ";  由 HaoLang 编译器生成 —— 请勿手工修改\n"
            << "; ============================================\n\n";
        if (!dataLayout_.empty())
            out << "target datalayout = \"" << dataLayout_ << "\"\n";
        out << "target triple = \"" << triple_ << "\"\n\n";

        if (!globals_.empty()) {
            out << "; ---------- 全局常量（字符串 / 虚表） ----------\n";
            for (const auto& g : globals_) out << g << "\n";
            out << "\n";
        }

        out << "; ---------- HaoLang 运行时 ----------\n";
        for (const auto& d : runtimeDecls()) out << d << "\n";
        out << "\n";

        out << body_.str();

        // lambda impl / 包装函数等独立函数定义
        if (!functionDefs_.empty()) {
            out << "\n; ---------- lambda / 包装函数 ----------\n";
            for (const auto& d : functionDefs_) out << d << "\n";
        }

        // 属性组（如 returns_twice）放在文件末尾
        out << "\n";
        for (const auto& g : attributeGroups()) out << g << "\n";

        if (diReady_ && (!dilocs_.empty() || !dilocals_.empty())) {
            out << "\n; ---------- Debug Info (I3/D3) ----------\n";
            out << "!llvm.dbg.cu = !{!0}\n";
            out << "!llvm.module.flags = !{!1000, !1001}\n";
            out << "!1000 = !{i32 7, !\"Dwarf Version\", i32 5}\n";
            out << "!1001 = !{i32 2, !\"Debug Info Version\", i32 3}\n";

            std::string file = debugFile_.empty() ? "hao.hao" : debugFile_;
            std::string dir = ".";
            auto slash = file.find_last_of("/\\");
            std::string base = file;
            if (slash != std::string::npos) {
                dir = file.substr(0, slash);
                base = file.substr(slash + 1);
            }
            auto esc = [](const std::string& s) {
                std::string o;
                o.reserve(s.size());
                for (char c : s) {
                    if (c == '\\' || c == '"') o += '\\';
                    o += c;
                }
                return o;
            };

            out << "!0 = distinct !DICompileUnit(language: DW_LANG_C_plus_plus, "
                   "file: !1, producer: \"HaoLang\", isOptimized: true, "
                   "runtimeVersion: 0, emissionKind: FullDebug, enums: !2)\n";
            out << "!1 = !DIFile(filename: \"" << esc(base)
                << "\", directory: \"" << esc(dir) << "\")\n";
            out << "!2 = !{}\n";
            out << "!3 = !DISubroutineType(types: !2)\n";
            out << "!4 = distinct !DISubprogram(name: \"hao\", scope: !1, "
                   "file: !1, line: 1, type: !3, scopeLine: 1, "
                   "spFlags: DISPFlagDefinition, unit: !0, retainedNodes: !2)\n";
            for (const auto& sp : subprograms_) {
                out << "!" << sp.id << " = distinct !DISubprogram(name: \""
                    << esc(sp.name) << "\", scope: !1, file: !1, line: "
                    << sp.line << ", type: !3, scopeLine: " << sp.line
                    << ", spFlags: DISPFlagDefinition, unit: !0, "
                       "retainedNodes: !2)\n";
            }
            if (diTypeId_ != 0) {
                out << "!" << diTypeId_
                    << " = !DIBasicType(name: \"hao_val\", size: 64, "
                       "encoding: DW_ATE_unsigned)\n";
            }
            if (diExprId_ != 0) {
                out << "!" << diExprId_ << " = !DIExpression()\n";
            }
            for (const auto& lv : dilocals_) {
                out << "!" << lv.id << " = !DILocalVariable(name: \""
                    << esc(lv.name) << "\"";
                if (lv.arg > 0) out << ", arg: " << lv.arg;
                out << ", scope: !" << lv.scope << ", file: !1, line: "
                    << lv.line << ", type: !" << diTypeId_ << ")\n";
            }
            for (const auto& loc : dilocs_) {
                out << "!" << loc.id << " = !DILocation(line: " << loc.line
                    << ", column: " << loc.col << ", scope: !" << loc.scope
                    << ")\n";
            }
        }
        return out.str();
    }

    // 判断某个 C 符号是否已是内建运行时声明（避免 extern 重复 declare）
    static bool isRuntimeDeclared(const std::string& linkName) {
        for (const auto& d : runtimeDecls()) {
            // 形如 "declare void @hao_println_str(ptr)"，提取 @ 后到 '(' 的名字
            auto at = d.find('@');
            if (at == std::string::npos) continue;
            auto lp = d.find('(', at);
            if (lp == std::string::npos) continue;
            if (d.compare(at + 1, lp - at - 1, linkName) == 0) return true;
        }
        return false;
    }

    // 运行时函数声明（对应 stdlib/runtime_*.c 各模块）
    static const std::vector<std::string>& runtimeDecls() {
        static const std::vector<std::string> decls = {
            "declare void @llvm.dbg.declare(metadata, metadata, metadata)",
            "declare void @llvm.dbg.value(metadata, metadata, metadata)",
            "declare void @hao_println_str(ptr)",
            "declare void @hao_print_str(ptr)",
            "declare ptr  @hao_str_from_cstr(ptr)",
            "declare ptr  @hao_float_to_str(float)",
            "declare ptr  @hao_double_to_str(double)",
            "declare i8   @hao_str_eq(ptr, ptr)",
            "declare void @hao_panic_null()",
            "declare void @hao_dbg_set_src_loc(ptr, i32, i32)",
            "declare void @hao_dbg_clear_src_loc()",
            "declare void @hao_dbg_push_frame(ptr, i32, i32, ptr)",
            "declare void @hao_dbg_pop_frame()",
            "declare void @hao_dbg_clear_frame_args()",
            "declare void @hao_dbg_add_frame_arg(ptr, i32, i64)",
            "declare void @hao_panic_div_zero()",
            "declare void @hao_panic_index(i64, i64)",
            "declare void @hao_panic_overflow()",
            "declare {i64, i1} @llvm.sadd.with.overflow.i64(i64, i64)",
            "declare ptr  @hao_array_new(i64, i64, i64)",
            "declare ptr  @hao_make_args(i32, ptr)",  // main(args) 构造参数数组
            "declare i64  @hao_array_len(ptr)",
            "declare i64  @hao_array_check(ptr, i64)",
            "declare ptr  @hao_array_clone(ptr)",
            "declare void @hao_arraycopy(ptr, i64, ptr, i64, i64)",
            "declare i32  @hao_json_try_write(ptr, ptr, i32, i32, ptr, i32)",
            "declare ptr  @hao_object_new(i64, i64)",
            "declare ptr  @hao_object_new_map(i64, ptr, i64)",
            "declare ptr  @hao_handle_wrap(ptr)",  // 永生/外部 raw → NativeHandle（drop=NULL）
            "declare void @hao_gc_barrier(ptr, ptr) #1",
            "declare void @hao_gc_shade(ptr) #1",
            "declare void @hao_gc_add_root_slot(ptr)",
            "declare i64  @hao_gc_root_watermark()",
            "declare void @hao_gc_root_push(ptr)",
            "declare void @hao_gc_root_unwind(i64)",
            "declare void @hao_gc_safepoint()",
            "declare ptr  @hao_thread_start(ptr)",
            "declare i8   @hao_chan_make(ptr, i32)",
            "declare i32  @hao_chan_send(ptr, i64)",
            "declare i64  @hao_chan_recv(ptr)",
            "declare i32  @hao_chan_try_send(ptr, i64)",
            "declare i32  @hao_chan_try_recv(ptr, ptr)",
            "declare void @hao_chan_close(ptr)",
            "declare i32  @hao_chan_select(ptr, i32, i32)",
            "declare void @hao_thread_sleep_ms(i32)",
            "declare i8   @hao_type_is(ptr, ptr)",
            "declare void @hao_panic_cast(ptr)",
            "declare ptr  @hao_box_i64(i64)",
            "declare ptr  @hao_box_f64(double)",
            "declare ptr  @hao_box_f32(float)",
            "declare ptr  @hao_box_i32(i32)",
            "declare i64  @hao_unbox_i64(ptr)",
            "declare double @hao_unbox_f64(ptr)",
            "declare float @hao_unbox_f32(ptr)",
            "declare i32  @hao_unbox_i32(ptr)",
            // 异常：帧由 hao_try_alloc 分配，setjmp 在 IR 中直接调用
            // （必须在用户函数的栈帧里，不能包在会返回的辅助函数中）。
            "declare ptr  @hao_try_alloc(ptr)",
            "declare void @hao_try_end(i32)",
            "declare ptr  @hao_except_capture()",
            "declare void @hao_throw(ptr)",
            "declare void @hao_rethrow(ptr)",
            // setjmp 是 returns_twice 内建。Windows x64 的 _setjmp 需要当前
            // 帧指针作第二参数（配合 @llvm.frameaddress），Linux/musl 的 setjmp
            // 只需一个参数。两个都声明，IRGen 按目标三元组选用。
            "declare i32  @setjmp(ptr) #0",
            "declare i32  @_setjmp(ptr, ptr) #0",
            "declare ptr  @llvm.frameaddress.p0(i32)",
        };
        return decls;
    }

    // 属性组：#0 = returns_twice（setjmp）；#1 = GC 写屏障（方法论：noinline + 内存语义）
    static const std::vector<std::string>& attributeGroups() {
        static const std::vector<std::string> groups = {
            "attributes #0 = { returns_twice }",
            "attributes #1 = { noinline nounwind memory(readwrite) }",
        };
        return groups;
    }

private:
    // 将字符串转义为 LLVM IR 常量表示。
    // IR 只认 \XX 十六进制形式，且反斜杠自身必须写成 \\5C。
    static std::string escapeIR(const std::string& s) {
        static const char* hex = "0123456789ABCDEF";
        std::string r;
        r.reserve(s.size() * 2);
        for (unsigned char c : s) {
            if (c == '\\' || c == '"' || c < 0x20 || c >= 0x7F) {
                r += '\\';
                r += hex[(c >> 4) & 0xF];
                r += hex[c & 0xF];
            } else {
                r += static_cast<char>(c);
            }
        }
        return r;
    }

    public:
    // ------------------------------------------------------------
    //  值装箱 / 拆箱（任意槽位类型 <-> i64）
    // ------------------------------------------------------------
    //  数组 push/pop、异常跨帧返回槽等统一按 8 字节 i64 存取。
    //  v0.25+：Int=i32 / Long=i64 / Float=f32 / Double=f64 / Short=i16 / Byte=u8（llvm i8）/ SByte=i8
    // 按真实 LLVM 类型装箱：Int?/String/Class 等 llvmType=ptr 必须 ptrtoint，
    // 不可只看 kind（否则 try 返回 Int? / [Int?] push 会 sext i32 %ptr）。
    std::string boxToI64(const std::string& ir, const TypePtr& t) {
        if (t && t->llvmType() == "ptr") {
            std::string r = nextTemp();
            emit(r + " = ptrtoint ptr " + ir + " to i64");
            return r;
        }
        return boxToI64(ir, t ? t->kind : TypeKind::Long);
    }

    std::string boxToI64(const std::string& ir, TypeKind kind) {
        // Long / ULong / UIntPtr 底层已是 i64
        if (kind == TypeKind::Long || kind == TypeKind::ULong ||
            kind == TypeKind::UIntPtr) return ir;
        switch (kind) {
            case TypeKind::Double: {
                std::string r = nextTemp();
                emit(r + " = bitcast double " + ir + " to i64");
                return r;
            }
            case TypeKind::Float: {
                // 先 bitcast 再 zext，保证 SSA 编号递增
                std::string bits = nextTemp();
                emit(bits + " = bitcast float " + ir + " to i32");
                std::string r = nextTemp();
                emit(r + " = zext i32 " + bits + " to i64");
                return r;
            }
            case TypeKind::Int:
            case TypeKind::Char: {
                std::string r = nextTemp();
                emit(r + " = sext i32 " + ir + " to i64");
                return r;
            }
            case TypeKind::UInt: {
                std::string r = nextTemp();
                emit(r + " = zext i32 " + ir + " to i64");
                return r;
            }
            case TypeKind::Short: {
                std::string r = nextTemp();
                emit(r + " = sext i16 " + ir + " to i64");
                return r;
            }
            case TypeKind::UShort: {
                std::string r = nextTemp();
                emit(r + " = zext i16 " + ir + " to i64");
                return r;
            }
            case TypeKind::SByte: {
                std::string r = nextTemp();
                emit(r + " = sext i8 " + ir + " to i64");
                return r;
            }
            case TypeKind::Bool:
            case TypeKind::Byte: {
                std::string r = nextTemp();
                emit(r + " = zext i8 " + ir + " to i64");
                return r;
            }
            default: {
                std::string r = nextTemp();
                emit(r + " = ptrtoint ptr " + ir + " to i64");
                return r;
            }
        }
    }

    std::string unboxFromI64(const std::string& raw, const TypePtr& t) {
        if (t && t->llvmType() == "ptr") {
            std::string r = nextTemp();
            emit(r + " = inttoptr i64 " + raw + " to ptr");
            return r;
        }
        return unboxFromI64(raw, t ? t->kind : TypeKind::Long);
    }

    std::string unboxFromI64(const std::string& raw, TypeKind kind) {
        if (kind == TypeKind::Long || kind == TypeKind::ULong ||
            kind == TypeKind::UIntPtr) return raw;
        switch (kind) {
            case TypeKind::Double: {
                std::string r = nextTemp();
                emit(r + " = bitcast i64 " + raw + " to double");
                return r;
            }
            case TypeKind::Float: {
                std::string bits = nextTemp();
                emit(bits + " = trunc i64 " + raw + " to i32");
                std::string r = nextTemp();
                emit(r + " = bitcast i32 " + bits + " to float");
                return r;
            }
            case TypeKind::Int:
            case TypeKind::UInt:
            case TypeKind::Char: {
                std::string r = nextTemp();
                emit(r + " = trunc i64 " + raw + " to i32");
                return r;
            }
            case TypeKind::Short:
            case TypeKind::UShort: {
                std::string r = nextTemp();
                emit(r + " = trunc i64 " + raw + " to i16");
                return r;
            }
            case TypeKind::SByte:
            case TypeKind::Bool:
            case TypeKind::Byte: {
                std::string r = nextTemp();
                emit(r + " = trunc i64 " + raw + " to i8");
                return r;
            }
            default: {
                std::string r = nextTemp();
                emit(r + " = inttoptr i64 " + raw + " to ptr");
                return r;
            }
        }
    }

private:
    unsigned tempCounter_ = 1;
    unsigned namedCounter_ = 1;
    unsigned labelCounter_ = 1;
    std::string currentBlock_ = "entry";
    std::string triple_ = "x86_64-pc-windows-msvc";
    std::string dataLayout_ =
        "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128";
    std::string entryAllocaBuf_;

    std::map<std::string, std::string> stringPool_;
    std::vector<std::string> globals_;
    std::vector<std::string> functionDefs_;
    std::vector<SavedFunctionState> saved_;
    std::ostringstream body_;

    // ---- I3/D3 Debug Info ----
    bool debugEnabled_ = false;
    bool diReady_ = false;
    std::string debugFile_;
    unsigned nextMetaId_ = 5;   // !0..!4 骨架预留（!4=回退 SP）
    unsigned currentScope_ = 0; // 当前 DISubprogram id；0 → 用 !4
    unsigned diTypeId_ = 0;     // 薄 hao_val 基本类型
    unsigned diExprId_ = 0;     // 空 DIExpression
    struct DILocRec { unsigned id, line, col, scope; };
    struct DISubRec { unsigned id; std::string name; unsigned line; };
    struct DILocalRec {
        unsigned id;
        std::string name;
        unsigned line;
        unsigned arg;   // 0=局部
        unsigned scope;
    };
    std::vector<DILocRec> dilocs_;
    std::vector<DISubRec> subprograms_;
    std::vector<DILocalRec> dilocals_;
    std::map<std::uint64_t, unsigned> dilocCache_;

    void ensureDISkeleton() {
        if (diReady_) return;
        diReady_ = true;
        nextMetaId_ = 5;
    }
    void ensureDIAuxTypes() {
        ensureDISkeleton();
        if (diTypeId_ == 0) diTypeId_ = nextMetaId_++;
        if (diExprId_ == 0) diExprId_ = nextMetaId_++;
    }
};

inline void IREmitter::beginDebugSubprogram(const std::string& name,
                                           unsigned line) {
    if (!debugEnabled_) return;
    ensureDISkeleton();
    unsigned id = nextMetaId_++;
    subprograms_.push_back({id, name, line ? line : 1});
    currentScope_ = id;
}

inline unsigned IREmitter::internDILocation(unsigned line, unsigned col) {
    if (!debugEnabled_ || line == 0) return 0;
    ensureDISkeleton();
    unsigned scope = currentScope_ != 0 ? currentScope_ : 4;
    // scope(16) | line(32) | col(16) —— 列号截断到 16 位够用
    std::uint64_t key = (static_cast<std::uint64_t>(scope & 0xffff) << 48) |
                        (static_cast<std::uint64_t>(line) << 16) |
                        static_cast<std::uint64_t>(col & 0xffff);
    auto it = dilocCache_.find(key);
    if (it != dilocCache_.end()) return it->second;
    unsigned id = nextMetaId_++;
    dilocCache_[key] = id;
    dilocs_.push_back({id, line, col, scope});
    return id;
}

inline unsigned IREmitter::diExpressionId() {
    if (!debugEnabled_) return 0;
    ensureDIAuxTypes();
    return diExprId_;
}

inline unsigned IREmitter::internDILocalVariable(const std::string& name,
                                                unsigned line, unsigned arg) {
    if (!debugEnabled_ || name.empty()) return 0;
    ensureDIAuxTypes();
    unsigned scope = currentScope_ != 0 ? currentScope_ : 4;
    unsigned id = nextMetaId_++;
    dilocals_.push_back({id, name, line ? line : 1, arg, scope});
    return id;
}

} // namespace hao
