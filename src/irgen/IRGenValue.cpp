// ============================================================
//  HaoLang IR 生成 —— 值转换与类型推断辅助
// ------------------------------------------------------------
//  从 IRGenExpr.cpp 拆分而来，逻辑保持不变。这些是表达式生成与其它
//  模块共享的底层工具：变量装载、类型强制、Bool/字符串转换、字段/
//  数组元素寻址，以及纯类型推断（不发射指令，供 when 表达式、lambda
//  返回类型推断等使用）。
// ============================================================

#include "irgen/IRGen.h"

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
        std::string reg = em_.nextTemp();
        em_.emit(reg + " = call i64 @hao_unbox_i64(ptr " + base.ir + ")");
        return Value(reg, nonNull);
    };
    auto unboxI32 = [&]() {
        std::string reg = em_.nextTemp();
        em_.emit(reg + " = call i32 @hao_unbox_i32(ptr " + base.ir + ")");
        return Value(reg, nonNull);
    };
    auto unboxNarrow = [&](const std::string& toTy) {
        std::string wide = em_.nextTemp();
        em_.emit(wide + " = call i32 @hao_unbox_i32(ptr " + base.ir + ")");
        std::string reg = em_.nextTemp();
        em_.emit(reg + " = trunc i32 " + wide + " to " + toTy);
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
            std::string reg = em_.nextTemp();
            em_.emit(reg + " = call double @hao_unbox_f64(ptr " + base.ir + ")");
            return Value(reg, nonNull);
        }
        case TypeKind::Float: {
            std::string reg = em_.nextTemp();
            em_.emit(reg + " = call float @hao_unbox_f32(ptr " + base.ir + ")");
            return Value(reg, nonNull);
        }
        default:
            return unboxI32();
    }
}

Value IRGen::loadVar(const SymbolPtr& sym) {
    std::string ptr = varValuePtr(sym);
    std::string reg = em_.nextTemp();
    em_.emit(reg + " = load " + sym->type->llvmType() + ", ptr " + ptr);
    Value v(reg, sym->type);
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
    std::string cell = em_.nextTemp();
    em_.emit(cell + " = load ptr, ptr " + sym->irAddr);
    return cell;
}

