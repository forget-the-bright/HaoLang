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

#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace hao {

class IREmitter {
public:
    IREmitter() = default;

    // 设置目标三元组。交叉编译时必须与 clang --target 一致，
    // 否则 clang 会以自身默认 triple 覆盖并给出 -Woverride-module 警告。
    void setTargetTriple(const std::string& triple) { triple_ = triple; }
    const std::string& targetTriple() const { return triple_; }

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
    };
    void pushFunctionState() {
        saved_.push_back({body_.str(), tempCounter_, namedCounter_,
                          labelCounter_, currentBlock_});
        body_.str("");
        body_.clear();
        resetFunctionState();
    }
    std::string popFunctionState() {
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
        return def;
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
    std::string finish() const {
        std::ostringstream out;

        out << "; ============================================\n"
            << ";  由 HaoLang 编译器生成 —— 请勿手工修改\n"
            << "; ============================================\n\n"
            << "target triple = \"" << triple_ << "\"\n\n";

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
            "declare void @hao_println_str(ptr)",
            "declare void @hao_println_sbyte(i8)",
            "declare void @hao_println_byte(i8)",
            "declare void @hao_println_short(i16)",
            "declare void @hao_println_ushort(i16)",
            "declare void @hao_println_int(i32)",
            "declare void @hao_println_uint(i32)",
            "declare void @hao_println_long(i64)",
            "declare void @hao_println_ulong(i64)",
            "declare void @hao_println_float(float)",
            "declare void @hao_println_double(double)",
            "declare void @hao_println_bool(i8)",
            "declare void @hao_println_char(i32)",
            "declare void @hao_print_str(ptr)",
            "declare ptr  @hao_str_concat(ptr, ptr)",
            "declare ptr  @hao_str_from_cstr(ptr)",
            "declare ptr  @hao_int_to_str(i32)",
            "declare ptr  @hao_long_to_str(i64)",
            "declare ptr  @hao_uint_to_str(i32)",
            "declare ptr  @hao_ulong_to_str(i64)",
            "declare ptr  @hao_float_to_str(float)",
            "declare ptr  @hao_double_to_str(double)",
            "declare ptr  @hao_bool_to_str(i8)",
            "declare ptr  @hao_char_to_str(i32)",
            "declare i64  @hao_str_len(ptr)",
            "declare i8   @hao_str_eq(ptr, ptr)",
            "declare i32  @hao_str_char_at(ptr, i64)",
            "declare void @hao_panic_null()",
            "declare void @hao_panic_div_zero()",
            "declare void @hao_panic_index(i64, i64)",
            "declare void @hao_panic_overflow()",
            "declare {i64, i1} @llvm.sadd.with.overflow.i64(i64, i64)",
            "declare ptr  @hao_array_new(i64, i64, i64)",
            "declare ptr  @hao_make_args(i32, ptr)",  // main(args) 构造参数数组
            "declare i64  @hao_array_len(ptr)",
            "declare i64  @hao_array_cap(ptr)",
            "declare i64  @hao_array_check(ptr, i64)",
            "declare ptr  @hao_array_push(ptr, i64)",
            "declare i64  @hao_array_pop(ptr)",
            "declare ptr  @hao_object_new(i64, i64)",
            "declare void @hao_gc_barrier(ptr, ptr)",
            "declare void @hao_gc_shade(ptr)",
            "declare void @hao_gc_add_root_slot(ptr)",
            "declare i64  @hao_gc_root_watermark()",
            "declare void @hao_gc_root_push(ptr)",
            "declare void @hao_gc_root_unwind(i64)",
            "declare void @hao_gc_safepoint()",
            "declare i64  @hao_thread_start(ptr)",
            "declare i8   @hao_chan_make(ptr, i32)",
            "declare i32  @hao_chan_send(ptr, i64)",
            "declare i64  @hao_chan_recv(ptr)",
            "declare i32  @hao_chan_try_send(ptr, i64)",
            "declare i32  @hao_chan_try_recv(ptr, ptr)",
            "declare void @hao_chan_close(ptr)",
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

    // 属性组：#0 = { returns_twice }，供 hao_try_begin 使用
    static const std::vector<std::string>& attributeGroups() {
        static const std::vector<std::string> groups = {
            "attributes #0 = { returns_twice }",
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
    //  v0.25：Int=i32 / Long=i64 / Float=f32 / Double=f64 / Short=i16 / Byte=i8
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

    std::map<std::string, std::string> stringPool_;
    std::vector<std::string> globals_;
    std::vector<std::string> functionDefs_;
    std::vector<SavedFunctionState> saved_;
    std::ostringstream body_;
};

} // namespace hao
