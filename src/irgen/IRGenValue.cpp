// ============================================================
//  HaoLang IR 生成 —— 值转换与类型推断辅助
// ------------------------------------------------------------
//  从 IRGenExpr.cpp 拆分而来，逻辑保持不变。这些是表达式生成与其它
//  模块共享的底层工具：变量装载、类型强制、Bool/字符串转换、字段/
//  数组元素寻址，以及纯类型推断（不发射指令，供 when 表达式、lambda
//  返回类型推断等使用）。
// ============================================================

#include "irgen/IRGen.h"

#include <cstdio>
#include <cstdlib>

namespace hao {

void IRGen::pushSmartCastFrame() { smartCastStack_.emplace_back(); }
void IRGen::popSmartCastFrame() {
    if (!smartCastStack_.empty()) smartCastStack_.pop_back();
}
void IRGen::addSmartCast(const std::string& name, TypePtr nonNull) {
    if (smartCastStack_.empty() || !nonNull || name.empty()) return;
    smartCastStack_.back()[name] = std::move(nonNull);
}
void IRGen::invalidateSmartCast(const std::string& name) {
    for (auto& frame : smartCastStack_) frame.erase(name);
}
TypePtr IRGen::lookupSmartCast(const std::string& name) const {
    for (auto it = smartCastStack_.rbegin(); it != smartCastStack_.rend(); ++it) {
        auto f = it->find(name);
        if (f != it->end()) return f->second;
    }
    return nullptr;
}

IRGen::NullCheckFact IRGen::analyzeNullCheck(antlr4::tree::ParseTree* expr) {
    NullCheckFact out;
    if (!expr) return out;

    // 在子树中找 equality（x == null / x != null）；拒绝 &&/|| 复合
    HaoLangParser::EqualityExprContext* eq = nullptr;
    std::vector<antlr4::tree::ParseTree*> stack{expr};
    while (!stack.empty()) {
        antlr4::tree::ParseTree* n = stack.back();
        stack.pop_back();
        if (!n) continue;
        if (auto* e = dynamic_cast<HaoLangParser::EqualityExprContext*>(n)) {
            if (e->relationalExpr().size() == 2) eq = e;
            continue;
        }
        if (auto* a = dynamic_cast<HaoLangParser::AndExprContext*>(n)) {
            if (a->bitOrExpr().size() > 1) continue;
        }
        if (auto* o = dynamic_cast<HaoLangParser::OrExprContext*>(n)) {
            if (o->andExpr().size() > 1) continue;
        }
        for (auto* c : n->children) stack.push_back(c);
    }
    if (!eq) return out;

    std::string op;
    if (eq->EQ().size() == 1) op = "==";
    else if (eq->NEQ().size() == 1) op = "!=";
    else return out;

    // 下钻到 IdentPrimary / Literal；勿越过 Primary 落到 TerminalNode
    auto findIdent = [](antlr4::tree::ParseTree* side) -> std::string {
        std::vector<antlr4::tree::ParseTree*> st{side};
        while (!st.empty()) {
            auto* n = st.back(); st.pop_back();
            if (!n) continue;
            if (auto* id = dynamic_cast<HaoLangParser::IdentPrimaryContext*>(n))
                return id->IDENT()->getText();
            if (dynamic_cast<HaoLangParser::LiteralContext*>(n)) continue;
            for (auto* c : n->children) st.push_back(c);
        }
        return "";
    };
    auto isNullLit = [](antlr4::tree::ParseTree* side) -> bool {
        std::vector<antlr4::tree::ParseTree*> st{side};
        while (!st.empty()) {
            auto* n = st.back(); st.pop_back();
            if (!n) continue;
            if (auto* lit = dynamic_cast<HaoLangParser::LiteralContext*>(n))
                return lit->NULL_LIT() != nullptr;
            for (auto* c : n->children) st.push_back(c);
        }
        return false;
    };
    auto* a = eq->relationalExpr(0);
    auto* b = eq->relationalExpr(1);
    std::string name;
    if (!findIdent(a).empty() && isNullLit(b)) name = findIdent(a);
    else if (isNullLit(a) && !findIdent(b).empty()) name = findIdent(b);
    else return out;

    auto sym = syms_.lookup(name);
    if (!sym || sym->kind != SymbolKind::Variable || !sym->type ||
        !sym->type->nullable) {
        return out;
    }
    out.name = name;
    out.isNotNull = (op == "!=");
    return out;
}

Value IRGen::unboxNullableKnown(const Value& base, const TypePtr& nonNull) {
    if (!base.type || !nonNull) return Value();
    if (!base.type->isBoxedNullable())
        return Value(base.ir, nonNull);
    auto unboxI64 = [&]() {
        return Value(emitCall("i64", "@hao_unbox_i64", "ptr " + base.ir), nonNull);
    };
    auto unboxI32 = [&]() {
        return Value(emitCall("i32", "@hao_unbox_i32", "ptr " + base.ir), nonNull);
    };
    auto unboxNarrow = [&](const std::string& toTy) {
        std::string wide = emitCall("i32", "@hao_unbox_i32", "ptr " + base.ir);
        std::string reg = emitCast("trunc", "i32", wide, toTy);
        return Value(reg, nonNull);
    };
    switch (base.type->kind) {
        case TypeKind::Long: case TypeKind::ULong: case TypeKind::UIntPtr:
            return unboxI64();
        case TypeKind::Int: case TypeKind::UInt: case TypeKind::Char:
            return unboxI32();
        case TypeKind::Bool:
            return unboxNarrow("i8");
        case TypeKind::Short: case TypeKind::UShort:
            return unboxNarrow("i16");
        case TypeKind::SByte: case TypeKind::Byte:
            return unboxNarrow("i8");
        case TypeKind::Double: {
            return Value(emitCall("double", "@hao_unbox_f64", "ptr " + base.ir), nonNull);
        }
        case TypeKind::Float: {
            return Value(emitCall("float", "@hao_unbox_f32", "ptr " + base.ir), nonNull);
        }
        default:
            return unboxI32();
    }
}

Value IRGen::loadVar(const SymbolPtr& sym) {
    std::string ptr = varValuePtr(sym);
    Value v(emitLoad(sym->type->llvmType(), ptr), sym->type);
    if (auto nt = lookupSmartCast(sym->name)) {
        if (sym->type->isBoxedNullable())
            return unboxNullableKnown(v, nt);
        return Value(v.ir, nt);
    }
    return v;
}

// 被 lambda 捕获的可变变量被装箱到堆 cell：
//   sym->irAddr 是栈上的 ptr holder（指向 cell），cell 才是真实值地址。
// 普通变量的真实值地址就是 sym->irAddr 本身。
std::string IRGen::varValuePtr(const SymbolPtr& sym) {
    if (!sym->boxed) return sym->irAddr;
    return emitLoad("ptr", sym->irAddr);
}

std::string IRGen::arrayArgSlot(antlr4::tree::ParseTree* expr,
                                const Value& arrVal) {
    auto makeTempSlot = [&]() -> std::string {
        std::string addr = emitAlloca("ptr");
        emitStore("ptr", arrVal.ir, addr);
        return addr;
    };

    if (!expr) return makeTempSlot();

    // 沿单子节点包装层下沉，但停在 postfix/ident，勿剥到 IDENT 终结符。
    antlr4::tree::ParseTree* node = expr;
    for (;;) {
        if (dynamic_cast<HaoLangParser::PostfixExprContext*>(node) ||
            dynamic_cast<HaoLangParser::IdentPrimaryContext*>(node))
            break;
        if (node->children.size() != 1) break;
        auto* next = node->children[0];
        if (dynamic_cast<antlr4::tree::TerminalNode*>(next)) break;
        node = next;
    }

    auto tryIdent = [&](HaoLangParser::IdentPrimaryContext* id) -> std::string {
        if (!id) return "";
        auto sym = syms_.lookup(id->IDENT()->getText());
        if (sym && sym->kind == SymbolKind::Variable)
            return varValuePtr(sym);
        return "";
    };

    if (auto* pf = dynamic_cast<HaoLangParser::PostfixExprContext*>(node)) {
        auto ops = pf->postfixOp();
        if (ops.empty()) {
            std::string s = tryIdent(
                dynamic_cast<HaoLangParser::IdentPrimaryContext*>(pf->primary()));
            if (!s.empty()) return s;
        } else {
            bool onlyMemBang = true;
            for (auto* op : ops) {
                if (!dynamic_cast<HaoLangParser::MemberAccessContext*>(op) &&
                    !dynamic_cast<HaoLangParser::NotNullAssertContext*>(op))
                    onlyMemBang = false;
            }
            if (onlyMemBang &&
                dynamic_cast<HaoLangParser::MemberAccessContext*>(ops.back())) {
                Value recv;
                if (evalAssignRecv(pf, ops.size() - 1, recv) &&
                    recv.type->kind == TypeKind::Class) {
                    auto* last = static_cast<HaoLangParser::MemberAccessContext*>(
                        ops.back());
                    auto ci = classOfType(recv.type);
                    if (ci) {
                        std::string fname = last->IDENT()->getText();
                        if (const FieldInfo* fi = ci->findField(fname))
                            return fieldPtr(recv.ir, fi->slot);
                        if (const FieldInfo* sfi = ci->findStaticField(fname))
                            return "@" + ci->name + "." + sfi->name;
                    }
                }
            }
        }
    } else if (auto* id = dynamic_cast<HaoLangParser::IdentPrimaryContext*>(node)) {
        std::string s = tryIdent(id);
        if (!s.empty()) return s;
    }

    // 非 lvalue：临时槽（形参内 += 不影响调用方）
    return makeTempSlot();
}

std::string IRGen::formatCallArg(const TypePtr& paramTy,
                                 antlr4::tree::ParseTree* expr,
                                 Value arg,
                                 bool arrayByRef) {
    /* 仅当 coerce 产生新 IR（典型：T→T? 的 hao_box_*）才挂根。
       已由调用方 rootGcOperand 的实参勿再 spill——曾致套件 OOP 段 AV。 */
    std::string irBefore = arg.ir;
    arg = coerce(arg, paramTy, 0, 0);
    if (arg.valid() && isGcPointerType(arg.type) && arg.ir != irBefore)
        rootGcOperand(arg);
    if (paramTy && paramTy->kind == TypeKind::Array && !paramTy->nullable) {
        if (arrayByRef)
            return "ptr " + arrayArgSlot(expr, arg);
        return "ptr " + arg.ir;   // extern C：直接传数组指针
    }
    return paramTy->llvmType() + " " + arg.ir;
}

Value IRGen::coerce(const Value& v, const TypePtr& target, size_t line, size_t col) {
    if (!v.valid() || !target) return v;

    // 数值互转（拓宽 / 窄化）；可空装箱另案，不走此路径
    if (v.type->isNumeric() && target->isNumeric() &&
        !v.type->nullable && !target->nullable &&
        v.type->kind != target->kind) {
        TypeKind from = v.type->kind;
        TypeKind to = target->kind;
        std::string src = v.ir;
        std::string fromTy = v.type->llvmType();

        // 整数互转：按位宽；无符号拓宽用 zext，有符号用 sext；同宽仅改类型标签
        if (v.type->isInteger() && target->isInteger()) {
            int fw = Type::bitWidthBits(from), tw = Type::bitWidthBits(to);
            if (fw < tw) {
                std::string op = v.type->isUnsigned() ? "zext" : "sext";
                return Value(emitCast(op, fromTy, src, target->llvmType()), target);
            }
            if (fw > tw) {
                return Value(emitCast("trunc", fromTy, src, target->llvmType()),
                             target);
            }
            // 同宽有符号/无符号：位模式不变，只改类型
            return Value(src, target);
        }

        // 整数 → 浮点
        if (v.type->isInteger() && target->isFloating()) {
            std::string op = v.type->isUnsigned() ? "uitofp" : "sitofp";
            return Value(emitCast(op, fromTy, src, target->llvmType()), target);
        }

        if (from == TypeKind::Float && to == TypeKind::Double) {
            return Value(emitCast("fpext", "float", src, "double"), target);
        }
        if (from == TypeKind::Double && to == TypeKind::Float) {
            return Value(emitCast("fptrunc", "double", src, "float"), target);
        }

        if (v.type->isFloating() && target->isInteger()) {
            std::string op = target->isUnsigned() ? "fptoui" : "fptosi";
            return Value(emitCast(op, fromTy, src, target->llvmType()), target);
        }
    }

    // 数值 → 不同宽度的可空数值：先升/降到非空底层再装箱
    if (target->nullable && !v.type->nullable &&
        target->isNumeric() && v.type->isNumeric() &&
        v.type->kind != target->kind) {
        auto underlying = std::make_shared<Type>(*target);
        underlying->nullable = false;
        Value mid = coerce(v, underlying, line, col);
        return coerce(mid, target, line, col);
    }

    // T -> T?：值类型装箱为指针。调用面由 formatCallArg 挂根；
    // 此处不 root——?. / ?? 的 phi 前驱里 spill 易与块标签纠缠（套件 AV）。
    if (target->nullable && !v.type->nullable &&
        v.type->kind == target->kind && v.type->sameShape(*target)) {
        if (v.type->kind == TypeKind::Long || v.type->kind == TypeKind::ULong ||
            v.type->kind == TypeKind::UIntPtr) {
            return Value(emitCall("ptr", "@hao_box_i64", "i64 " + v.ir), target);
        }
        if (v.type->kind == TypeKind::Int || v.type->kind == TypeKind::UInt ||
            v.type->kind == TypeKind::Char) {
            return Value(emitCall("ptr", "@hao_box_i32", "i32 " + v.ir), target);
        }
        if (v.type->kind == TypeKind::Bool) {
            std::string wide = emitCast("zext", "i8", v.ir, "i32");
            return Value(emitCall("ptr", "@hao_box_i32", "i32 " + wide), target);
        }
        if (v.type->kind == TypeKind::Short) {
            std::string wide = emitCast("sext", "i16", v.ir, "i32");
            return Value(emitCall("ptr", "@hao_box_i32", "i32 " + wide), target);
        }
        if (v.type->kind == TypeKind::UShort) {
            std::string wide = emitCast("zext", "i16", v.ir, "i32");
            return Value(emitCall("ptr", "@hao_box_i32", "i32 " + wide), target);
        }
        if (v.type->kind == TypeKind::SByte) {
            std::string wide = emitCast("sext", "i8", v.ir, "i32");
            return Value(emitCall("ptr", "@hao_box_i32", "i32 " + wide), target);
        }
        if (v.type->kind == TypeKind::Byte) {
            std::string wide = emitCast("zext", "i8", v.ir, "i32");
            return Value(emitCall("ptr", "@hao_box_i32", "i32 " + wide), target);
        }
        if (v.type->kind == TypeKind::Double) {
            return Value(emitCall("ptr", "@hao_box_f64", "double " + v.ir), target);
        }
        if (v.type->kind == TypeKind::Float) {
            return Value(emitCall("ptr", "@hao_box_f32", "float " + v.ir), target);
        }
        return Value(v.ir, target);
    }

    if (target->nullable && v.type->nullable &&
        v.type->isReferenceType() && target->isReferenceType()) {
        auto vn = std::make_shared<Type>(*v.type); vn->nullable = false;
        auto tn = std::make_shared<Type>(*target); tn->nullable = false;
        if (isAssignable(vn, tn)) return Value(v.ir, target);
    }

    (void)line; (void)col;
    return v;
}

bool IRGen::ensureNonNullOperand(const Value& v, antlr4::ParserRuleContext* ctx,
                                 const std::string& op) {
    if (v.type && v.type->nullable) {
        error(ctx, "可空类型 " + v.type->toString() + " 不能直接用于运算符 '" +
                   op + "'，请先用 !! 或 ??");
        return false;
    }
    return true;
}

void IRGen::emitIntDivZeroCheck(const Value& divisor) {
    if (!divisor.type || !divisor.type->isInteger()) return;
    std::string isZero = emitICmp("eq", divisor.type->llvmType(), divisor.ir, "0");
    std::string panicL = em_.nextLabel("div0.panic");
    std::string okL = em_.nextLabel("div0.ok");
    emitCondBr(isZero, panicL, okL);
    em_.emitLabel(panicL);
    emitCallVoid("@hao_panic_div_zero", "");
    emitUnreachable();
    em_.emitLabel(okL);
    blockTerminated_ = false;
}

Value IRGen::emitSafeSignedDivisor(const Value& dividend, const Value& divisor) {
    if (!divisor.type || !divisor.type->isInteger() || divisor.type->isUnsigned())
        return divisor;
    if (!dividend.type || !dividend.type->isInteger()) return divisor;
    std::string ty = divisor.type->llvmType();
    int bits = Type::bitWidthBits(divisor.type->kind);
    std::string minLit;
    if (bits <= 8)       minLit = "-128";
    else if (bits <= 16) minLit = "-32768";
    else if (bits <= 32) minLit = "-2147483648";
    else                 minLit = "-9223372036854775808";
    // MIN/-1 → 除数改 1：sdiv 得 MIN，srem 得 0（无毒值）
    std::string isNeg1 = emitICmp("eq", ty, divisor.ir, "-1");
    std::string isMin = emitICmp("eq", ty, dividend.ir, minLit);
    std::string ov = emitBinOp("and", "i1", isNeg1, isMin);
    std::string safe = emitSelect(ov, ty, "1", divisor.ir);
    return Value(safe, divisor.type);
}

std::string IRGen::toI1(const Value& v) {
    // Bool 存 i8（0/1），比较跳转需要 i1
    return emitICmp("ne", "i8", v.ir, "0");
}

Value IRGen::toStringValue(const Value& v) {
    // 可空须先 !! / ??；否则会把装箱 ptr 当 i32/i64 传给 to_str
    if (v.type && v.type->nullable) return Value();
    switch (v.type->kind) {
        case TypeKind::String:
            return v;
        case TypeKind::Long:
            return Value(emitCall("ptr", "@hao_long_to_str", "i64 " + v.ir),
                         Type::makeString());
        case TypeKind::ULong:
        case TypeKind::UIntPtr:
            return Value(emitCall("ptr", "@hao_ulong_to_str", "i64 " + v.ir),
                         Type::makeString());
        case TypeKind::Int:
            return Value(emitCall("ptr", "@hao_int_to_str", "i32 " + v.ir),
                         Type::makeString());
        case TypeKind::UInt:
            return Value(emitCall("ptr", "@hao_uint_to_str", "i32 " + v.ir),
                         Type::makeString());
        case TypeKind::SByte:
        case TypeKind::Byte:
        case TypeKind::Short:
        case TypeKind::UShort: {
            Value asInt = coerce(v, Type::makeInt(), 0, 0);
            return toStringValue(asInt);
        }
        case TypeKind::Double:
            return Value(emitCall("ptr", "@hao_double_to_str", "double " + v.ir),
                         Type::makeString());
        case TypeKind::Float:
            return Value(emitCall("ptr", "@hao_float_to_str", "float " + v.ir),
                         Type::makeString());
        case TypeKind::Bool:
            return Value(emitCall("ptr", "@hao_bool_to_str", "i8 " + v.ir),
                         Type::makeString());
        case TypeKind::Char:
            return Value(emitCall("ptr", "@hao_char_to_str", "i32 " + v.ir),
                         Type::makeString());
        default:
            return Value();
    }
}

// 计算对象字段地址
std::string IRGen::fieldPtr(const std::string& objIR, int slot) {
    std::string ptr = emitGep("i64", objIR, "i64", std::to_string(slot));
    return ptr;
}

bool IRGen::isGcPointerType(const TypePtr& t) {
    return t && (t->isReferenceType() || t->isBoxedNullable());
}

int64_t IRGen::objectPtrBitmap(const ClassInfo* ci) {
    int64_t bm = 0;
    if (!ci) return 0;
    // 与 runtime_object：nfields>32 → FULL；位图仅低 32 槽有效
    for (const auto& f : ci->fields) {
        if (f.slot >= 0 && f.slot < 32 && isGcPointerType(f.type))
            bm |= (int64_t(1) << f.slot);
    }
    return bm;
}

void IRGen::emitHeapStore(const std::string& addr, const std::string& valIr,
                          const TypePtr& ty, const std::string& barrierBase) {
    std::string lt = ty ? ty->llvmType() : "ptr";
    /* v0.54：混合屏障；dst 必须是槽地址（addr），非对象基址 */
    if (isGcPointerType(ty) && !barrierBase.empty())
        ops_.emitCallVoid("@hao_gc_barrier", "ptr " + addr + ", ptr " + valIr);
    ops_.emitStore(lt, valIr, addr);
}

void IRGen::emitGlobalGcStore(const std::string& gptr, const std::string& valIr,
                              const TypePtr& ty) {
    std::string lt = ty ? ty->llvmType() : "ptr";
    /* 静态槽同样走混合屏障（gptr 即槽地址） */
    if (isGcPointerType(ty))
        ops_.emitCallVoid("@hao_gc_barrier", "ptr " + gptr + ", ptr " + valIr);
    ops_.emitStore(lt, valIr, gptr);
}

void IRGen::beginFunctionGcRoots() {
    gcRootWm_ = ops_.emitCall("i64", "@hao_gc_root_watermark", "");
}

void IRGen::emitGcRootPush(const std::string& slotAddr) {
    if (slotAddr.empty()) return;
    ops_.emitCallVoid("@hao_gc_root_push", "ptr " + slotAddr);
}

void IRGen::emitGcRootUnwind() {
    if (gcRootWm_.empty()) return;
    ops_.emitCallVoid("@hao_gc_root_unwind", "i64 " + gcRootWm_);
}

void IRGen::emitSafepoint() {
    ops_.emitCallVoid("@hao_gc_safepoint", "");
}

void IRGen::emitAllocUnwindSlots() {
    unwindReasonAddr_ = "%unwind.reason.addr";
    unwindRetAddr_    = "%unwind.ret.addr";
    unwindStopAddr_   = "%unwind.stop.addr";
    unwindGcRootAddr_ = "%unwind.gc.addr";
    emitAllocaAt(unwindReasonAddr_, "i32");
    emitAllocaAt(unwindRetAddr_, "i64");
    emitAllocaAt(unwindStopAddr_, "i32");
    emitAllocaAt(unwindGcRootAddr_, "ptr");
    emitStore("i32", "0", unwindReasonAddr_);
    emitStore("ptr", "null", unwindGcRootAddr_);
}

void IRGen::emitPushUnwindGcRoot() {
    if (!unwindGcRootAddr_.empty())
        emitGcRootPush(unwindGcRootAddr_);
}

void IRGen::storeUnwindGcRootPtr(const std::string& ptrIr) {
    if (unwindGcRootAddr_.empty()) return;
    emitStore("ptr", ptrIr, unwindGcRootAddr_);
}

void IRGen::clearUnwindGcRoot() {
    storeUnwindGcRootPtr("null");
}

void IRGen::beginBlockGcScope() {
    /* A11：正路径可观测（默认关） */
    {
        const char* tr = getenv("HAO_IRGEN_TRACE");
        if (tr && tr[0] && tr[0] != '0') {
            fprintf(stderr, "hao:irgen:block_enter depth=%zu\n",
                    blockGcSlots_.size());
            fflush(stderr);
        }
    }
    blockGcSlots_.push_back({});
}

void IRGen::endBlockGcScope() {
    if (blockGcSlots_.empty()) return;
    /* A11：正路径可观测（默认关） */
    {
        const char* tr = getenv("HAO_IRGEN_TRACE");
        if (tr && tr[0] && tr[0] != '0') {
            fprintf(stderr, "hao:irgen:block_leave depth=%zu slots=%zu\n",
                    blockGcSlots_.size(), blockGcSlots_.back().size());
            fflush(stderr);
        }
    }
    if (!blockTerminated_) {
        for (const auto& addr : blockGcSlots_.back())
            emitStore("ptr", "null", addr);
    }
    blockGcSlots_.pop_back();
}

void IRGen::noteBlockGcSlot(const std::string& slotAddr) {
    if (slotAddr.empty() || blockGcSlots_.empty()) return;
    blockGcSlots_.back().push_back(slotAddr);
}

/* 循环入口预分配槽数：须在循环前（支配整段循环），禁止在 body 内 alloca */
static constexpr size_t kLoopSpillPoolPrealloc = 48;

void IRGen::enterLoopSpillScope() {
    if (loopSpillDepth_ == 0) {
        LoopSpillPool pool;
        /* 在 while/for 之前发射：支配 body/step/end 上所有 store null */
        for (size_t i = 0; i < kLoopSpillPoolPrealloc; ++i) {
            std::string addr = emitAllocaNamed("loop.spill", "ptr");
            emitStore("ptr", "null", addr);
            emitGcRootPush(addr);
            pool.slots.push_back(addr);
        }
        pool.next = 0;
        pool.highWater = 0;
        loopSpillPools_.push_back(std::move(pool));
    }
    if (!loopSpillPools_.empty()) {
        auto& pool = loopSpillPools_.back();
        /* A10：正路径可观测（默认关） */
        {
            const char* tr = getenv("HAO_IRGEN_TRACE");
            if (tr && tr[0] && tr[0] != '0') {
                fprintf(stderr,
                        "hao:irgen:enter_spill depth=%d next=%zu slots=%zu\n",
                        loopSpillDepth_, pool.next, pool.slots.size());
                fflush(stderr);
            }
        }
        pool.scopeStack.push_back(pool.next);
        size_t enterSticky = pool.stickyStack.size();
        pool.stickyEnterStack.push_back(enterSticky);
        /* 占位；while 在 hoist pin 后 markLoopSpillStickyFloor 覆写 */
        pool.stickyFloorStack.push_back(enterSticky);
    }
    ++loopSpillDepth_;
}

void IRGen::leaveLoopSpillScope() {
    if (loopSpillDepth_ <= 0) return;
    if (!loopSpillPools_.empty()) {
        auto& pool = loopSpillPools_.back();
        if (!pool.scopeStack.empty()) {
            size_t base = pool.scopeStack.back();
            /* A9：正路径可观测（默认关） */
            {
                const char* tr = getenv("HAO_IRGEN_TRACE");
                if (tr && tr[0] && tr[0] != '0') {
                    fprintf(stderr,
                            "hao:irgen:leave_spill base=%zu next=%zu high=%zu\n",
                            base, pool.next, pool.highWater);
                    fflush(stderr);
                }
            }
            size_t lim = pool.highWater > pool.next ? pool.highWater : pool.next;
            if (lim > pool.slots.size()) lim = pool.slots.size();
            for (size_t i = base; i < lim; ++i)
                emitStore("ptr", "null", pool.slots[i]);
            pool.next = base;
            if (pool.highWater > base) pool.highWater = base;
            pool.scopeStack.pop_back();
            /* 只卸本层 sticky；按 enter 时深度回退，禁止 >=base 误剥外层 */
            size_t enterSticky = pool.stickyEnterStack.empty()
                ? 0
                : pool.stickyEnterStack.back();
            while (pool.stickyStack.size() > enterSticky)
                pool.stickyStack.pop_back();
            if (!pool.stickyEnterStack.empty())
                pool.stickyEnterStack.pop_back();
            if (!pool.stickyFloorStack.empty())
                pool.stickyFloorStack.pop_back();
        }
    }
    --loopSpillDepth_;
    if (loopSpillDepth_ == 0 && !loopSpillPools_.empty())
        loopSpillPools_.pop_back();
}

void IRGen::pinLoopSpillCheckpoint() {
    if (loopSpillPools_.empty()) return;
    auto& pool = loopSpillPools_.back();
    /* A10：正路径可观测（默认关） */
    {
        const char* tr = getenv("HAO_IRGEN_TRACE");
        if (tr && tr[0] && tr[0] != '0') {
            fprintf(stderr,
                    "hao:irgen:pin_spill next=%zu sticky_n=%zu\n",
                    pool.next, pool.stickyStack.size());
            fflush(stderr);
        }
    }
    pool.stickyStack.push_back(pool.next);
}

void IRGen::markLoopSpillStickyFloor() {
    if (loopSpillPools_.empty()) return;
    auto& pool = loopSpillPools_.back();
    if (pool.stickyFloorStack.empty()) return;
    /* A11：正路径可观测（默认关） */
    {
        const char* tr = getenv("HAO_IRGEN_TRACE");
        if (tr && tr[0] && tr[0] != '0') {
            fprintf(stderr,
                    "hao:irgen:sticky_floor sticky_n=%zu next=%zu\n",
                    pool.stickyStack.size(), pool.next);
            fflush(stderr);
        }
    }
    pool.stickyFloorStack.back() = pool.stickyStack.size();
}

void IRGen::clearLoopSpillSlots() {
    if (loopSpillPools_.empty()) return;
    auto& pool = loopSpillPools_.back();
    /* 只清本层作用域；sticky（条件根 / for.seq）跨迭代持有，禁止抹掉或 pop */
    size_t base = pool.scopeStack.empty() ? 0 : pool.scopeStack.back();
    size_t target = base;
    if (!pool.stickyStack.empty() && pool.stickyStack.back() > target)
        target = pool.stickyStack.back();
    size_t lim = pool.highWater > pool.next ? pool.highWater : pool.next;
    if (lim > pool.slots.size()) lim = pool.slots.size();
    /* A6：正路径可观测（默认关）；固定前缀便于门禁匹配 */
    {
        const char* tr = getenv("HAO_IRGEN_TRACE");
        if (tr && tr[0] && tr[0] != '0') {
            fprintf(stderr,
                    "hao:irgen:clear_spill base=%zu target=%zu next=%zu\n",
                    base, target, pool.next);
            fflush(stderr);
        }
    }
    /* A5：清到本层 base 以下 = 嵌套 sticky 分层仍坏
     * 始终打固定前缀；HAO_IRGEN_STRICT=1 时记诊断（拒绝 emit） */
    if (target < base) {
        fprintf(stderr,
                "hao:irgen:clear_spill_underflow target=%zu < base=%zu "
                "(nested sticky floor bug?)\n",
                target, base);
        fflush(stderr);
        const char* tr = getenv("HAO_IRGEN_TRACE");
        if (tr && tr[0] && tr[0] != '0') {
            fprintf(stderr,
                    "[hao:irgen] clearLoopSpill target=%zu < base=%zu "
                    "(nested sticky floor bug?)\n",
                    target, base);
        }
        const char* st = getenv("HAO_IRGEN_STRICT");
        int strict = 0;
        if (st && st[0] && !(st[0] == '0' && st[1] == '\0') &&
            !((st[0] == 'n' || st[0] == 'N') &&
              (st[1] == 'o' || st[1] == 'O') && st[2] == '\0') &&
            !((st[0] == 'f' || st[0] == 'F') &&
              (st[1] == 'a' || st[1] == 'A') &&
              (st[2] == 'l' || st[2] == 'L') &&
              (st[3] == 's' || st[3] == 'S') &&
              (st[4] == 'e' || st[4] == 'E') && st[5] == '\0'))
            strict = 1;
        if (strict) {
            diags_.error(0, 0,
                         "hao:irgen:clear_spill_underflow "
                         "(HAO_IRGEN_STRICT=1)");
        }
    }
    for (size_t i = target; i < lim; ++i)
        emitStore("ptr", "null", pool.slots[i]);
    pool.next = target;
    if (pool.highWater > target) pool.highWater = target;
}

void IRGen::recycleLoopSpillSlots() {
    if (loopSpillPools_.empty()) return;
    auto& pool = loopSpillPools_.back();
    size_t target = 0;
    if (!pool.stickyStack.empty())
        target = pool.stickyStack.back();
    else if (!pool.scopeStack.empty())
        target = pool.scopeStack.back();
    /* A8：正路径可观测（默认关） */
    {
        const char* tr = getenv("HAO_IRGEN_TRACE");
        if (tr && tr[0] && tr[0] != '0') {
            fprintf(stderr,
                    "hao:irgen:recycle_spill target=%zu next=%zu high=%zu\n",
                    target, pool.next, pool.highWater);
            fflush(stderr);
        }
    }
    /* 回退游标前须清 null，否则上轮 spill 残留在已 push 的槽里假活 */
    size_t lim = pool.highWater > pool.next ? pool.highWater : pool.next;
    if (lim > pool.slots.size()) lim = pool.slots.size();
    for (size_t i = target; i < lim; ++i)
        emitStore("ptr", "null", pool.slots[i]);
    pool.next = target;
    if (pool.highWater > target) pool.highWater = target;
}

void IRGen::unpinLoopSpillCheckpoint() {
    if (loopSpillPools_.empty()) return;
    auto& pool = loopSpillPools_.back();
    if (pool.stickyStack.empty()) return;
    size_t sticky = pool.stickyStack.back();
    /* A9：正路径可观测（默认关） */
    {
        const char* tr = getenv("HAO_IRGEN_TRACE");
        if (tr && tr[0] && tr[0] != '0') {
            fprintf(stderr,
                    "hao:irgen:unpin_spill sticky=%zu next=%zu\n",
                    sticky, pool.next);
            fflush(stderr);
        }
    }
    size_t lim = pool.next > sticky ? pool.next : sticky;
    for (size_t i = sticky; i < lim && i < pool.slots.size(); ++i)
        emitStore("ptr", "null", pool.slots[i]);
    pool.next = sticky;
    pool.stickyStack.pop_back();
}

std::string IRGen::acquireLoopGcSlot(const std::string& nameHint) {
    if (loopSpillPools_.empty()) {
        std::string addr = emitAllocaNamed(nameHint, "ptr");
        emitStore("ptr", "null", addr);
        emitGcRootPush(addr);
        return addr;
    }
    auto& pool = loopSpillPools_.back();
    /* A7：正路径可观测（默认关） */
    {
        const char* tr = getenv("HAO_IRGEN_TRACE");
        if (tr && tr[0] && tr[0] != '0') {
            fprintf(stderr,
                    "hao:irgen:acquire_spill next=%zu slots=%zu hint=%s\n",
                    pool.next, pool.slots.size(), nameHint.c_str());
            fflush(stderr);
        }
    }
    if (pool.next >= pool.slots.size()) {
        /* 扩池：alloca 进 entry（支配），再 root_push 一次并入可复用表 */
        std::string addr = em_.emitEntryAllocaPtr(nameHint);
        emitStore("ptr", "null", addr);
        emitGcRootPush(addr);
        pool.slots.push_back(addr);
    }
    std::string addr = pool.slots[pool.next++];
    if (pool.next > pool.highWater) pool.highWater = pool.next;
    return addr;
}

std::string IRGen::emitSpillGcRoot(const std::string& nameHint, const std::string& ptrIr) {
    if (!loopSpillPools_.empty()) {
        std::string addr = acquireLoopGcSlot(nameHint);
        emitStore("ptr", ptrIr, addr);
        /* 池寿命由 clear/recycle/leave 管；勿 noteBlock——内层块尾会误杀仍在用的池槽 */
        return addr;
    }
    /* alloca 必须进 entry，否则分支内 spill 的 use 不支配（套件 clang 失败） */
    std::string addr = em_.emitEntryAllocaPtr(nameHint);
    emitStore("ptr", ptrIr, addr);
    emitGcRootPush(addr);
    /* 不 noteBlock/Expr：表达式临时寿命到函数 unwind；语句尾清曾误杀
     * new/构造期仍被 SSA 使用的槽（G1 债：需更精的寿命分析） */
    return addr;
}

void IRGen::rootGcOperand(Value& v) {
    /* 静态 ClassName.f：recv.ir 为空，勿 spill */
    if (!v.valid() || v.ir.empty() || !isGcPointerType(v.type)) return;
    std::string slot = emitSpillGcRoot("op.root", v.ir);
    v.ir = emitLoad("ptr", slot);
}

void IRGen::emitVarStore(const SymbolPtr& sym, const TypePtr& ty,
                         const std::string& valIr) {
    std::string addr = varValuePtr(sym);
    // boxed 可变捕获；by-ref 数组形参可能写回堆字段（须写屏障）
    if (sym->boxed || (sym->byRefParam && isGcPointerType(ty)))
        emitHeapStore(addr, valIr, ty, addr);
    else
        emitStore(ty->llvmType(), valIr, addr);
}

std::string IRGen::emitObjectNew(int64_t nfields, int64_t bitmap) {
    return emitCall("ptr", "@hao_object_new",
                    "i64 " + std::to_string(nfields) + ", i64 " +
                        std::to_string(bitmap));
}

// 将数组下标提升为 i64（语言侧 Int=i32）
std::string IRGen::indexAsI64(const Value& idx) {
    if (idx.type && (idx.type->kind == TypeKind::Long ||
                     idx.type->kind == TypeKind::ULong ||
                     idx.type->kind == TypeKind::UIntPtr))
        return idx.ir;
    if (idx.type && (idx.type->kind == TypeKind::Int ||
                     idx.type->kind == TypeKind::UInt)) {
        std::string r = em_.nextTemp();
        // 下标非负语义：无符号 zext，有符号 sext
        if (idx.type->isUnsigned())
            r = emitCast("zext", "i32", idx.ir, "i64");
        else
            r = emitCast("sext", "i32", idx.ir, "i64");
        return r;
    }
    // 窄整数等：先升 Int 再 sext
    Value asInt = coerce(idx, Type::makeInt(), 0, 0);
    std::string r = emitCast("sext", "i32", asInt.ir, "i64");
    return r;
}

// 计算数组元素地址，带运行时边界检查
std::string IRGen::arrayElemPtr(const Value& arr, const Value& idx) {
    std::string idx64 = indexAsI64(idx);
    std::string checked = emitCall("i64", "@hao_array_check",
                                   "ptr " + arr.ir + ", i64 " + idx64);
    std::string gepTy = "i64";
    if (arr.type && arr.type->elem)
        gepTy = arr.type->elem->arrayGepType();
    std::string ptr = emitGep(gepTy, arr.ir, "i64", checked);
    return ptr;
}

// ============================================================
//  纯类型推断（不发射指令）
// ------------------------------------------------------------
//  when 表达式需要在生成任何分支代码之前确定结果类型，
//  以便把结果暂存变量的 alloca 放在入口块。
//  这里只覆盖常见形态；无法判定时返回 Unknown，由调用方报错。
// ============================================================
TypePtr IRGen::inferExprType(HaoLangParser::ExprContext* e) {
    if (!e) return Type::makeUnknown();
    return inferNodeType(e);
}

TypePtr IRGen::inferNodeType(antlr4::tree::ParseTree* start) {
    // 沿单一子节点链下降。每一层都要检查是否命中已知节点类型，
    // 因为链条最终会走到 TerminalNode，若只在末端判断会越过
    // LiteralContext / IdentPrimaryContext 等真正携带类型信息的节点。
    antlr4::tree::ParseTree* node = start;
    while (node) {
        // ---- 字面量 ----
        if (auto* lit = dynamic_cast<HaoLangParser::LiteralContext*>(node)) {
            if (lit->INT_LIT())   return Type::makeInt();
            if (lit->FLOAT_LIT()) return Type::makeDouble();
            if (lit->CHAR_LIT())  return Type::makeChar();
            if (lit->STRING_LIT() || lit->VERBATIM_STRING() || lit->templateString())
                return Type::makeString();
            if (lit->TRUE() || lit->FALSE()) return Type::makeBool();
            if (lit->NULL_LIT()) return Type::makeNull();
            return Type::makeUnknown();
        }

        // ---- 标识符：查符号表 ----
        if (auto* ip = dynamic_cast<HaoLangParser::IdentPrimaryContext*>(node)) {
            auto sym = syms_.lookup(ip->IDENT()->getText());
            if (!sym || !sym->type) return Type::makeUnknown();
            if (auto nt = lookupSmartCast(sym->name)) return nt;
            return sym->type;
        }

        // ---- 括号：对内部表达式递归 ----
        if (auto* pp = dynamic_cast<HaoLangParser::ParenPrimaryContext*>(node))
            return inferExprType(pp->expr());

        // ---- lambda：推断函数类型 ----
        if (auto* lamP = dynamic_cast<HaoLangParser::LambdaPrimaryContext*>(node))
            return inferLambdaType(lamP->lambda());
        if (auto* lam = dynamic_cast<HaoLangParser::LambdaContext*>(node))
            return inferLambdaType(lam);

        // ---- 嵌套 when 表达式 ----
        if (auto* wp = dynamic_cast<HaoLangParser::WhenPrimaryContext*>(node)) {
            // 取其第一个分支体的类型
            for (auto* b : wp->whenStmt()->whenBranch())
                if (b->expr()) return inferExprType(b->expr());
            return Type::makeUnknown();
        }

        // ---- 数组字面量（含 ... 展开）----
        if (auto* ap = dynamic_cast<HaoLangParser::ArrayPrimaryContext*>(node)) {
            auto* list = ap->arrayLiteral()->arrayElementList();
            if (!list || list->arrayElement().empty())
                return Type::makeArray(Type::makeInt());
            for (auto* ae : list->arrayElement()) {
                TypePtr t = inferExprType(ae->expr());
                if (!t) continue;
                if (ae->ELLIPSIS()) {
                    if (t->kind == TypeKind::Array && t->elem)
                        return Type::makeArray(t->elem);
                    continue;
                }
                return Type::makeArray(t);
            }
            return Type::makeArray(Type::makeInt());
        }

        // ---- new 表达式（含 new [T](n[, fill])）----
        if (auto* np = dynamic_cast<HaoLangParser::NewPrimaryContext*>(node))
            return resolveType(np->type());

        // ---- this ----
        if (dynamic_cast<HaoLangParser::ThisPrimaryContext*>(node)) {
            if (currentClass_) return Type::makeClass(currentClass_->name);
            return Type::makeUnknown();
        }

        // ---- 比较与逻辑运算：结果恒为 Bool（须在下降前判断分支数）----
        if (auto* eq = dynamic_cast<HaoLangParser::EqualityExprContext*>(node))
            if (eq->relationalExpr().size() > 1) return Type::makeBool();
        if (auto* rel = dynamic_cast<HaoLangParser::RelationalExprContext*>(node)) {
            // as 的结果是目标类型；is 与其他比较运算的结果是 Bool
            if (rel->AS()) return resolveType(rel->type());
            if (rel->shiftExpr().size() > 1 || rel->IS())
                return Type::makeBool();
        }
        if (auto* an = dynamic_cast<HaoLangParser::AndExprContext*>(node))
            if (an->bitOrExpr().size() > 1) return Type::makeBool();
        if (auto* orr = dynamic_cast<HaoLangParser::OrExprContext*>(node))
            if (orr->andExpr().size() > 1) return Type::makeBool();

        // ---- 位运算：binaryBitwisePromote；移位：仅左操作数一元提升（对齐 Java）----
        auto inferBitwiseResult = [&](const std::vector<antlr4::tree::ParseTree*>& parts) -> TypePtr {
            TypePtr acc;
            for (auto* p : parts) {
                TypePtr t = inferNodeType(p);
                if (!t || !t->isInteger()) continue;
                if (!acc) acc = t;
                else {
                    if (Type::isMixedSignedUnsigned64(acc->kind, t->kind))
                        return Type::makeUnknown();
                    auto pmt = Type::binaryBitwisePromote(acc->kind, t->kind);
                    if (pmt) acc = pmt;
                }
            }
            return acc ? acc : Type::makeInt();
        };
        if (auto* bo = dynamic_cast<HaoLangParser::BitOrExprContext*>(node))
            if (bo->bitXorExpr().size() > 1) {
                std::vector<antlr4::tree::ParseTree*> parts;
                for (auto* x : bo->bitXorExpr()) parts.push_back(x);
                return inferBitwiseResult(parts);
            }
        if (auto* bx = dynamic_cast<HaoLangParser::BitXorExprContext*>(node))
            if (bx->bitAndExpr().size() > 1) {
                std::vector<antlr4::tree::ParseTree*> parts;
                for (auto* x : bx->bitAndExpr()) parts.push_back(x);
                return inferBitwiseResult(parts);
            }
        if (auto* ba = dynamic_cast<HaoLangParser::BitAndExprContext*>(node))
            if (ba->equalityExpr().size() > 1) {
                std::vector<antlr4::tree::ParseTree*> parts;
                for (auto* x : ba->equalityExpr()) parts.push_back(x);
                return inferBitwiseResult(parts);
            }
        if (auto* sh = dynamic_cast<HaoLangParser::ShiftExprContext*>(node))
            if (sh->additiveExpr().size() > 1) {
                // 移位结果类型 = 左操作数一元提升
                TypePtr lt = inferNodeType(sh->additiveExpr(0));
                if (lt && lt->isInteger()) {
                    auto p = Type::unaryBitwisePromote(lt->kind);
                    return p ? p : Type::makeInt();
                }
                return Type::makeInt();
            }

        // ---- 加减：字符串拼接 / Java 数值提升阶梯 ----
        if (auto* add = dynamic_cast<HaoLangParser::AdditiveExprContext*>(node)) {
            auto muls = add->multiplicativeExpr();
            if (muls.size() > 1) {
                bool anyString = false;
                int bestRank = Type::numericRank(TypeKind::Int);
                for (auto* m : muls) {
                    TypePtr t = inferNodeType(m);
                    if (!t) continue;
                    if (t->kind == TypeKind::String) anyString = true;
                    int r = Type::numericRank(t->kind);
                    if (r > bestRank) bestRank = r;
                }
                if (anyString) return Type::makeString();
                if (bestRank >= 6) return Type::makeDouble();
                if (bestRank >= 5) return Type::makeFloat();
                if (bestRank >= 4) return Type::makeLong();
                return Type::makeInt();
            }
        }

        // ---- 乘除模：同数值提升 ----
        if (auto* mul = dynamic_cast<HaoLangParser::MultiplicativeExprContext*>(node)) {
            auto uns = mul->unaryExpr();
            if (uns.size() > 1) {
                int bestRank = Type::numericRank(TypeKind::Int);
                for (auto* u : uns) {
                    TypePtr t = inferNodeType(u);
                    if (!t) continue;
                    int r = Type::numericRank(t->kind);
                    if (r > bestRank) bestRank = r;
                }
                if (bestRank >= 6) return Type::makeDouble();
                if (bestRank >= 5) return Type::makeFloat();
                if (bestRank >= 4) return Type::makeLong();
                return Type::makeInt();
            }
        }

        // ---- 后缀：函数调用 / 方法调用 / .length / 字段 / 索引 ----
        if (auto* pf = dynamic_cast<HaoLangParser::PostfixExprContext*>(node)) {
            auto ops = pf->postfixOp();
            if (ops.size() == 1) {
                if (dynamic_cast<HaoLangParser::CallOpContext*>(ops[0])) {
                    if (auto* idp =
                            dynamic_cast<HaoLangParser::IdentPrimaryContext*>(pf->primary())) {
                        auto sym = syms_.lookup(idp->IDENT()->getText());
                        if (sym && sym->kind == SymbolKind::Function && sym->returnType)
                            return sym->returnType;
                    }
                    return Type::makeUnknown();
                }
                if (auto* mem = dynamic_cast<HaoLangParser::MemberAccessContext*>(ops[0])) {
                    std::string fname = mem->IDENT()->getText();
                    if (fname == "length") return Type::makeInt();
                    // 单字段：obj.field → 字段类型（供 when 推断）
                    TypePtr recvTy = inferNodeType(pf->primary());
                    if (recvTy && recvTy->kind == TypeKind::Class) {
                        auto ci = classOfType(recvTy);
                        if (ci) {
                            if (const FieldInfo* fi = ci->findField(fname))
                                return fi->type;
                            if (const FieldInfo* sfi = ci->findStaticField(fname))
                                return sfi->type;
                        }
                    }
                    return Type::makeUnknown();
                }
                if (dynamic_cast<HaoLangParser::IndexOpContext*>(ops[0])) {
                    if (auto* idp =
                            dynamic_cast<HaoLangParser::IdentPrimaryContext*>(pf->primary())) {
                        auto sym = syms_.lookup(idp->IDENT()->getText());
                        if (sym && sym->type && sym->type->kind == TypeKind::Array)
                            return sym->type->elem ? sym->type->elem : Type::makeInt();
                    }
                    return Type::makeUnknown();
                }
            }
            // obj.m(...) / pkg.fn(...) / Class.static(...)：查真实返回类型
            // （旧逻辑一律 Unit 会把 c.get() 误判，导致 when alloca void）
            if (ops.size() >= 2) {
                auto* call = dynamic_cast<HaoLangParser::CallOpContext*>(ops.back());
                auto* mem = (ops.size() >= 2)
                    ? dynamic_cast<HaoLangParser::MemberAccessContext*>(ops[ops.size() - 2])
                    : nullptr;
                if (call && mem) {
                    std::string mname = mem->IDENT()->getText();
                    // pkg.fn(args)：仅 MemberAccess+CallOp 且 primary 为导入包别名
                    if (ops.size() == 2) {
                        if (auto* idp = dynamic_cast<HaoLangParser::IdentPrimaryContext*>(
                                pf->primary())) {
                            std::string alias = idp->IDENT()->getText();
                            bool isImportedPkg = false;
                            for (const auto& im : currentImports_)
                                if (im.alias == alias && !im.wildcard) {
                                    isImportedPkg = true;
                                    break;
                                }
                            if (isImportedPkg) {
                                auto sym = resolveQualifiedName(alias, mname, nullptr);
                                if (sym && sym->kind == SymbolKind::Function &&
                                    sym->returnType)
                                    return sym->returnType;
                                if (sym && sym->kind == SymbolKind::Class && sym->classInfo) {
                                    // pkg.Type.m — 需要更多后缀，此处不够
                                }
                            }
                        }
                    }
                    // 接收者 = primary + 去掉末尾 MemberAccess/CallOp 的前缀
                    // 简化：仅处理 ops==2（recv.m()）与 ops==3 且中间为 !!（recv!!.m()）
                    TypePtr recvTy;
                    if (ops.size() == 2) {
                        recvTy = inferNodeType(pf->primary());
                    } else if (ops.size() == 3 &&
                               dynamic_cast<HaoLangParser::NotNullAssertContext*>(ops[0])) {
                        recvTy = inferNodeType(pf->primary());
                        if (recvTy && recvTy->nullable) {
                            auto nn = std::make_shared<Type>(*recvTy);
                            nn->nullable = false;
                            recvTy = nn;
                        }
                    }
                    if (recvTy && recvTy->kind == TypeKind::Class) {
                        auto ci = classOfType(recvTy);
                        if (ci) {
                            if (const MethodInfo* mi = ci->findMethod(mname))
                                if (mi->returnType) return mi->returnType;
                            auto scands = ci->findStaticMethods(mname);
                            if (!scands.empty() && scands[0]->returnType)
                                return scands[0]->returnType;
                        }
                    }
                    if (recvTy && recvTy->kind == TypeKind::Interface) {
                        auto it = interfaces_.find(recvTy->className);
                        if (it != interfaces_.end() && it->second) {
                            if (const MethodInfo* mi = it->second->findMethod(mname))
                                if (mi->returnType) return mi->returnType;
                        }
                    }
                }
                return Type::makeUnknown();
            }
        }

        // ---- 一元 ----
        if (auto* un = dynamic_cast<HaoLangParser::UnaryExprContext*>(node)) {
            if (!un->postfixExpr()) {
                std::string op = un->children[0]->getText();
                if (op == "!") return Type::makeBool();
                // -x / +x 保持操作数类型
                return inferNodeType(un->unaryExpr());
            }
        }

        // 继续向下
        if (node->children.size() == 1) {
            node = node->children[0];
            continue;
        }
        break;
    }

    return Type::makeUnknown();
}


// ============================================================
//  表达式分发
// ============================================================


} // namespace hao