std::string IRGen::arrayArgSlot(antlr4::tree::ParseTree* expr,
                                const Value& arrVal) {
    auto makeTempSlot = [&]() -> std::string {
        std::string addr = em_.nextTemp();
        em_.emit(addr + " = alloca ptr");
        em_.emit("store ptr " + arrVal.ir + ", ptr " + addr);
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
    arg = coerce(arg, paramTy, 0, 0);
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

        auto emit = [&](const std::string& insn) {
            std::string reg = em_.nextTemp();
            em_.emit(reg + " = " + insn);
            return reg;
        };

        // 整数互转：按位宽；无符号拓宽用 zext，有符号用 sext；同宽仅改类型标签
        if (v.type->isInteger() && target->isInteger()) {
            int fw = Type::bitWidthBits(from), tw = Type::bitWidthBits(to);
            if (fw < tw) {
                std::string op = v.type->isUnsigned() ? "zext" : "sext";
                return Value(emit(op + " " + fromTy + " " + src + " to " +
                                  target->llvmType()), target);
            }
            if (fw > tw) {
                return Value(emit("trunc " + fromTy + " " + src + " to " +
                                  target->llvmType()), target);
            }
            // 同宽有符号/无符号：位模式不变，只改类型
            return Value(src, target);
        }

        // 整数 → 浮点
        if (v.type->isInteger() && target->isFloating()) {
            std::string op = v.type->isUnsigned() ? "uitofp" : "sitofp";
            return Value(emit(op + " " + fromTy + " " + src + " to " +
                              target->llvmType()), target);
        }

        if (from == TypeKind::Float && to == TypeKind::Double) {
            return Value(emit("fpext float " + src + " to double"), target);
        }
        if (from == TypeKind::Double && to == TypeKind::Float) {
            return Value(emit("fptrunc double " + src + " to float"), target);
        }

        if (v.type->isFloating() && target->isInteger()) {
            std::string op = target->isUnsigned() ? "fptoui" : "fptosi";
            return Value(emit(op + " " + fromTy + " " + src + " to " +
                              target->llvmType()), target);
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

    // T -> T?：值类型装箱为指针
    if (target->nullable && !v.type->nullable &&
        v.type->kind == target->kind && v.type->sameShape(*target)) {
        if (v.type->kind == TypeKind::Long || v.type->kind == TypeKind::ULong ||
            v.type->kind == TypeKind::UIntPtr) {
            std::string reg = em_.nextTemp();
            em_.emit(reg + " = call ptr @hao_box_i64(i64 " + v.ir + ")");
            return Value(reg, target);
        }
        if (v.type->kind == TypeKind::Int || v.type->kind == TypeKind::UInt ||
            v.type->kind == TypeKind::Char) {
            std::string reg = em_.nextTemp();
            em_.emit(reg + " = call ptr @hao_box_i32(i32 " + v.ir + ")");
            return Value(reg, target);
        }
        if (v.type->kind == TypeKind::Bool) {
            std::string wide = em_.nextTemp();
            em_.emit(wide + " = zext i8 " + v.ir + " to i32");
            std::string reg = em_.nextTemp();
            em_.emit(reg + " = call ptr @hao_box_i32(i32 " + wide + ")");
            return Value(reg, target);
        }
        if (v.type->kind == TypeKind::Short) {
            std::string wide = em_.nextTemp();
            em_.emit(wide + " = sext i16 " + v.ir + " to i32");
            std::string reg = em_.nextTemp();
            em_.emit(reg + " = call ptr @hao_box_i32(i32 " + wide + ")");
            return Value(reg, target);
        }
        if (v.type->kind == TypeKind::UShort) {
            std::string wide = em_.nextTemp();
            em_.emit(wide + " = zext i16 " + v.ir + " to i32");
            std::string reg = em_.nextTemp();
            em_.emit(reg + " = call ptr @hao_box_i32(i32 " + wide + ")");
            return Value(reg, target);
        }
        if (v.type->kind == TypeKind::SByte) {
            std::string wide = em_.nextTemp();
            em_.emit(wide + " = sext i8 " + v.ir + " to i32");
            std::string reg = em_.nextTemp();
            em_.emit(reg + " = call ptr @hao_box_i32(i32 " + wide + ")");
            return Value(reg, target);
        }
        if (v.type->kind == TypeKind::Byte) {
            std::string wide = em_.nextTemp();
            em_.emit(wide + " = zext i8 " + v.ir + " to i32");
            std::string reg = em_.nextTemp();
            em_.emit(reg + " = call ptr @hao_box_i32(i32 " + wide + ")");
            return Value(reg, target);
        }
        if (v.type->kind == TypeKind::Double) {
            std::string reg = em_.nextTemp();
            em_.emit(reg + " = call ptr @hao_box_f64(double " + v.ir + ")");
            return Value(reg, target);
        }
        if (v.type->kind == TypeKind::Float) {
            std::string reg = em_.nextTemp();
            em_.emit(reg + " = call ptr @hao_box_f32(float " + v.ir + ")");
            return Value(reg, target);
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
    std::string isZero = em_.nextTemp();
    em_.emit(isZero + " = icmp eq " + divisor.type->llvmType() + " " +
             divisor.ir + ", 0");
    std::string panicL = em_.nextLabel("div0.panic");
    std::string okL = em_.nextLabel("div0.ok");
    em_.emit("br i1 " + isZero + ", label %" + panicL + ", label %" + okL);
    em_.emitLabel(panicL);
    em_.emit("call void @hao_panic_div_zero()");
    em_.emit("unreachable");
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
    std::string isNeg1 = em_.nextTemp();
    em_.emit(isNeg1 + " = icmp eq " + ty + " " + divisor.ir + ", -1");
    std::string isMin = em_.nextTemp();
    em_.emit(isMin + " = icmp eq " + ty + " " + dividend.ir + ", " + minLit);
    std::string ov = em_.nextTemp();
    em_.emit(ov + " = and i1 " + isNeg1 + ", " + isMin);
    std::string safe = em_.nextTemp();
    em_.emit(safe + " = select i1 " + ov + ", " + ty + " 1, " + ty + " " +
             divisor.ir);
    return Value(safe, divisor.type);
}

std::string IRGen::toI1(const Value& v) {
    // Bool 存 i8（0/1），比较跳转需要 i1
    std::string reg = em_.nextTemp();
    em_.emit(reg + " = icmp ne i8 " + v.ir + ", 0");
    return reg;
}

Value IRGen::toStringValue(const Value& v) {
    // 可空须先 !! / ??；否则会把装箱 ptr 当 i32/i64 传给 to_str
    if (v.type && v.type->nullable) return Value();
    switch (v.type->kind) {
        case TypeKind::String:
            return v;
        case TypeKind::Long: {
            std::string reg = em_.nextTemp();
            em_.emit(reg + " = call ptr @hao_long_to_str(i64 " + v.ir + ")");
            return Value(reg, Type::makeString());
        }
        case TypeKind::ULong:
        case TypeKind::UIntPtr: {
            std::string reg = em_.nextTemp();
            em_.emit(reg + " = call ptr @hao_ulong_to_str(i64 " + v.ir + ")");
            return Value(reg, Type::makeString());
        }
        case TypeKind::Int: {
            std::string reg = em_.nextTemp();
            em_.emit(reg + " = call ptr @hao_int_to_str(i32 " + v.ir + ")");
            return Value(reg, Type::makeString());
        }
        case TypeKind::UInt: {
            std::string reg = em_.nextTemp();
            em_.emit(reg + " = call ptr @hao_uint_to_str(i32 " + v.ir + ")");
            return Value(reg, Type::makeString());
        }
        case TypeKind::SByte:
        case TypeKind::Byte:
        case TypeKind::Short:
        case TypeKind::UShort: {
            Value asInt = coerce(v, Type::makeInt(), 0, 0);
            return toStringValue(asInt);
        }
        case TypeKind::Double: {
            std::string reg = em_.nextTemp();
            em_.emit(reg + " = call ptr @hao_double_to_str(double " + v.ir + ")");
            return Value(reg, Type::makeString());
        }
        case TypeKind::Float: {
            std::string reg = em_.nextTemp();
            em_.emit(reg + " = call ptr @hao_float_to_str(float " + v.ir + ")");
            return Value(reg, Type::makeString());
        }
        case TypeKind::Bool: {
            std::string reg = em_.nextTemp();
            em_.emit(reg + " = call ptr @hao_bool_to_str(i8 " + v.ir + ")");
            return Value(reg, Type::makeString());
        }
        case TypeKind::Char: {
            std::string reg = em_.nextTemp();
            em_.emit(reg + " = call ptr @hao_char_to_str(i32 " + v.ir + ")");
            return Value(reg, Type::makeString());
        }
        default:
            return Value();
    }
}

// 计算对象字段地址
std::string IRGen::fieldPtr(const std::string& objIR, int slot) {
    std::string ptr = em_.nextTemp();
    em_.emit(ptr + " = getelementptr i64, ptr " + objIR +
             ", i64 " + std::to_string(slot));
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
    /* v0.53：Dijkstra 先 shade 再 publish，禁止 store→barrier 窗口漏标 */
    if (isGcPointerType(ty) && !barrierBase.empty())
        em_.emit("call void @hao_gc_barrier(ptr " + barrierBase +
                 ", ptr " + valIr + ")");
    em_.emit("store " + lt + " " + valIr + ", ptr " + addr);
}

void IRGen::emitVarStore(const SymbolPtr& sym, const TypePtr& ty,
                         const std::string& valIr) {
    std::string addr = varValuePtr(sym);
    // boxed 可变捕获；by-ref 数组形参可能写回堆字段（须写屏障）
    if (sym->boxed || (sym->byRefParam && isGcPointerType(ty)))
        emitHeapStore(addr, valIr, ty, addr);
    else
        em_.emit("store " + ty->llvmType() + " " + valIr + ", ptr " + addr);
}

std::string IRGen::emitObjectNew(int64_t nfields, int64_t bitmap) {
    std::string obj = em_.nextTemp();
    em_.emit(obj + " = call ptr @hao_object_new(i64 " +
             std::to_string(nfields) + ", i64 " + std::to_string(bitmap) + ")");
    return obj;
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
            em_.emit(r + " = zext i32 " + idx.ir + " to i64");
        else
            em_.emit(r + " = sext i32 " + idx.ir + " to i64");
        return r;
    }
    // 窄整数等：先升 Int 再 sext
    Value asInt = coerce(idx, Type::makeInt(), 0, 0);
    std::string r = em_.nextTemp();
    em_.emit(r + " = sext i32 " + asInt.ir + " to i64");
    return r;
}

// 计算数组元素地址，带运行时边界检查
std::string IRGen::arrayElemPtr(const Value& arr, const Value& idx) {
    std::string idx64 = indexAsI64(idx);
    std::string checked = em_.nextTemp();
    em_.emit(checked + " = call i64 @hao_array_check(ptr " + arr.ir +
             ", i64 " + idx64 + ")");
    std::string gepTy = "i64";
    if (arr.type && arr.type->elem)
        gepTy = arr.type->elem->arrayGepType();
    std::string ptr = em_.nextTemp();
    em_.emit(ptr + " = getelementptr " + gepTy + ", ptr " + arr.ir +
             ", i64 " + checked);
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
