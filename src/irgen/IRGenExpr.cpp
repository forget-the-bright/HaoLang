// ============================================================
//  HaoLang IR 生成器 —— 表达式部分
// ============================================================

#include "irgen/IRGen.h"

namespace hao {
Value IRGen::genExpr(HaoLangParser::ExprContext* e) {
    if (!e) return Value();
    setDebugLoc(e);
    return genAssign(e->assignExpr());
}

// ---------- 赋值 ----------
// 沿 postfix 链求值 primary 与成员访问 / !!，得到最终的数组/对象基值。
// 支持 this.items、obj!!.field 等；遇到索引或调用停止（由调用方处理）。
Value IRGen::evalPostfixBase(HaoLangParser::PostfixExprContext* pf) {
    Value cur = genPrimary(pf->primary());
    if (!cur.valid()) return cur;
    for (auto* op : pf->postfixOp()) {
        if (auto* mem = dynamic_cast<HaoLangParser::MemberAccessContext*>(op)) {
            cur = applyMemberAccess(cur, mem->IDENT()->getText(), pf);
            if (!cur.valid()) return cur;
            continue;
        }
        if (dynamic_cast<HaoLangParser::NotNullAssertContext*>(op)) {
            cur = applyNotNullAssert(cur, pf);
            if (!cur.valid()) return cur;
            continue;
        }
        break;   // 索引/调用由调用方处理
    }
    return cur;
}

bool IRGen::evalAssignRecv(HaoLangParser::PostfixExprContext* pf,
                           size_t endExclusive, Value& recv) {
    recv = genPrimary(pf->primary());
    if (!recv.valid()) return false;
    for (size_t i = 0; i < endExclusive; ++i) {
        auto* op = pf->postfixOp(static_cast<int>(i));
        if (auto* mem = dynamic_cast<HaoLangParser::MemberAccessContext*>(op)) {
            recv = applyMemberAccess(recv, mem->IDENT()->getText(), pf);
        } else if (dynamic_cast<HaoLangParser::NotNullAssertContext*>(op)) {
            recv = applyNotNullAssert(recv, pf);
        } else {
            return false;
        }
        if (!recv.valid()) return false;
    }
    return true;
}

// push 扩容后把新数组指针写回左值：变量直接 store；对象字段写回该字段槽。
Value IRGen::genAssign(HaoLangParser::AssignExprContext* e) {
    // 无赋值运算符：退化为三元表达式
    if (!e->assignOp()) return genTernary(e->ternaryExpr());

    std::string op = e->assignOp()->getText();
    auto* lhsCtx = e->ternaryExpr();

    // 沿单一子节点链下降，找到 postfixExpr 才能识别索引赋值
    HaoLangParser::PostfixExprContext* pf = nullptr;
    {
        antlr4::tree::ParseTree* node = lhsCtx;
        while (node && node->children.size() == 1)
            node = node->children[0];
        pf = dynamic_cast<HaoLangParser::PostfixExprContext*>(node);
    }

    // 统计 postfix 链末尾是否为"(.field|!!)* + 索引"
    size_t prefixBeforeIndex = 0;
    if (pf) {
        for (auto* op : pf->postfixOp()) {
            if (dynamic_cast<HaoLangParser::MemberAccessContext*>(op) ||
                dynamic_cast<HaoLangParser::NotNullAssertContext*>(op))
                ++prefixBeforeIndex;
            else break;
        }
    }

    // ---------- 数组元素赋值 a[i] = v / a!![i] = v / a.f[i] = v ----------
    if (pf && pf->postfixOp().size() == prefixBeforeIndex + 1) {
        if (auto* io = dynamic_cast<HaoLangParser::IndexOpContext*>(
                pf->postfixOp(prefixBeforeIndex))) {
            Value arr = evalPostfixBase(pf);
            if (!arr.valid()) return Value();
            if (arr.type->kind != TypeKind::Array) {
                error(pf, "只能对数组使用索引赋值，实际为 " + arr.type->toString());
                return Value();
            }
            if (arr.type->nullable) {
                error(pf, "不能对可空类型 " + arr.type->toString() +
                          " 直接赋值下标，请使用 '!!'");
                return Value();
            }
            rootGcOperand(arr); /* 数组跨下标/右值求值 */
            TypePtr elemType = arr.type->elem ? arr.type->elem : Type::makeInt();

            Value idx = genExpr(io->expr());
            if (!idx.valid()) return Value();
            if (!idx.type->isInteger()) {
                error(io->expr(), "数组索引必须是整数类型，实际为 " + idx.type->toString());
                return Value();
            }
            if (!ensureNonNullOperand(idx, io->expr(), "数组下标")) return Value();

            Value rhs = genAssign(e->assignExpr());
            if (!rhs.valid()) return Value();

            std::string ptr = arrayElemPtr(arr, idx);
            /* D9：下标写回薄 dbg.value（DI 名优先数组变量名） */
            std::string arrDbgName = "arr_elem";
            if (auto* idp = dynamic_cast<HaoLangParser::IdentPrimaryContext*>(
                    pf->primary())) {
                if (idp->IDENT()) arrDbgName = idp->IDENT()->getText();
            }

            if (op == "=") {
                if (!isAssignable(rhs.type, elemType)) {
                    error(e, "无法将 " + rhs.type->toString() + " 赋值给 " +
                             elemType->toString() + " 类型的数组元素");
                    return Value();
                }
                rhs = coerce(rhs, elemType, 0, 0);
                emitHeapStore(ptr, rhs.ir, elemType, arr.ir);
                emitDbgValueIf(elemType->llvmType(), rhs.ir, arrDbgName, 0, 0);
                return rhs;
            }

            // 复合赋值 a[i] += v（窄整数/浮点先提升再写回 trunc）
            std::string cur = emitLoad(elemType->llvmType(), ptr);
            Value curV(cur, elemType);
            if (!ensureNonNullOperand(curV, e, op) ||
                !ensureNonNullOperand(rhs, e, op))
                return Value();

            bool isBitAssign = (op == "&=" || op == "|=" || op == "^=" ||
                                op == "<<=" || op == ">>=");
            TypePtr calcTy;
            if (isBitAssign) {
                if (!elemType->isInteger() || !rhs.type->isInteger()) {
                    error(e, "位运算复合赋值要求整数类型，实际为 " +
                             elemType->toString());
                    return Value();
                }
                // <<= / >>=：结果类型=左操作数一元提升（对齐 Java）
                if (op == "<<=" || op == ">>=")
                    calcTy = Type::unaryBitwisePromote(elemType->kind);
                else
                    calcTy = Type::binaryBitwisePromote(elemType->kind, rhs.type->kind);
            } else if (!elemType->isNumeric()) {
                error(e, "运算符 '" + op + "' 不能用于 " + elemType->toString() + " 类型");
                return Value();
            } else {
                TypeKind rk = rhs.type->isNumeric() ? rhs.type->kind : elemType->kind;
                calcTy = Type::binaryNumericPromote(elemType->kind, rk);
            }
            if (!calcTy) {
                if (Type::isMixedSignedUnsigned64(elemType->kind, rhs.type->kind))
                    error(e, "64 位有符号与无符号不能隐式混合，请显式转换");
                else
                    error(e, "运算符 '" + op + "' 不能用于 " + elemType->toString());
                return Value();
            }

            bool isF = calcTy->isFloating();
            bool isU = calcTy->isUnsigned();
            std::string irOp;
            if      (op == "+=") irOp = isF ? "fadd" : "add";
            else if (op == "-=") irOp = isF ? "fsub" : "sub";
            else if (op == "*=") irOp = isF ? "fmul" : "mul";
            else if (op == "/=") irOp = isF ? "fdiv" : (isU ? "udiv" : "sdiv");
            else if (op == "%=") irOp = isF ? "frem" : (isU ? "urem" : "srem");
            else if (op == "&=")  irOp = "and";
            else if (op == "|=")  irOp = "or";
            else if (op == "^=")  irOp = "xor";
            else if (op == "<<=") irOp = "shl";
            else if (op == ">>=") irOp = isU ? "lshr" : "ashr";
            else {
                error(e, "数组元素不支持运算符 '" + op + "'");
                return Value();
            }
            curV = coerce(curV, calcTy, 0, 0);
            rhs = coerce(rhs, calcTy, 0, 0);
            if (op == "<<=" || op == ">>=") {
                int mask = (1 << Type::shiftMaskBits(calcTy->kind)) - 1;
                std::string masked = emitBinOp("and", calcTy->llvmType(), rhs.ir, std::to_string(mask));
                rhs = Value(masked, calcTy);
            }
            if (!isF && (op == "/=" || op == "%=")) {
                emitIntDivZeroCheck(rhs);
                if (!isU) rhs = emitSafeSignedDivisor(curV, rhs);
            }
            std::string reg = emitBinOp(irOp, calcTy->llvmType(), curV.ir, rhs.ir);
            Value out(reg, calcTy);
            out = coerce(out, elemType, 0, 0);
            emitHeapStore(ptr, out.ir, elemType, arr.ir);
            emitDbgValueIf(elemType->llvmType(), out.ir, arrDbgName, 0, 0);
            return out;
        }
    }

    // ---------- 对象字段赋值 obj.field / obj!!.field / a.b.c = v ----------
    if (pf && !pf->postfixOp().empty()) {
        if (auto* mem = dynamic_cast<HaoLangParser::MemberAccessContext*>(
                pf->postfixOp(pf->postfixOp().size() - 1))) {
            Value recv;
            if (!evalAssignRecv(pf, pf->postfixOp().size() - 1, recv)) {
                // 前缀含调用等，不是可赋值字段左值——落入下方报错
            } else if (recv.type->kind == TypeKind::Class) {
                if (recv.type->nullable) {
                    error(pf, "不能对可空类型 " + recv.type->toString() +
                              " 直接赋值字段，请使用 '!!'");
                    return Value();
                }
                auto ci = classOfType(recv.type);
                if (!ci) {
                    error(pf, "未定义的类 '" + recv.type->className + "'");
                    return Value();
                }
                std::string fname = mem->IDENT()->getText();

                // ---------- 静态字段赋值：类级全局变量，无实例指针 ----------
                if (const FieldInfo* sfi = ci->findStaticField(fname)) {
                    if (!canAccessMember(sfi->visibility, sfi->ownerClass)) {
                        error(pf, std::string("不能访问 ") + visName(sfi->visibility) +
                                   " 静态字段 '" + sfi->ownerClass + "." + fname + "'");
                        return Value();
                    }
                    if (!sfi->isMutable) {
                        error(pf, "不能给 val 声明的不可变静态字段 '" + fname + "' 赋值");
                        return Value();
                    }
                    emitStaticEnsureInit(ci);
                    std::string gptr = "@" + ci->name + "." + sfi->name;

                    Value rhs;
                    if (op == "=" && sfi->type->kind == TypeKind::Array &&
                        isEmptyArrayLiteral(e->assignExpr())) {
                        rhs = genEmptyArray(sfi->type->elem
                                            ? sfi->type->elem : Type::makeInt());
                    } else {
                        rhs = genAssign(e->assignExpr());
                        if (!rhs.valid()) return Value();
                    }

                    if (op == "=") {
                        if (!isAssignable(rhs.type, sfi->type)) {
                            error(e, "无法将 " + rhs.type->toString() + " 赋值给 " +
                                     sfi->type->toString() + " 类型的静态字段 '" + fname + "'");
                            return Value();
                        }
                        rhs = coerce(rhs, sfi->type, 0, 0);
                        emitGlobalGcStore(gptr, rhs.ir, sfi->type);
                        /* D8：静态字段赋值薄 dbg.value */
                        emitDbgValueIf(sfi->type->llvmType(), rhs.ir, fname, 0, 0);
                        return rhs;
                    }

                    // 复合赋值 Class.X op= v
                    std::string cur = emitLoad(sfi->type->llvmType(), gptr);

                    if (sfi->type->kind == TypeKind::String && op == "+=") {
                        Value curV(cur, sfi->type);
                        if (!ensureNonNullOperand(curV, e, "+=") ||
                            !ensureNonNullOperand(rhs, e, "+="))
                            return Value();
                        rootGcOperand(curV);
                        Value rs = toStringValue(rhs);
                        if (!rs.valid()) {
                            error(e, "无法将 " + rhs.type->toString() +
                                     " 拼接到 String");
                            return Value();
                        }
                        rootGcOperand(rs);
                        std::string reg = emitCall("ptr", "@hao_str_concat", "ptr " + curV.ir + ", ptr " + rs.ir);
                        Value out(reg, Type::makeString());
                        rootGcOperand(out);
                        emitGlobalGcStore(gptr, out.ir, Type::makeString());
                        /* D10：静态字段复合薄 dbg.value */
                        emitDbgValueIf(Type::makeString()->llvmType(), out.ir, fname, 0, 0);
                        return out;
                    }
                    if (sfi->type->kind == TypeKind::Array && op == "+=") {
                        Value curV(cur, sfi->type);
                        if (!ensureNonNullOperand(curV, e, "+=")) return Value();
                        TypePtr elemType = sfi->type->elem ? sfi->type->elem : Type::makeInt();
                        if (!isAssignable(rhs.type, elemType)) {
                            error(e, "无法将 " + rhs.type->toString() + " 追加到 " +
                                     sfi->type->toString() + " 类型的静态字段");
                            return Value();
                        }
                        rhs = coerce(rhs, elemType, 0, 0);
                        std::string val64 = em_.boxToI64(rhs.ir, elemType);
                        std::string reg = emitCall("ptr", "@hao_array_push",
                                                   "ptr " + cur + ", i64 " + val64);
                        emitGlobalGcStore(gptr, reg, sfi->type);
                        /* D10：静态字段复合薄 dbg.value */
                        emitDbgValueIf(sfi->type->llvmType(), reg, fname, 0, 0);
                        return Value(reg, sfi->type);
                    }
                    if (!sfi->type->isNumeric() &&
                        !(op == "&=" || op == "|=" || op == "^=" ||
                          op == "<<=" || op == ">>=")) {
                        error(e, "运算符 '" + op + "' 不能用于 " +
                                 sfi->type->toString() + " 类型");
                        return Value();
                    }
                    Value curV(cur, sfi->type);
                    if (!ensureNonNullOperand(curV, e, op) ||
                        !ensureNonNullOperand(rhs, e, op))
                        return Value();
                    bool isBitAssign = (op == "&=" || op == "|=" || op == "^=" ||
                                        op == "<<=" || op == ">>=");
                    TypePtr calcTy;
                    if (isBitAssign) {
                        if (!sfi->type->isInteger() || !rhs.type->isInteger()) {
                            error(e, "位运算复合赋值要求整数类型，实际为 " +
                                     sfi->type->toString());
                            return Value();
                        }
                        if (op == "<<=" || op == ">>=")
                            calcTy = Type::unaryBitwisePromote(sfi->type->kind);
                        else
                            calcTy = Type::binaryBitwisePromote(sfi->type->kind,
                                                                rhs.type->kind);
                    } else if (!sfi->type->isNumeric()) {
                        error(e, "运算符 '" + op + "' 不能用于 " +
                                 sfi->type->toString() + " 类型");
                        return Value();
                    } else {
                        TypeKind rk = rhs.type->isNumeric() ? rhs.type->kind
                                                           : sfi->type->kind;
                        calcTy = Type::binaryNumericPromote(sfi->type->kind, rk);
                    }
                    if (!calcTy) {
                        if (Type::isMixedSignedUnsigned64(sfi->type->kind, rhs.type->kind))
                            error(e, "64 位有符号与无符号不能隐式混合，请显式转换");
                        else
                            error(e, "静态字段不支持运算符 '" + op + "'");
                        return Value();
                    }
                    bool isF = calcTy->isFloating();
                    bool isU = calcTy->isUnsigned();
                    std::string irOp;
                    if      (op == "+=") irOp = isF ? "fadd" : "add";
                    else if (op == "-=") irOp = isF ? "fsub" : "sub";
                    else if (op == "*=") irOp = isF ? "fmul" : "mul";
                    else if (op == "/=") irOp = isF ? "fdiv" : (isU ? "udiv" : "sdiv");
                    else if (op == "%=") irOp = isF ? "frem" : (isU ? "urem" : "srem");
                    else if (op == "&=")  irOp = "and";
                    else if (op == "|=")  irOp = "or";
                    else if (op == "^=")  irOp = "xor";
                    else if (op == "<<=") irOp = "shl";
                    else if (op == ">>=") irOp = isU ? "lshr" : "ashr";
                    else {
                        error(e, "静态字段不支持运算符 '" + op + "'");
                        return Value();
                    }
                    curV = coerce(curV, calcTy, 0, 0);
                    rhs = coerce(rhs, calcTy, 0, 0);
                    if (op == "<<=" || op == ">>=") {
                        int mask = (1 << Type::shiftMaskBits(calcTy->kind)) - 1;
                        std::string masked = emitBinOp("and", calcTy->llvmType(), rhs.ir, std::to_string(mask));
                        rhs = Value(masked, calcTy);
                    }
                    if (!isF && (op == "/=" || op == "%=")) {
                        emitIntDivZeroCheck(rhs);
                        if (!isU) rhs = emitSafeSignedDivisor(curV, rhs);
                    }
                    std::string reg = emitBinOp(irOp, calcTy->llvmType(), curV.ir, rhs.ir);
                    Value out(reg, calcTy);
                    out = coerce(out, sfi->type, 0, 0);
                    emitStore(sfi->type->llvmType(), out.ir, gptr);
                    /* D10：静态字段复合薄 dbg.value */
                    emitDbgValueIf(sfi->type->llvmType(), out.ir, fname, 0, 0);
                    return out;
                }

                const FieldInfo* fi = ci->findField(fname);
                if (!fi) {
                    error(pf, "类 '" + ci->name + "' 没有字段 '" + fname + "'");
                    return Value();
                }
                if (!canAccessMember(fi->visibility, fi->ownerClass)) {
                    error(pf, std::string("不能访问 ") + visName(fi->visibility) +
                               " 字段 '" + fi->ownerClass + "." + fname + "'");
                    return Value();
                }
                if (!fi->isMutable) {
                    error(pf, "不能给 val 声明的不可变字段 '" + fname + "' 赋值");
                    return Value();
                }

                rootGcOperand(recv); /* 实例跨右值求值 */
                // 空数组字面量 [] 按字段类型生成（[] 默认是 [Int]，赋给
                // [String] 字段会类型不匹配，这里按目标元素类型生成）
                Value rhs;
                if (op == "=" && fi->type->kind == TypeKind::Array &&
                    isEmptyArrayLiteral(e->assignExpr())) {
                    rhs = genEmptyArray(fi->type->elem
                                        ? fi->type->elem : Type::makeInt());
                } else {
                    rhs = genAssign(e->assignExpr());
                    if (!rhs.valid()) return Value();
                }

                std::string fp = fieldPtr(recv.ir, fi->slot);

                if (op == "=") {
                    if (!isAssignable(rhs.type, fi->type)) {
                        error(e, "无法将 " + rhs.type->toString() + " 赋值给 " +
                                 fi->type->toString() + " 类型的字段 '" + fname + "'");
                        return Value();
                    }
                    rhs = coerce(rhs, fi->type, 0, 0);
                    emitHeapStore(fp, rhs.ir, fi->type, recv.ir);
                    /* D8：实例字段赋值薄 dbg.value */
                    emitDbgValueIf(fi->type->llvmType(), rhs.ir, fname, 0, 0);
                    return rhs;
                }

                // 复合赋值 obj.f += v
                std::string cur = emitLoad(fi->type->llvmType(), fp);

                // 字符串 += 走拼接（String? 字段须先 !!）
                if (fi->type->kind == TypeKind::String && op == "+=") {
                    Value curV(cur, fi->type);
                    if (!ensureNonNullOperand(curV, e, "+=") ||
                        !ensureNonNullOperand(rhs, e, "+="))
                        return Value();
                    rootGcOperand(curV);
                    Value rs = toStringValue(rhs);
                    if (!rs.valid()) {
                        error(e, "无法将 " + rhs.type->toString() +
                                 " 拼接到 String");
                        return Value();
                    }
                    rootGcOperand(rs);
                    std::string reg = emitCall("ptr", "@hao_str_concat", "ptr " + curV.ir + ", ptr " + rs.ir);
                    Value out(reg, Type::makeString());
                    rootGcOperand(out);
                    emitHeapStore(fp, out.ir, Type::makeString(), recv.ir);
                    /* D10：实例字段复合薄 dbg.value */
                    emitDbgValueIf(Type::makeString()->llvmType(), out.ir, fname, 0, 0);
                    return out;
                }

                // 数组字段 += 元素 => push，扩容后写回字段
                if (fi->type->kind == TypeKind::Array && op == "+=") {
                    Value curV(cur, fi->type);
                    if (!ensureNonNullOperand(curV, e, "+=")) return Value();
                    TypePtr elemType = fi->type->elem ? fi->type->elem : Type::makeInt();
                    if (!isAssignable(rhs.type, elemType)) {
                        error(e, "无法将 " + rhs.type->toString() + " 追加到 " +
                                 fi->type->toString() + " 类型的字段");
                        return Value();
                    }
                    rhs = coerce(rhs, elemType, 0, 0);
                    std::string val64 = em_.boxToI64(rhs.ir, elemType);
                    std::string reg = emitCall("ptr", "@hao_array_push",
                                               "ptr " + cur + ", i64 " + val64);
                    emitHeapStore(fp, reg, fi->type, recv.ir);
                    /* D10：实例字段复合薄 dbg.value */
                    emitDbgValueIf(fi->type->llvmType(), reg, fname, 0, 0);
                    return Value(reg, fi->type);
                }

                if (!fi->type->isNumeric() &&
                    !(op == "&=" || op == "|=" || op == "^=" ||
                      op == "<<=" || op == ">>=")) {
                    error(e, "运算符 '" + op + "' 不能用于 " +
                             fi->type->toString() + " 类型");
                    return Value();
                }
                Value curV(cur, fi->type);
                if (!ensureNonNullOperand(curV, e, op) ||
                    !ensureNonNullOperand(rhs, e, op))
                    return Value();
                bool isBitAssign = (op == "&=" || op == "|=" || op == "^=" ||
                                    op == "<<=" || op == ">>=");
                TypePtr calcTy;
                if (isBitAssign) {
                    if (!fi->type->isInteger() || !rhs.type->isInteger()) {
                        error(e, "位运算复合赋值要求整数类型，实际为 " +
                                 fi->type->toString());
                        return Value();
                    }
                    if (op == "<<=" || op == ">>=")
                        calcTy = Type::unaryBitwisePromote(fi->type->kind);
                    else
                        calcTy = Type::binaryBitwisePromote(fi->type->kind,
                                                            rhs.type->kind);
                } else if (!fi->type->isNumeric()) {
                    error(e, "运算符 '" + op + "' 不能用于 " +
                             fi->type->toString() + " 类型");
                    return Value();
                } else {
                    TypeKind rk = rhs.type->isNumeric() ? rhs.type->kind
                                                       : fi->type->kind;
                    calcTy = Type::binaryNumericPromote(fi->type->kind, rk);
                }
                if (!calcTy) {
                    if (Type::isMixedSignedUnsigned64(fi->type->kind, rhs.type->kind))
                        error(e, "64 位有符号与无符号不能隐式混合，请显式转换");
                    else
                        error(e, "字段不支持运算符 '" + op + "'");
                    return Value();
                }
                bool isF = calcTy->isFloating();
                bool isU = calcTy->isUnsigned();
                std::string irOp;
                if      (op == "+=") irOp = isF ? "fadd" : "add";
                else if (op == "-=") irOp = isF ? "fsub" : "sub";
                else if (op == "*=") irOp = isF ? "fmul" : "mul";
                else if (op == "/=") irOp = isF ? "fdiv" : (isU ? "udiv" : "sdiv");
                else if (op == "%=") irOp = isF ? "frem" : (isU ? "urem" : "srem");
                else if (op == "&=")  irOp = "and";
                else if (op == "|=")  irOp = "or";
                else if (op == "^=")  irOp = "xor";
                else if (op == "<<=") irOp = "shl";
                else if (op == ">>=") irOp = isU ? "lshr" : "ashr";
                else {
                    error(e, "字段不支持运算符 '" + op + "'");
                    return Value();
                }
                curV = coerce(curV, calcTy, 0, 0);
                rhs = coerce(rhs, calcTy, 0, 0);
                if (op == "<<=" || op == ">>=") {
                    int mask = (1 << Type::shiftMaskBits(calcTy->kind)) - 1;
                    std::string masked = emitBinOp("and", calcTy->llvmType(), rhs.ir, std::to_string(mask));
                    rhs = Value(masked, calcTy);
                }
                if (!isF && (op == "/=" || op == "%=")) {
                    emitIntDivZeroCheck(rhs);
                    if (!isU) rhs = emitSafeSignedDivisor(curV, rhs);
                }
                std::string reg = emitBinOp(irOp, calcTy->llvmType(), curV.ir, rhs.ir);
                Value out(reg, calcTy);
                out = coerce(out, fi->type, 0, 0);
                emitHeapStore(fp, out.ir, fi->type, recv.ir);
                /* D10：实例字段复合薄 dbg.value */
                emitDbgValueIf(fi->type->llvmType(), out.ir, fname, 0, 0);
                return out;
            }
        }
    }

    // ---------- 简单变量赋值 ----------
    std::string varName = lhsCtx->getText();
    auto sym = syms_.lookup(varName);

    if (!sym || sym->kind != SymbolKind::Variable) {
        error(lhsCtx, "赋值目标 '" + varName + "' 不是可赋值的变量");
        return Value();
    }
    if (!sym->isMutable) {
        error(lhsCtx, "不能给 val 声明的不可变变量 '" + varName + "' 赋值");
        return Value();
    }

    Value rhs = genAssign(e->assignExpr());
    if (!rhs.valid()) return Value();

    // 复合赋值：先取当前值参与运算
    if (op != "=") {
        Value cur = loadVar(sym);

        bool isBitAssign = (op == "&=" || op == "|=" || op == "^=" ||
                            op == "<<=" || op == ">>=");

        // 字符串 += 走拼接（String? 须先 !!）
        if (sym->type->kind == TypeKind::String && op == "+=") {
            if (!ensureNonNullOperand(cur, e, "+=") ||
                !ensureNonNullOperand(rhs, e, "+="))
                return Value();
            rootGcOperand(cur);
            Value rs = toStringValue(rhs);
            if (!rs.valid()) {
                error(e, "无法将 " + rhs.type->toString() + " 拼接到 String");
                return Value();
            }
            rootGcOperand(rs);
            std::string reg = emitCall("ptr", "@hao_str_concat", "ptr " + cur.ir + ", ptr " + rs.ir);
            Value out(reg, Type::makeString());
            rootGcOperand(out);
            emitVarStore(sym, Type::makeString(), out.ir);
            emitDbgValueIf(Type::makeString()->llvmType(), out.ir, varName,
                           sym->line, 0);
            return out;
        }

        // 数组 += 元素 => push（动态扩容，可能 realloc 移动，必须写回变量）
        if (sym->type->kind == TypeKind::Array && op == "+=") {
            if (!ensureNonNullOperand(cur, e, "+=")) return Value();
            TypePtr elemType = sym->type->elem ? sym->type->elem : Type::makeInt();
            if (!isAssignable(rhs.type, elemType)) {
                error(e, "无法将 " + rhs.type->toString() + " 追加到 " +
                         sym->type->toString() + " 类型的数组");
                return Value();
            }
            rhs = coerce(rhs, elemType, 0, 0);

            // 把元素统一转成 i64 传入（任意 8 字节类型均可；可空/引用走 ptrtoint）
            std::string val64 = em_.boxToI64(rhs.ir, elemType);

            std::string reg = emitCall("ptr", "@hao_array_push",
                                       "ptr " + cur.ir + ", i64 " + val64);
            emitVarStore(sym, sym->type, reg);
            emitDbgValueIf(sym->type->llvmType(), reg, varName, sym->line, 0);
            return Value(reg, sym->type);
        }

        TypePtr calcTy;
        if (isBitAssign) {
            if (!sym->type->isInteger() || !rhs.type->isInteger()) {
                error(e, "位运算复合赋值要求整数类型，实际为 " +
                         sym->type->toString());
                return Value();
            }
            if (!ensureNonNullOperand(cur, e, op) ||
                !ensureNonNullOperand(rhs, e, op))
                return Value();
            if (op == "<<=" || op == ">>=")
                calcTy = Type::unaryBitwisePromote(sym->type->kind);
            else
                calcTy = Type::binaryBitwisePromote(sym->type->kind, rhs.type->kind);
        } else if (!sym->type->isNumeric()) {
            error(e, "运算符 '" + op + "' 不能用于 " + sym->type->toString() + " 类型");
            return Value();
        } else {
            if (!ensureNonNullOperand(cur, e, op) ||
                !ensureNonNullOperand(rhs, e, op))
                return Value();
            TypeKind rk = rhs.type->isNumeric() ? rhs.type->kind : sym->type->kind;
            calcTy = Type::binaryNumericPromote(sym->type->kind, rk);
        }
        if (!calcTy) {
            if (sym && Type::isMixedSignedUnsigned64(sym->type->kind, rhs.type->kind))
                error(e, "64 位有符号与无符号不能隐式混合，请显式转换");
            else
                error(e, "当前版本尚不支持运算符 '" + op + "'");
            return Value();
        }

        bool isF = calcTy->isFloating();
        bool isU = calcTy->isUnsigned();
        std::string irOp;
        if      (op == "+=") irOp = isF ? "fadd" : "add";
        else if (op == "-=") irOp = isF ? "fsub" : "sub";
        else if (op == "*=") irOp = isF ? "fmul" : "mul";
        else if (op == "/=") irOp = isF ? "fdiv" : (isU ? "udiv" : "sdiv");
        else if (op == "%=") irOp = isF ? "frem" : (isU ? "urem" : "srem");
        else if (op == "&=")  irOp = "and";
        else if (op == "|=")  irOp = "or";
        else if (op == "^=")  irOp = "xor";
        else if (op == "<<=") irOp = "shl";
        else if (op == ">>=") irOp = isU ? "lshr" : "ashr";
        else {
            error(e, "当前版本尚不支持运算符 '" + op + "'");
            return Value();
        }

        // 窄整数/浮点复合赋值：提升运算再 trunc/fptrunc 写回
        Value lhsV = coerce(cur, calcTy, 0, 0);
        rhs = coerce(rhs, calcTy, 0, 0);
        if (op == "<<=" || op == ">>=") {
            int mask = (1 << Type::shiftMaskBits(calcTy->kind)) - 1;
            std::string masked = emitBinOp("and", calcTy->llvmType(), rhs.ir, std::to_string(mask));
            rhs = Value(masked, calcTy);
        }
        if (!isF && (op == "/=" || op == "%=")) {
            emitIntDivZeroCheck(rhs);
            if (!isU) rhs = emitSafeSignedDivisor(lhsV, rhs);
        }
        std::string reg = emitBinOp(irOp, calcTy->llvmType(), lhsV.ir, rhs.ir);
        Value out(reg, calcTy);
        out = coerce(out, sym->type, 0, 0);
        emitVarStore(sym, sym->type, out.ir);
        /* D7：复合赋值薄 dbg.value */
        emitDbgValueIf(sym->type->llvmType(), out.ir, varName, sym->line, 0);
        invalidateSmartCast(varName);
        return out;
    }

    // 空数组字面量 [] 按变量标注类型生成（与 var 声明一致）
    if (op == "=" && sym->type->kind == TypeKind::Array &&
        isEmptyArrayLiteral(e->assignExpr())) {
        rhs = genEmptyArray(sym->type->elem ? sym->type->elem : Type::makeInt());
    }

    // 简单赋值
    if (!isAssignable(rhs.type, sym->type)) {
        error(e, "无法将 " + rhs.type->toString() + " 赋值给 " +
                 sym->type->toString() + " 类型的变量 '" + varName + "'");
        return Value();
    }
    rhs = coerce(rhs, sym->type, 0, 0);
    emitVarStore(sym, sym->type, rhs.ir);
    /* D6：-g 简单赋值薄 dbg.value（非全覆盖） */
    emitDbgValueIf(sym->type->llvmType(), rhs.ir, varName, sym->line, 0);
    invalidateSmartCast(varName);
    return rhs;
}

// ---------- 三元 ----------
Value IRGen::genTernary(HaoLangParser::TernaryExprContext* e) {
    if (e->QUESTION() == nullptr) return genNullCoalesce(e->nullCoalesceExpr());
    error(e, "当前版本尚不支持三元条件表达式");
    return Value();
}

// ---------- 空合并 ?? ----------
//  左结合：a ?? b ?? c => (a ?? b) ?? c。
//  左侧非可空时直接取左侧（短路，不求值右侧）。否则生成控制流：
//  左侧非空就用左侧，为空才求值右侧；两侧先用 phi 合并为非空指针，
//  若是装箱值类型再拆箱为底层值。
Value IRGen::genNullCoalesce(HaoLangParser::NullCoalesceExprContext* e) {
    auto ors = e->orExpr();
    Value acc = genOr(ors[0]);
    if (!acc.valid()) return Value();
    if (ors.size() == 1) return acc;

    for (size_t i = 1; i < ors.size(); ++i) {
        // 非可空类型不可能为 null，直接保留左侧（短路：不求值右侧）
        if (!acc.type->nullable && !acc.type->isNull()) {
            return acc;
        }

        /* 左值跨右侧求值（safepoint）须挂根；phi 用 spill 后的 ir */
        rootGcOperand(acc);

        std::string rhsL   = em_.nextLabel("coalesce.rhs");
        std::string mergeL = em_.nextLabel("coalesce.merge");

        std::string entryBlock = em_.currentBlock();
        std::string isNull = genNotNullCheck(acc);   // 1 表示非空
        emitCondBr(isNull, mergeL, rhsL);

        em_.emitLabel(rhsL);
        Value rhs = genOr(ors[i]);
        if (!rhs.valid()) return Value();
        rootGcOperand(rhs);

        // 确定结果类型：取左侧去掉可空后的底层类型；
        // 若左侧是纯 null 字面量，则取右侧类型。
        TypePtr resultType;
        if (acc.type->isNull()) {
            resultType = rhs.type;
        } else {
            resultType = std::make_shared<Type>(*acc.type);
            resultType->nullable = false;
            // 右侧须与左侧底层兼容（禁 Int? ?? "x" / 错误宽度混用）
            auto leftNN = resultType;
            auto rhsNN = std::make_shared<Type>(*rhs.type);
            rhsNN->nullable = false;
            TypePtr coerceTo = leftNN;
            if (rhs.type->nullable)
                coerceTo = leftNN->asNullable();
            if (!rhs.type->isNull() &&
                !isAssignable(rhs.type, coerceTo) &&
                !isAssignable(rhsNN, leftNN)) {
                error(ors[i], std::string("运算符 '") + "??" + "' 右侧类型 " +
                              rhs.type->toString() +
                              " 与左侧 " + acc.type->toString() + " 不兼容");
                return Value();
            }
            if (!rhs.type->isNull())
                rhs = coerce(rhs, coerceTo, 0, 0);
            if (rhs.type->nullable)
                resultType = resultType->asNullable();
        }

        // 右侧若非可空值类型，需要先装箱成 ptr 才能与左侧 phi 合并。
        // 必须在发出终结 br 之前完成，否则装箱指令会落在 br 之后。
        Value rhsPtr = rhs;
        if (!rhs.type->nullable &&
            (rhs.type->isInteger() || rhs.type->isFloating() ||
             rhs.type->kind == TypeKind::Bool)) {
            rhsPtr = boxToNullable(rhs);
        }

        std::string rhsBlock = em_.currentBlock();
        emitBr(mergeL);

        em_.emitLabel(mergeL);
        std::string phi = emitPhi("ptr",
            "[ " + acc.ir + ", %" + entryBlock +
            " ], [ " + rhsPtr.ir + ", %" + rhsBlock + " ]");

        // 底层是值类型且结果非可空：拆箱（窄整数先 wide 再 trunc）
        if (!resultType->nullable &&
            (resultType->isInteger() || resultType->isFloating() ||
             resultType->kind == TypeKind::Bool)) {
            std::string unboxed;
            switch (resultType->kind) {
                case TypeKind::Long: case TypeKind::ULong: case TypeKind::UIntPtr:
                    unboxed = emitCall("i64", "@hao_unbox_i64", "ptr " + phi);
                    break;
                case TypeKind::Double:
                    unboxed = emitCall("double", "@hao_unbox_f64", "ptr " + phi);
                    break;
                case TypeKind::Float:
                    unboxed = emitCall("float", "@hao_unbox_f32", "ptr " + phi);
                    break;
                case TypeKind::Short: case TypeKind::UShort: {
                    std::string wide = emitCall("i32", "@hao_unbox_i32", "ptr " + phi);
                    unboxed = emitCast("trunc", "i32", wide, "i16");
                    break;
                }
                case TypeKind::SByte: case TypeKind::Byte: {
                    std::string wide = emitCall("i32", "@hao_unbox_i32", "ptr " + phi);
                    unboxed = emitCast("trunc", "i32", wide, "i8");
                    break;
                }
                case TypeKind::Bool: {
                    std::string wide = emitCall("i32", "@hao_unbox_i32", "ptr " + phi);
                    unboxed = emitCast("trunc", "i32", wide, "i8");
                    break;
                }
                default: // Int / UInt / Char
                    unboxed = emitCall("i32", "@hao_unbox_i32", "ptr " + phi);
                    break;
            }
            acc = Value(unboxed, resultType);
        } else {
            acc = Value(phi, resultType);
            rootGcOperand(acc); /* phi 后重新挂根，供下一轮 ?? / 调用 */
        }
    }
    return acc;
}

// ---------- 逻辑或（短路） ----------
Value IRGen::genOr(HaoLangParser::OrExprContext* e) {
    auto ands = e->andExpr();
    if (ands.size() == 1) return genAnd(ands[0]);

    Value acc = genAnd(ands[0]);
    if (!acc.valid()) return Value();
    if (acc.type->kind != TypeKind::Bool) {
        error(ands[0], "运算符 '||' 要求 Bool 类型，实际为 " + acc.type->toString());
        return Value();
    }
    if (!ensureNonNullOperand(acc, ands[0], "||")) return Value();

    // a || b：左侧为真则短路，phi 汇聚两条路径
    for (size_t i = 1; i < ands.size(); ++i) {
        std::string rhsL   = em_.nextLabel("or.rhs");
        std::string mergeL = em_.nextLabel("or.merge");

        std::string lhsI1 = toI1(acc);
        std::string entryBlock = em_.currentBlock();   // phi 的短路前驱
        emitCondBr(lhsI1, mergeL, rhsL);

        em_.emitLabel(rhsL);
        Value rhs = genAnd(ands[i]);
        if (!rhs.valid()) return Value();
        if (rhs.type->kind != TypeKind::Bool) {
            error(ands[i], "运算符 '||' 要求 Bool 类型，实际为 " + rhs.type->toString());
            return Value();
        }
        if (!ensureNonNullOperand(rhs, ands[i], "||")) return Value();
        std::string rhsI1 = toI1(rhs);
        // 右侧求值可能自身包含分支，其结束块以 currentBlock() 为准
        std::string rhsEndBlock = em_.currentBlock();
        emitBr(mergeL);

        em_.emitLabel(mergeL);
        std::string phi = emitPhi("i1",
            "[ true, %" + entryBlock + " ], [ " +
            rhsI1 + ", %" + rhsEndBlock + " ]");
        std::string ext = emitCast("zext", "i1", phi, "i8");
        acc = Value(ext, Type::makeBool());
    }
    return acc;
}

// ---------- 逻辑与（短路） ----------
Value IRGen::genAnd(HaoLangParser::AndExprContext* e) {
    auto bits = e->bitOrExpr();
    if (bits.size() == 1) return genBitOr(bits[0]);

    Value acc = genBitOr(bits[0]);
    if (!acc.valid()) return Value();
    if (acc.type->kind != TypeKind::Bool) {
        error(bits[0], "运算符 '&&' 要求 Bool 类型，实际为 " + acc.type->toString());
        return Value();
    }
    if (!ensureNonNullOperand(acc, bits[0], "&&")) return Value();

    // a && b：左侧为假则短路
    for (size_t i = 1; i < bits.size(); ++i) {
        std::string rhsL   = em_.nextLabel("and.rhs");
        std::string mergeL = em_.nextLabel("and.merge");

        std::string lhsI1 = toI1(acc);
        std::string entryBlock = em_.currentBlock();
        emitCondBr(lhsI1, rhsL, mergeL);

        em_.emitLabel(rhsL);
        Value rhs = genBitOr(bits[i]);
        if (!rhs.valid()) return Value();
        if (rhs.type->kind != TypeKind::Bool) {
            error(bits[i], "运算符 '&&' 要求 Bool 类型，实际为 " + rhs.type->toString());
            return Value();
        }
        if (!ensureNonNullOperand(rhs, bits[i], "&&")) return Value();
        std::string rhsI1 = toI1(rhs);
        std::string rhsEndBlock = em_.currentBlock();
        emitBr(mergeL);

        em_.emitLabel(mergeL);
        std::string phi = emitPhi("i1",
            "[ false, %" + entryBlock + " ], [ " +
            rhsI1 + ", %" + rhsEndBlock + " ]");
        std::string ext = emitCast("zext", "i1", phi, "i8");
        acc = Value(ext, Type::makeBool());
    }
    return acc;
}

// ---------- 按位或 ----------
Value IRGen::genBitOr(HaoLangParser::BitOrExprContext* e) {
    auto xs = e->bitXorExpr();
    if (xs.size() == 1) return genBitXor(xs[0]);

    Value lhs = genBitXor(xs[0]);
    if (!lhs.valid()) return Value();
    for (size_t i = 1; i < xs.size(); ++i) {
        Value rhs = genBitXor(xs[i]);
        if (!rhs.valid()) return Value();
        if (!lhs.type->isInteger() || !rhs.type->isInteger()) {
            error(e, "运算符 '|' 要求整数类型，实际为 " + lhs.type->toString() +
                     " 与 " + rhs.type->toString());
            return Value();
        }
        if (!ensureNonNullOperand(lhs, e, "|") ||
            !ensureNonNullOperand(rhs, e, "|"))
            return Value();
        TypePtr rt = Type::binaryBitwisePromote(lhs.type->kind, rhs.type->kind);
        if (!rt) {
            if (Type::isMixedSignedUnsigned64(lhs.type->kind, rhs.type->kind))
                error(e, "64 位有符号与无符号不能隐式混合，请显式转换");
            else
                error(e, "运算符 '|' 无法提升 " + lhs.type->toString() +
                         " 与 " + rhs.type->toString());
            return Value();
        }
        lhs = coerce(lhs, rt, 0, 0);
        rhs = coerce(rhs, rt, 0, 0);
        std::string reg = emitBinOp("or", rt->llvmType(), lhs.ir, rhs.ir);
        lhs = Value(reg, rt);
    }
    return lhs;
}

// ---------- 按位异或 ----------
Value IRGen::genBitXor(HaoLangParser::BitXorExprContext* e) {
    auto as_ = e->bitAndExpr();
    if (as_.size() == 1) return genBitAnd(as_[0]);

    Value lhs = genBitAnd(as_[0]);
    if (!lhs.valid()) return Value();
    for (size_t i = 1; i < as_.size(); ++i) {
        Value rhs = genBitAnd(as_[i]);
        if (!rhs.valid()) return Value();
        if (!lhs.type->isInteger() || !rhs.type->isInteger()) {
            error(e, "运算符 '^' 要求整数类型，实际为 " + lhs.type->toString() +
                     " 与 " + rhs.type->toString());
            return Value();
        }
        if (!ensureNonNullOperand(lhs, e, "^") ||
            !ensureNonNullOperand(rhs, e, "^"))
            return Value();
        TypePtr rt = Type::binaryBitwisePromote(lhs.type->kind, rhs.type->kind);
        if (!rt) {
            if (Type::isMixedSignedUnsigned64(lhs.type->kind, rhs.type->kind))
                error(e, "64 位有符号与无符号不能隐式混合，请显式转换");
            else
                error(e, "运算符 '^' 无法提升 " + lhs.type->toString() +
                         " 与 " + rhs.type->toString());
            return Value();
        }
        lhs = coerce(lhs, rt, 0, 0);
        rhs = coerce(rhs, rt, 0, 0);
        std::string reg = emitBinOp("xor", rt->llvmType(), lhs.ir, rhs.ir);
        lhs = Value(reg, rt);
    }
    return lhs;
}

// ---------- 按位与 ----------
Value IRGen::genBitAnd(HaoLangParser::BitAndExprContext* e) {
    auto eqs = e->equalityExpr();
    if (eqs.size() == 1) return genEquality(eqs[0]);

    Value lhs = genEquality(eqs[0]);
    if (!lhs.valid()) return Value();
    for (size_t i = 1; i < eqs.size(); ++i) {
        Value rhs = genEquality(eqs[i]);
        if (!rhs.valid()) return Value();
        if (!lhs.type->isInteger() || !rhs.type->isInteger()) {
            error(e, "运算符 '&' 要求整数类型，实际为 " + lhs.type->toString() +
                     " 与 " + rhs.type->toString());
            return Value();
        }
        if (!ensureNonNullOperand(lhs, e, "&") ||
            !ensureNonNullOperand(rhs, e, "&"))
            return Value();
        TypePtr rt = Type::binaryBitwisePromote(lhs.type->kind, rhs.type->kind);
        if (!rt) {
            if (Type::isMixedSignedUnsigned64(lhs.type->kind, rhs.type->kind))
                error(e, "64 位有符号与无符号不能隐式混合，请显式转换");
            else
                error(e, "运算符 '&' 无法提升 " + lhs.type->toString() +
                         " 与 " + rhs.type->toString());
            return Value();
        }
        lhs = coerce(lhs, rt, 0, 0);
        rhs = coerce(rhs, rt, 0, 0);
        std::string reg = emitBinOp("and", rt->llvmType(), lhs.ir, rhs.ir);
        lhs = Value(reg, rt);
    }
    return lhs;
}

// ---------- 相等比较 ----------
Value IRGen::genEquality(HaoLangParser::EqualityExprContext* e) {
    auto rels = e->relationalExpr();
    if (rels.size() == 1) return genRelational(rels[0]);

    Value lhs = genRelational(rels[0]);
    if (!lhs.valid()) return Value();

    for (size_t i = 1; i < rels.size(); ++i) {
        rootGcOperand(lhs); /* String/对象左值跨右求值 */
        Value rhs = genRelational(rels[i]);
        if (!rhs.valid()) return Value();

        // 运算符位于两个操作数之间：children 布局为 [操作数, 运算符, 操作数, ...]
        std::string op = e->children[2 * i - 1]->getText();

        // 与 null 比较，或任一方为可空引用/装箱值：用指针比较，
        // 不能走 hao_str_eq（会解引用 null）。
        bool eitherNull = lhs.type->isNull() || rhs.type->isNull();
        bool eitherNullable = lhs.type->nullable || rhs.type->nullable;
        bool bothStrings = lhs.type->kind == TypeKind::String &&
                           rhs.type->kind == TypeKind::String;
        // String? == null：指针与 null 比较
        if (bothStrings && eitherNull) {
            std::string reg = emitICmp((op == "==" ? "eq" : "ne"), "ptr",
                                       lhs.ir, rhs.ir);
            std::string ext = emitCast("zext", "i1", reg, "i8");
            lhs = Value(ext, Type::makeBool());
            continue;
        }

        // String / String? 内容相等（hao_str_eq 已处理一侧/双侧 null）
        if (bothStrings) {
            std::string eqr = emitCall("i8", "@hao_str_eq", "ptr " + lhs.ir + ", ptr " + rhs.ir);
            if (op == "==") {
                lhs = Value(eqr, Type::makeBool());
            } else {
                std::string i1r = emitICmp("eq", "i8", eqr, "0");
                std::string ext = emitCast("zext", "i1", i1r, "i8");
                lhs = Value(ext, Type::makeBool());
            }
            continue;
        }

        // null 与任意引用/可空值比较
        if (eitherNull && (lhs.type->llvmType() == "ptr" ||
                           rhs.type->llvmType() == "ptr")) {
            Value ptrSide = lhs.type->isNull() ? rhs : lhs;
            std::string otherIR = lhs.type->isNull() ? rhs.ir : lhs.ir;
            (void)ptrSide;
            std::string reg = emitICmp((op == "==" ? "eq" : "ne"), "ptr",
                                       otherIR, "null");
            std::string ext = emitCast("zext", "i1", reg, "i8");
            lhs = Value(ext, Type::makeBool());
            continue;
        }

        // 不同形态的装箱可空（Int?/Long?）禁止直接比较（对齐赋值拒绝）
        if (lhs.type->isBoxedNullable() && rhs.type->isBoxedNullable() &&
            lhs.type->kind != rhs.type->kind) {
            error(e, "不同宽度的可空类型不能直接比较 '" + op + "'：" +
                     lhs.type->toString() + " 与 " + rhs.type->toString() +
                     "，请先 !! 再比较");
            return Value();
        }

        // 装箱可空值类型（Int?/Bool?/Double?…）：null 短路后按载荷比较
        if (lhs.type->isBoxedNullable() && rhs.type->isBoxedNullable() &&
            lhs.type->kind == rhs.type->kind) {
            std::string bothL = em_.nextLabel("eq.bothnull");
            std::string oneL  = em_.nextLabel("eq.onenull");
            std::string valL  = em_.nextLabel("eq.vals");
            std::string mergeL = em_.nextLabel("eq.merge");

            std::string ln = emitICmp("eq", "ptr", lhs.ir, "null");
            std::string rn = emitICmp("eq", "ptr", rhs.ir, "null");
            std::string bothN = emitBinOp("and", "i1", ln, rn);
            std::string eitherN = emitBinOp("or", "i1", ln, rn);

            std::string entryBlk = em_.currentBlock();
            emitCondBr(bothN, bothL, oneL);

            em_.emitLabel(bothL);
            // 双侧 null：== → true，!= → false
            emitBr(mergeL);
            std::string bothBlk = em_.currentBlock();

            em_.emitLabel(oneL);
            // 恰一侧 null → 不相等；否则比载荷
            emitCondBr(eitherN, mergeL, valL);
            std::string oneBlk = em_.currentBlock();

            em_.emitLabel(valL);
            auto unboxSide = [&](const std::string& ptr) -> std::string {
                switch (lhs.type->kind) {
                    case TypeKind::Long: case TypeKind::ULong: case TypeKind::UIntPtr: {
                        std::string r = emitCall("i64", "@hao_unbox_i64", "ptr " + ptr);
                        return r;
                    }
                    case TypeKind::Double: {
                        std::string r = emitCall("double", "@hao_unbox_f64", "ptr " + ptr);
                        return r;
                    }
                    case TypeKind::Float: {
                        std::string r = emitCall("float", "@hao_unbox_f32", "ptr " + ptr);
                        return r;
                    }
                    case TypeKind::Short: case TypeKind::UShort: {
                        std::string w = emitCall("i32", "@hao_unbox_i32", "ptr " + ptr);
                        std::string r = emitCast("trunc", "i32", w, "i16");
                        return r;
                    }
                    case TypeKind::SByte: case TypeKind::Byte: case TypeKind::Bool: {
                        std::string w = emitCall("i32", "@hao_unbox_i32", "ptr " + ptr);
                        std::string r = emitCast("trunc", "i32", w, "i8");
                        return r;
                    }
                    default: { // Int / UInt / Char
                        std::string r = emitCall("i32", "@hao_unbox_i32", "ptr " + ptr);
                        return r;
                    }
                }
            };
            std::string lv = unboxSide(lhs.ir);
            std::string rv = unboxSide(rhs.ir);
            std::string lty = Type::makeOfKind(lhs.type->kind)->llvmType();
            std::string vc;
            if (lhs.type->kind == TypeKind::Double || lhs.type->kind == TypeKind::Float) {
                vc = emitFCmp("oeq", lty, lv, rv);
            } else {
                vc = emitICmp("eq", lty, lv, rv);
            }
            // != 时在 vals 块内取反，再汇入 phi
            bool isEq = (op == "==");
            std::string valI1 = vc;
            if (!isEq) {
                valI1 = emitBinOp("xor", "i1", vc, "true");
            }
            emitBr(mergeL);
            std::string valBlk = em_.currentBlock();

            em_.emitLabel(mergeL);
            // bothnull: ==→true !=→false；onenull: ==→false !=→true；vals: valI1
            std::string phi = emitPhi("i1",
                "[ " + std::string(isEq ? "true" : "false") +
                ", %" + bothBlk + " ], [ " +
                std::string(isEq ? "false" : "true") + ", %" + oneBlk +
                " ], [ " + valI1 + ", %" + valBlk + " ]");
            (void)entryBlk;
            std::string ext = emitCast("zext", "i1", phi, "i8");
            lhs = Value(ext, Type::makeBool());
            continue;
        }

        // 引用可空（对象?/数组? 等）：指针身份相等
        if (eitherNullable && lhs.type->llvmType() == "ptr" &&
            rhs.type->llvmType() == "ptr") {
            std::string reg = emitICmp((op == "==" ? "eq" : "ne"), "ptr",
                                       lhs.ir, rhs.ir);
            std::string ext = emitCast("zext", "i1", reg, "i8");
            lhs = Value(ext, Type::makeBool());
            continue;
        }

        // 可空与非空混比（如 Bool?==true、Int?==1）：须先 !!
        if (eitherNullable) {
            error(e, "可空类型不能直接与非空值比较 '" + op +
                     "'，请先用 !! 或 ??");
            return Value();
        }

        // v0.50.1：reflect.Class 为每类型单例，== 走下方普通引用指针比较

        // 数值比较：先提升再分配比较结果寄存器（coerce 可能占 temp）
        if (lhs.type->isNumeric() && rhs.type->isNumeric()) {
            if (!ensureNonNullOperand(lhs, e, op) ||
                !ensureNonNullOperand(rhs, e, op))
                return Value();
            TypePtr rt = Type::binaryNumericPromote(lhs.type->kind, rhs.type->kind);
            if (!rt) {
                if (Type::isMixedSignedUnsigned64(lhs.type->kind, rhs.type->kind))
                    error(e, "64 位有符号与无符号不能隐式混合，请显式转换");
                else
                    error(e, "不能比较 " + lhs.type->toString() + " 与 " +
                             rhs.type->toString());
                return Value();
            }
            lhs = coerce(lhs, rt, 0, 0);
            rhs = coerce(rhs, rt, 0, 0);
            std::string reg;
            if (rt->isFloating()) {
                reg = emitFCmp((op == "==" ? "oeq" : "one"), rt->llvmType(),
                               lhs.ir, rhs.ir);
            } else {
                reg = emitICmp((op == "==" ? "eq" : "ne"), rt->llvmType(),
                               lhs.ir, rhs.ir);
            }
            std::string ext = emitCast("zext", "i1", reg, "i8");
            lhs = Value(ext, Type::makeBool());
            continue;
        }

        // Bool / 同类引用等：两侧底层 LLVM 类型必须一致，禁止 true==1 之类
        if (lhs.type->llvmType() != rhs.type->llvmType() ||
            lhs.type->kind != rhs.type->kind) {
            error(e, "不能比较类型不兼容的值 '" + lhs.type->toString() +
                     "' " + op + " '" + rhs.type->toString() + "'");
            return Value();
        }
        {
            std::string ty = lhs.type->llvmType();
            std::string reg = emitICmp((op == "==" ? "eq" : "ne"), ty, lhs.ir, rhs.ir);
            std::string ext = emitCast("zext", "i1", reg, "i8");
            lhs = Value(ext, Type::makeBool());
        }
    }
    return lhs;
}

// ---------- 关系比较 ----------
Value IRGen::genRelational(HaoLangParser::RelationalExprContext* e) {
    // ---------- is / as 类型判定 ----------
    if (e->IS() || e->AS()) {
        Value obj = genShift(e->shiftExpr(0));
        if (!obj.valid()) return Value();

        TypePtr target = resolveType(e->type());
        bool isIs = (e->IS() != nullptr);

        if (obj.type->kind != TypeKind::Class &&
            obj.type->kind != TypeKind::Interface) {
            error(e, std::string(isIs ? "is" : "as") +
                     " 只能用于类或接口类型，实际为 " + obj.type->toString());
            return Value();
        }
        if (target->kind != TypeKind::Class && target->kind != TypeKind::Interface) {
            error(e, std::string(isIs ? "is" : "as") +
                     " 的目标必须是类或接口，实际为 " + target->toString());
            return Value();
        }

        // 编译期可判定的情形：目标是自身或父类型，判定恒为真
        if (isAssignable(obj.type, target)) {
            if (isIs) {
                diags_.warning(e->getStart()->getLine(),
                               e->getStart()->getCharPositionInLine(),
                               obj.type->toString() + " 一定是 " +
                               target->toString() + "，该 is 判定恒为真");
                return Value("1", Type::makeBool());
            }
            // as 向上转型：运行时无需任何操作
            return Value(obj.ir, target);
        }

        auto it = typeIdLists_.find(target->className);
        if (it == typeIdLists_.end()) {
            // 目标类型没有任何可实例化的实现，判定恒为假
            if (isIs) {
                diags_.warning(e->getStart()->getLine(),
                               e->getStart()->getCharPositionInLine(),
                               "没有任何可实例化的类型是 " + target->toString() +
                               "，该 is 判定恒为假");
                return Value("0", Type::makeBool());
            }
            error(e, "无法转换为 " + target->toString() +
                     "：没有任何可实例化的类型满足它");
            return Value();
        }

        std::string chk = emitCall("i8", "@hao_type_is", "ptr " + obj.ir + ", ptr " + it->second);

        if (isIs) return Value(chk, Type::makeBool());

        // ---- as 向下转型：失败即 panic（对标 Java 的强制转换）----
        std::string okL   = em_.nextLabel("cast.ok");
        std::string failL = em_.nextLabel("cast.fail");
        std::string cond  = em_.nextTemp();
        cond = emitICmp("ne", "i8", chk, "0");
        emitCondBr(cond, okL, failL);

        em_.emitLabel(failL);
        std::string msg = em_.internString(target->toString());
        emitCallVoid("@hao_panic_cast", "ptr " + msg);
        emitUnreachable();

        em_.emitLabel(okL);
        blockTerminated_ = false;
        // 转换成功后指针不变，只是静态类型改变
        return Value(obj.ir, target);
    }

    auto shifts = e->shiftExpr();
    if (shifts.size() == 1) return genShift(shifts[0]);

    Value lhs = genShift(shifts[0]);
    if (!lhs.valid()) return Value();

    for (size_t i = 1; i < shifts.size(); ++i) {
        Value rhs = genShift(shifts[i]);
        if (!rhs.valid()) return Value();

        std::string op = e->children[2 * i - 1]->getText();

        if (!lhs.type->isNumeric() || !rhs.type->isNumeric()) {
            error(e, "运算符 '" + op + "' 不能用于 " + lhs.type->toString() +
                     " 与 " + rhs.type->toString());
            return Value();
        }
        if (!ensureNonNullOperand(lhs, e, op) ||
            !ensureNonNullOperand(rhs, e, op))
            return Value();

        TypePtr rt = Type::binaryNumericPromote(lhs.type->kind, rhs.type->kind);
        if (!rt) {
            if (Type::isMixedSignedUnsigned64(lhs.type->kind, rhs.type->kind))
                error(e, "64 位有符号与无符号不能隐式混合，请显式转换");
            else
                error(e, "运算符 '" + op + "' 不能用于 " + lhs.type->toString() +
                         " 与 " + rhs.type->toString());
            return Value();
        }
        lhs = coerce(lhs, rt, 0, 0);
        rhs = coerce(rhs, rt, 0, 0);
        std::string reg;
        if (rt->isFloating()) {
            std::string pred = (op == "<") ? "olt" : (op == ">") ? "ogt"
                             : (op == "<=") ? "ole" : "oge";
            reg = emitFCmp(pred, rt->llvmType(), lhs.ir, rhs.ir);
        } else {
            bool un = rt->isUnsigned();
            std::string pred;
            if (op == "<")       pred = un ? "ult" : "slt";
            else if (op == ">")  pred = un ? "ugt" : "sgt";
            else if (op == "<=") pred = un ? "ule" : "sle";
            else                 pred = un ? "uge" : "sge";
            reg = emitICmp(pred, rt->llvmType(), lhs.ir, rhs.ir);
        }
        std::string ext = emitCast("zext", "i1", reg, "i8");
        lhs = Value(ext, Type::makeBool());
    }
    return lhs;
}

// ---------- 移位 ----------
Value IRGen::genShift(HaoLangParser::ShiftExprContext* e) {
    auto adds = e->additiveExpr();
    if (adds.size() == 1) return genAdditive(adds[0]);

    // children: additive ((LSHIFT | GT GT) additive)*
    // >> 拆成两个 GT，避免与嵌套泛型 List<List<Int>> 冲突
    Value lhs = genAdditive(adds[0]);
    if (!lhs.valid()) return Value();
    size_t childIdx = 1;
    for (size_t i = 1; i < adds.size(); ++i) {
        if (childIdx >= e->children.size()) {
            error(e, "移位表达式结构异常");
            return Value();
        }
        std::string t = e->children[childIdx]->getText();
        std::string op;
        if (t == "<<") {
            op = "<<";
            childIdx += 1;
        } else if (t == ">") {
            childIdx += 1;
            if (childIdx >= e->children.size() ||
                e->children[childIdx]->getText() != ">") {
                error(e, "无效的移位运算符");
                return Value();
            }
            op = ">>";
            childIdx += 1;
        } else {
            error(e, "无效的移位运算符 '" + t + "'");
            return Value();
        }

        Value rhs = genAdditive(adds[i]);
        if (!rhs.valid()) return Value();
        if (!lhs.type->isInteger() || !rhs.type->isInteger()) {
            error(e, "运算符 '" + op + "' 要求整数类型，实际为 " +
                     lhs.type->toString() + " 与 " + rhs.type->toString());
            return Value();
        }
        if (!ensureNonNullOperand(lhs, e, op) ||
            !ensureNonNullOperand(rhs, e, op))
            return Value();
        TypePtr rt = Type::unaryBitwisePromote(lhs.type->kind);
        lhs = coerce(lhs, rt, 0, 0);
        // 移位量：提到与 lhs 同宽后按 Java 掩码（32 位 &31，64 位 &63）
        rhs = coerce(rhs, rt, 0, 0);
        int maskBits = Type::shiftMaskBits(rt->kind);
        int mask = (1 << maskBits) - 1;
        std::string masked = emitBinOp("and", rt->llvmType(), rhs.ir, std::to_string(mask));
        std::string irOp;
        if (op == "<<") irOp = "shl";
        else irOp = rt->isUnsigned() ? "lshr" : "ashr";
        std::string reg = emitBinOp(irOp, rt->llvmType(), lhs.ir, masked);
        lhs = Value(reg, rt);
        childIdx += 1; // 跳过本段右侧 additive
    }
    return lhs;
}

// ---------- 加减 ----------
Value IRGen::genAdditive(HaoLangParser::AdditiveExprContext* e) {
    auto muls = e->multiplicativeExpr();
    if (muls.size() == 1) return genMultiplicative(muls[0]);

    Value lhs = genMultiplicative(muls[0]);
    if (!lhs.valid()) return Value();

    for (size_t i = 1; i < muls.size(); ++i) {
        /* 左值必须先于右操作数挂根（右求值可 safepoint） */
        rootGcOperand(lhs);
        Value rhs = genMultiplicative(muls[i]);
        if (!rhs.valid()) return Value();

        std::string op = e->children[2 * i - 1]->getText();

        // 字符串 + 任意类型 => 拼接（可空 String? 须先 !!）
        if (op == "+" && (lhs.type->kind == TypeKind::String ||
                          rhs.type->kind == TypeKind::String)) {
            if (!ensureNonNullOperand(lhs, e, "+") ||
                !ensureNonNullOperand(rhs, e, "+"))
                return Value();
            Value ls = toStringValue(lhs);
            if (!ls.valid()) {
                error(e, "无法将 " + lhs.type->toString() + " 与 " +
                         rhs.type->toString() + " 用 '+' 连接");
                return Value();
            }
            rootGcOperand(ls);
            Value rs = toStringValue(rhs);
            if (!rs.valid()) {
                error(e, "无法将 " + lhs.type->toString() + " 与 " +
                         rhs.type->toString() + " 用 '+' 连接");
                return Value();
            }
            rootGcOperand(rs);
            std::string reg = emitCall("ptr", "@hao_str_concat", "ptr " + ls.ir + ", ptr " + rs.ir);
            lhs = Value(reg, Type::makeString());
            rootGcOperand(lhs); /* 拼接结果跨后续右操作数/调用 */
            continue;
        }

        if (!lhs.type->isNumeric() || !rhs.type->isNumeric()) {
            error(e, "运算符 '" + op + "' 不能用于 " + lhs.type->toString() +
                     " 与 " + rhs.type->toString());
            return Value();
        }
        if (!ensureNonNullOperand(lhs, e, op) ||
            !ensureNonNullOperand(rhs, e, op))
            return Value();

        TypePtr rt = Type::binaryNumericPromote(lhs.type->kind, rhs.type->kind);
        if (!rt) {
            if (Type::isMixedSignedUnsigned64(lhs.type->kind, rhs.type->kind))
                error(e, "64 位有符号与无符号不能隐式混合，请显式转换");
            else
                error(e, "运算符 '" + op + "' 不能用于 " + lhs.type->toString() +
                         " 与 " + rhs.type->toString());
            return Value();
        }
        lhs = coerce(lhs, rt, 0, 0);
        rhs = coerce(rhs, rt, 0, 0);

        bool isF = rt->isFloating();
        std::string irOp = isF ? (op == "+" ? "fadd" : "fsub")
                               : (op == "+" ? "add"  : "sub");
        std::string reg = emitBinOp(irOp, rt->llvmType(), lhs.ir, rhs.ir);
        lhs = Value(reg, rt);
    }
    return lhs;
}

// ---------- 乘除模 ----------
Value IRGen::genMultiplicative(HaoLangParser::MultiplicativeExprContext* e) {
    auto uns = e->unaryExpr();
    if (uns.size() == 1) return genUnary(uns[0]);

    Value lhs = genUnary(uns[0]);
    if (!lhs.valid()) return Value();

    for (size_t i = 1; i < uns.size(); ++i) {
        Value rhs = genUnary(uns[i]);
        if (!rhs.valid()) return Value();

        std::string op = e->children[2 * i - 1]->getText();

        if (!lhs.type->isNumeric() || !rhs.type->isNumeric()) {
            error(e, "运算符 '" + op + "' 不能用于 " + lhs.type->toString() +
                     " 与 " + rhs.type->toString());
            return Value();
        }
        if (!ensureNonNullOperand(lhs, e, op) ||
            !ensureNonNullOperand(rhs, e, op))
            return Value();

        TypePtr rt = Type::binaryNumericPromote(lhs.type->kind, rhs.type->kind);
        if (!rt) {
            if (Type::isMixedSignedUnsigned64(lhs.type->kind, rhs.type->kind))
                error(e, "64 位有符号与无符号不能隐式混合，请显式转换");
            else
                error(e, "运算符 '" + op + "' 不能用于 " + lhs.type->toString() +
                         " 与 " + rhs.type->toString());
            return Value();
        }
        lhs = coerce(lhs, rt, 0, 0);
        rhs = coerce(rhs, rt, 0, 0);

        bool isF = rt->isFloating();
        std::string irOp;
        if      (op == "*") irOp = isF ? "fmul" : "mul";
        else if (op == "/") {
            if (isF) irOp = "fdiv";
            else irOp = rt->isUnsigned() ? "udiv" : "sdiv";
        } else {
            if (isF) irOp = "frem";
            else irOp = rt->isUnsigned() ? "urem" : "srem";
        }

        if (!isF && (op == "/" || op == "%")) {
            emitIntDivZeroCheck(rhs);
            if (!rt->isUnsigned())
                rhs = emitSafeSignedDivisor(lhs, rhs);
        }

        std::string reg = emitBinOp(irOp, rt->llvmType(), lhs.ir, rhs.ir);
        lhs = Value(reg, rt);
    }
    return lhs;
}

// ---------- 一元 ----------
Value IRGen::genUnary(HaoLangParser::UnaryExprContext* e) {
    if (auto* pf = e->postfixExpr()) return genPostfix(pf);

    std::string op = e->children[0]->getText();
    Value v = genUnary(e->unaryExpr());
    if (!v.valid()) return Value();

    if (op == "-") {
        if (!v.type->isNumeric()) {
            error(e, "一元 '-' 不能用于 " + v.type->toString());
            return Value();
        }
        if (!ensureNonNullOperand(v, e, "-")) return Value();
        // 窄整数一元取负至少升到 Int
        if (v.type->isInteger() && Type::bitWidthBits(v.type->kind) < 32)
            v = coerce(v, Type::makeInt(), 0, 0);
        std::string reg;
        if (v.type->isFloating())
            reg = emitFNeg(v.type->llvmType(), v.ir);
        else
            reg = emitBinOp("sub", v.type->llvmType(), "0", v.ir);
        return Value(reg, v.type);
    }

    if (op == "+") {
        if (!v.type->isNumeric()) {
            error(e, "一元 '+' 不能用于 " + v.type->toString());
            return Value();
        }
        if (!ensureNonNullOperand(v, e, "+")) return Value();
        if (v.type->isInteger() && Type::bitWidthBits(v.type->kind) < 32)
            return coerce(v, Type::makeInt(), 0, 0);
        return v;
    }

    if (op == "~") {
        if (!v.type->isInteger()) {
            error(e, "运算符 '~' 要求整数类型，实际为 " + v.type->toString());
            return Value();
        }
        if (!ensureNonNullOperand(v, e, "~")) return Value();
        if (Type::bitWidthBits(v.type->kind) < 32)
            v = coerce(v, Type::makeInt(), 0, 0);
        std::string reg = emitBinOp("xor", v.type->llvmType(), v.ir, "-1");
        return Value(reg, v.type);
    }

    if (op == "!") {
        if (v.type->kind != TypeKind::Bool) {
            error(e, "运算符 '!' 要求 Bool 类型，实际为 " + v.type->toString());
            return Value();
        }
        if (!ensureNonNullOperand(v, e, "!")) return Value();
        std::string i1 = emitICmp("eq", "i8", v.ir, "0");
        std::string ext = emitCast("zext", "i1", i1, "i8");
        return Value(ext, Type::makeBool());
    }

    error(e, "当前版本尚不支持一元运算符 '" + op + "'");
    return Value();
}
// ============================================================
//  后缀表达式
// ------------------------------------------------------------
//  形如 a.b[0].c().d 的链式表达式，由 genPostfix 逐个操作归约：
//  每一步的结果作为下一步的接收者。这样任意长度的链都能处理。
//
//  两处需要在归约之前特判，因为它们的接收者不是普通值：
//    fmt.println(...)  —— fmt 是内建命名空间，没有对应的对象
//    super.m(...)      —— 需静态绑定父类实现，不能走虚表
// ============================================================

// ---------- base.field ----------
Value IRGen::applyMemberAccess(const Value& base, const std::string& field,
                               antlr4::ParserRuleContext* ctx) {
    // 可空接收者不能直接取成员，必须用 ?. 或 !!
    if (base.type->nullable) {
        error(ctx, "不能对可空类型 " + base.type->toString() +
                   " 直接访问成员 '" + field + "'，请使用 '?.' 或 '!!'");
        return Value();
    }
    // String / 数组的 length（C 仍返回 i64，截断为 Int）
    if (field == "length") {
        if (base.type->kind == TypeKind::String) {
            Value b = base;
            rootGcOperand(b);
            std::string wide = emitCall("i64", "@hao_str_len", "ptr " + b.ir);
            std::string reg = emitCast("trunc", "i64", wide, "i32");
            return Value(reg, Type::makeInt());
        }
        if (base.type->kind == TypeKind::Array) {
            Value b = base;
            rootGcOperand(b);
            std::string wide = emitCall("i64", "@hao_array_len", "ptr " + b.ir);
            std::string reg = emitCast("trunc", "i64", wide, "i32");
            return Value(reg, Type::makeInt());
        }
    }

    // 对象字段读取
    if (base.type->kind == TypeKind::Class) {
        auto ci = classOfType(base.type);
        if (!ci) {
            error(ctx, "未定义的类 '" + base.type->toString() + "'");
            return Value();
        }
        // 静态字段：ClassName.X / obj.X —— 类级全局变量，忽略实例指针 base.ir
        // （含编译器合成的 static Class，对标 Java 类字面量 / 类型身份）
        if (const FieldInfo* sfi = ci->findStaticField(field)) {
            if (!canAccessMember(sfi->visibility, sfi->ownerClass)) {
                error(ctx, std::string("不能访问 ") + visName(sfi->visibility) +
                           " 静态字段 '" + sfi->ownerClass + "." + field + "'");
                return Value();
            }
            emitStaticEnsureInit(ci);
            std::string reg = emitLoad(sfi->type->llvmType(),
                           "@" + ci->name + "." + sfi->name);
            return Value(reg, sfi->type);
        }
        if (const FieldInfo* fi = ci->findField(field)) {
            // 类名（base.ir 为空）访问实例字段非法
            if (base.ir.empty()) {
                error(ctx, "静态上下文不能访问实例字段 '" + field +
                           "'（请改用静态字段或持有实例对象访问）");
                return Value();
            }
            if (!canAccessMember(fi->visibility, fi->ownerClass)) {
                error(ctx, std::string("不能访问 ") + visName(fi->visibility) +
                           " 字段 '" + fi->ownerClass + "." + field + "'");
                return Value();
            }
            std::string fp = fieldPtr(base.ir, fi->slot);
            std::string reg = emitLoad(fi->type->llvmType(), fp);
            return Value(reg, fi->type);
        }
        // 方法组转换（v0.19.0）：obj.method 作为函数值，绑定 this。
        if (const MethodInfo* mmi = ci->findMethod(field)) {
            if (base.ir.empty()) {
                error(ctx, "静态上下文不能把实例方法 '" + field +
                           "' 当函数值（请改用静态方法或持有实例对象）");
                return Value();
            }
            if (!canAccessMember(mmi->visibility, mmi->ownerClass)) {
                error(ctx, std::string("不能访问 ") + visName(mmi->visibility) +
                           " 方法 '" + mmi->ownerClass + "." + field + "'");
                return Value();
            }
            std::string wname = ensureMethodWrapper(ci, *mmi);
            Value baseR = base;
            rootGcOperand(baseR); /* this 跨 env 分配 */
            // slot0=fnptr 非 GC；slot1=this
            std::string env = emitObjectNew(2, 2);
            std::string envSlot = emitSpillGcRoot("mwrap.env", env);
            env = emitLoad("ptr", envSlot);
            std::string fp0 = fieldPtr(env, 0);
            emitStore("ptr", wname, fp0);
            std::string fp1 = fieldPtr(env, 1);
            emitHeapStore(fp1, baseR.ir, baseR.type, env);
            return Value(env, Type::makeFunc(mmi->paramTypes, mmi->returnType));
        }
        if (!ci->findStaticMethods(field).empty()) {
            error(ctx, "静态方法 '" + field + "' 需要调用，请写作 " + field + "()");
            return Value();
        }
        error(ctx, "类 '" + ci->name + "' 没有成员 '" + field + "'");
        return Value();
    }

    if (base.type->kind == TypeKind::Interface) {
        error(ctx, "不能通过接口访问字段 '" + field + "'（接口只有方法）");
        return Value();
    }

    error(ctx, base.type->toString() + " 类型没有成员 '" + field + "'");
    return Value();
}

// ---------- base[idx] ----------
Value IRGen::applyIndex(const Value& base, HaoLangParser::IndexOpContext* io,
                        antlr4::ParserRuleContext* ctx) {
    if (base.type->nullable) {
        error(ctx, "不能对可空类型 " + base.type->toString() +
                   " 直接使用下标，请使用 '!!'");
        return Value();
    }
    Value baseR = base;
    rootGcOperand(baseR);
    // String[i] → Char（码点下标）
    if (baseR.type->kind == TypeKind::String) {
        Value idx = genExpr(io->expr());
        if (!idx.valid()) return Value();
        if (!idx.type->isInteger()) {
            error(io->expr(), "字符串索引必须是整数类型，实际为 " + idx.type->toString());
            return Value();
        }
        if (!ensureNonNullOperand(idx, io->expr(), "字符串下标")) return Value();
        // 与数组下标一致：保留 i64，避免 Long 经 Int 截断后静默错位
        std::string idx64 = indexAsI64(idx);
        std::string reg = emitCall("i32", "@hao_str_char_at", "ptr " + baseR.ir + ", i64 " + idx64);
        return Value(reg, Type::makeChar());
    }

    if (baseR.type->kind != TypeKind::Array) {
        error(ctx, "只能对数组或字符串使用索引访问，实际为 " + baseR.type->toString());
        return Value();
    }
    TypePtr elemType = baseR.type->elem ? baseR.type->elem : Type::makeInt();

    Value idx = genExpr(io->expr());
    if (!idx.valid()) return Value();
    if (!idx.type->isInteger()) {
        error(io->expr(), "数组索引必须是整数类型，实际为 " + idx.type->toString());
        return Value();
    }
    if (!ensureNonNullOperand(idx, io->expr(), "数组下标")) return Value();

    std::string ptr = arrayElemPtr(baseR, idx);
    std::string reg = emitLoad(elemType->llvmType(), ptr);
    return Value(reg, elemType);
}

// ---------- base.method(args) ----------
Value IRGen::applyMethodCall(const Value& recv, const std::string& method,
                             HaoLangParser::CallOpContext* call,
                             antlr4::ParserRuleContext* ctx) {
    // 可空接收者不能直接调方法，必须用 ?. 或 !!
    if (recv.type->nullable) {
        error(ctx, "不能对可空类型 " + recv.type->toString() +
                   " 直接调用方法 '" + method + "'，请使用 '?.' 或 '!!'");
        return Value();
    }
    /* recv 跨后续实参求值 / safepoint 须在 shadow */
    Value recvR = recv;
    rootGcOperand(recvR);

    // ===== 数组内建方法 =====
    if (recvR.type->kind == TypeKind::Array) {
        if (method == "pop") {
            if (call->argList()) {
                error(ctx, "数组的 pop() 方法不接受参数");
                return Value();
            }
            TypePtr elemType = recvR.type->elem ? recvR.type->elem : Type::makeInt();
            std::string raw = emitCall("i64", "@hao_array_pop", "ptr " + recvR.ir);
            std::string reg = em_.unboxFromI64(raw, elemType);
            return Value(reg, elemType);
        }
        error(ctx, "数组没有方法 '" + method + "'（当前仅支持 pop()）");
        return Value();
    }

    // ===== 接口方法：经虚表动态分派 =====
    if (recvR.type->kind == TypeKind::Interface) {
        // 泛型接口按实例名解析（Iterable<Int> -> Iterable$Int，槽位继承模板）
        std::string iname = recvR.type->typeArgs.empty()
            ? recvR.type->className
            : recvR.type->monoName();
        auto ii = lookupInterface(iname);
        if (!ii) {
            error(ctx, "未定义的接口 '" + iname + "'");
            return Value();
        }
        const MethodInfo* im = ii->findMethod(method);
        if (!im) {
            error(ctx, "接口 '" + ii->name + "' 没有方法 '" + method + "'");
            return Value();
        }

        std::vector<Value> args;
        std::vector<antlr4::tree::ParseTree*> argExprs;
        if (auto* al = call->argList())
            for (size_t k = 0; k < al->arg().size(); ++k) {
                if (k < im->paramTypes.size())
                    expectedTypes_.push_back(im->paramTypes[k]);
                auto* aex = al->arg(k)->expr();
                Value av = genExpr(aex);
                if (k < im->paramTypes.size()) expectedTypes_.pop_back();
                if (!av.valid()) return Value();
                rootGcOperand(av);
                args.push_back(av);
                argExprs.push_back(aex);
            }
        if (args.size() != im->paramTypes.size()) {
            error(ctx, "接口方法 '" + ii->name + "." + method + "' 需要 " +
                       std::to_string(im->paramTypes.size()) + " 个参数，实际提供 " +
                       std::to_string(args.size()) + " 个");
            return Value();
        }

        // 取虚表指针（对象槽位 0），再按槽位取函数指针
        std::string vtp = emitGep("ptr", recvR.ir, "i64", "0");
        std::string vt = emitLoad("ptr", vtp);
        std::string mp = emitGep("ptr", vt, "i64", std::to_string(im->vtableSlot));
        std::string fp = emitLoad("ptr", mp);

        std::string argStr = "ptr " + recvR.ir;
        std::string sigTypes = "ptr";
        for (size_t i = 0; i < args.size(); ++i) {
            if (!isAssignable(args[i].type, im->paramTypes[i])) {
                error(ctx, "接口方法 '" + ii->name + "." + method + "' 第 " +
                           std::to_string(i + 1) + " 个参数类型不匹配：期望 " +
                           im->paramTypes[i]->toString() +
                           "，实际 " + args[i].type->toString());
                return Value();
            }
            argStr  += ", " + formatCallArg(im->paramTypes[i], argExprs[i], args[i]);
            sigTypes += ", " + im->paramTypes[i]->llvmType();
        }

        pinRuntimeCallSite(ctx);
        // 间接调用需显式写出函数类型
        std::string fnTy = im->returnType->llvmType() + " (" + sigTypes + ")";
        if (im->returnType->isUnit()) {
            emitCallTyped(fnTy, fp, argStr);
            return Value("", Type::makeUnit());
        }
        std::string reg = emitCall(fnTy, fp, argStr);
        return Value(reg, im->returnType);
    }

    // ===== 类方法 =====
    if (recvR.type->kind != TypeKind::Class) {
        error(ctx, recvR.type->toString() + " 类型没有方法 '" + method + "'");
        return Value();
    }
    auto ci = classOfType(recvR.type);
    if (!ci) {
        error(ctx, "未定义的类 '" + recvR.type->toString() + "'");
        return Value();
    }
    if (ci->isGenericTemplate()) {
        error(ctx, "泛型类 '" + ci->name + "' 必须提供类型参数才能使用");
        return Value();
    }
    // 静态方法：ClassName.f(args) / obj.f(args) —— 按签名重载选最佳
    {
        auto staticCands = ci->findStaticMethods(method);
        if (!staticCands.empty()) {
            emitStaticEnsureInit(ci);
            std::vector<Value> args;
            std::vector<antlr4::tree::ParseTree*> argExprs;
            if (auto* al = call->argList())
                for (auto* a : al->arg()) {
                    Value av = genExpr(a->expr());
                    if (!av.valid()) return Value();
                    rootGcOperand(av);
                    args.push_back(av);
                    argExprs.push_back(a->expr());
                }
            const MethodInfo* smi = selectStaticOverload(
                staticCands, args, ctx, ci->name + "." + method);
            if (!smi) return Value();
            if (!canAccessMember(smi->visibility, smi->ownerClass)) {
                error(ctx, std::string("不能访问 ") + visName(smi->visibility) +
                           " 静态方法 '" + smi->ownerClass + "." + method + "'");
                return Value();
            }
            std::string argStr;
            for (size_t k = 0; k < args.size(); ++k) {
                if (!isAssignable(args[k].type, smi->paramTypes[k])) {
                    error(ctx, "静态方法 '" + ci->name + "." + method + "' 第 " +
                               std::to_string(k + 1) + " 个参数类型不匹配：期望 " +
                               smi->paramTypes[k]->toString() + "，实际 " +
                               args[k].type->toString());
                    return Value();
                }
                args[k] = coerce(args[k], smi->paramTypes[k], 0, 0);
                if (k) argStr += ", ";
                argStr += formatCallArg(smi->paramTypes[k], argExprs[k], args[k]);
            }
            pinRuntimeCallSite(ctx);
            if (smi->returnType->isUnit()) {
                emitCallVoid(smi->irName, argStr);
                return Value("", Type::makeUnit());
            }
            std::string reg = emitCall(smi->returnType->llvmType(), smi->irName, argStr);
            return Value(reg, smi->returnType);
        }
    }
    // 静态 Func/Action 字段：ClassName.fn(args)
    if (const FieldInfo* sff = ci->findStaticField(method)) {
        if (sff->type->kind == TypeKind::Func) {
            if (!canAccessMember(sff->visibility, sff->ownerClass)) {
                error(ctx, std::string("不能访问 ") + visName(sff->visibility) +
                           " 静态字段 '" + sff->ownerClass + "." + method + "'");
                return Value();
            }
            emitStaticEnsureInit(ci);
            std::string fenv = emitLoad("ptr", "@" + ci->name + "." + sff->name);
            Value fenvV(fenv, sff->type);
            rootGcOperand(fenvV); /* Func env 跨实参求值 */
            fenv = fenvV.ir;
            TypePtr fnRet = sff->type->elem ? sff->type->elem : Type::makeUnit();
            const auto& fnParams = sff->type->params;
            std::vector<Value> args;
            std::vector<antlr4::tree::ParseTree*> argExprs;
            if (auto* al = call->argList())
                for (auto* a : al->arg()) {
                    Value av = genExpr(a->expr());
                    if (!av.valid()) return Value();
                    rootGcOperand(av);
                    args.push_back(av);
                    argExprs.push_back(a->expr());
                }
            if (args.size() != fnParams.size()) {
                error(ctx, "静态函数字段 '" + method + "' 需要 " +
                           std::to_string(fnParams.size()) +
                           " 个参数，实际提供 " +
                           std::to_string(args.size()) + " 个");
                return Value();
            }
            std::string fpp = fieldPtr(fenv, 0);
            std::string fp = emitLoad("ptr", fpp);
            std::string argStr = "ptr " + fenv;
            for (size_t k = 0; k < args.size(); ++k) {
                if (!isAssignable(args[k].type, fnParams[k])) {
                    error(ctx, "静态函数字段 '" + method + "' 第 " +
                               std::to_string(k + 1) +
                               " 个参数类型不匹配：期望 " +
                               fnParams[k]->toString() + "，实际 " +
                               args[k].type->toString());
                    return Value();
                }
                args[k] = coerce(args[k], fnParams[k], 0, 0);
                argStr += ", " + formatCallArg(fnParams[k], argExprs[k], args[k]);
            }
            if (fnRet->isUnit()) {
                emitCallVoid(fp, argStr);
                return Value("", Type::makeUnit());
            }
            std::string reg = emitCall(fnRet->llvmType(), fp, argStr);
            return Value(reg, fnRet);
        }
    }
    // Instance overload: arity + assignable types (mirror static)
    auto instCands = ci->findMethods(method);
    const MethodInfo* mi = nullptr;
    if (instCands.empty()) {

        // 泛型方法（v0.9.0）：方法级类型参数在调用时从实参推断，
        // 如 List<Int>.map<R>(f) 的 R。
        std::string tplName = ci->instanceOf.empty() ? ci->name : ci->instanceOf;
        auto gmit = genericMethods_.find(tplName + "." + method);
        if (gmit != genericMethods_.end())
            return callGenericMethod(recvR, ci, gmit->second, call, ctx);
        // Func/Action 字段：obj.fn(args) 等价于取字段再间接调用
        if (const FieldInfo* ffi = ci->findField(method)) {
            if (ffi->type->kind == TypeKind::Func) {
                if (recvR.ir.empty()) {
                    error(ctx, "静态上下文不能调用实例字段 '" + method + "'");
                    return Value();
                }
                if (!canAccessMember(ffi->visibility, ffi->ownerClass)) {
                    error(ctx, std::string("不能访问 ") + visName(ffi->visibility) +
                               " 字段 '" + ffi->ownerClass + "." + method + "'");
                    return Value();
                }
                std::string ffp = fieldPtr(recvR.ir, ffi->slot);
                std::string fenv = emitLoad("ptr", ffp);
                Value fenvV(fenv, ffi->type);
                rootGcOperand(fenvV); /* Func env 跨实参求值 */
                fenv = fenvV.ir;
                TypePtr fnRet = ffi->type->elem ? ffi->type->elem : Type::makeUnit();
                const auto& fnParams = ffi->type->params;
                std::vector<Value> args;
                std::vector<antlr4::tree::ParseTree*> argExprs;
                if (auto* al = call->argList())
                    for (auto* a : al->arg()) {
                        Value av = genExpr(a->expr());
                        if (!av.valid()) return Value();
                        rootGcOperand(av);
                        args.push_back(av);
                        argExprs.push_back(a->expr());
                    }
                if (args.size() != fnParams.size()) {
                    error(ctx, "函数字段 '" + method + "' 需要 " +
                               std::to_string(fnParams.size()) +
                               " 个参数，实际提供 " +
                               std::to_string(args.size()) + " 个");
                    return Value();
                }
                std::string fpp = fieldPtr(fenv, 0);
                std::string fp = emitLoad("ptr", fpp);
                std::string argStr = "ptr " + fenv;
                for (size_t k = 0; k < args.size(); ++k) {
                    if (!isAssignable(args[k].type, fnParams[k])) {
                        error(ctx, "函数字段 '" + method + "' 第 " +
                                   std::to_string(k + 1) +
                                   " 个参数类型不匹配：期望 " +
                                   fnParams[k]->toString() + "，实际 " +
                                   args[k].type->toString());
                        return Value();
                    }
                    args[k] = coerce(args[k], fnParams[k], 0, 0);
                    argStr += ", " + formatCallArg(fnParams[k], argExprs[k], args[k]);
                }
                if (fnRet->isUnit()) {
                    emitCallVoid(fp, argStr);
                    return Value("", Type::makeUnit());
                }
                std::string reg = emitCall(fnRet->llvmType(), fp, argStr);
                return Value(reg, fnRet);
            }
            error(ctx, "'" + method + "' 是字段而非方法，不能调用");
            return Value();
        }
        error(ctx, "类 '" + ci->name + "' 没有方法 '" + method + "'");
        return Value();
    }

    std::vector<Value> args;
    std::vector<antlr4::tree::ParseTree*> argExprs;
    if (instCands.size() == 1) {
        // Single candidate: push expectedTypes so lambdas infer (e.g. ()->Unit)
        mi = instCands[0];
        if (auto* al = call->argList())
            for (size_t k = 0; k < al->arg().size(); ++k) {
                if (k < mi->paramTypes.size())
                    expectedTypes_.push_back(mi->paramTypes[k]);
                auto* aex = al->arg(k)->expr();
                Value av = genExpr(aex);
                if (k < mi->paramTypes.size()) expectedTypes_.pop_back();
                if (!av.valid()) return Value();
                rootGcOperand(av);
                args.push_back(av);
                argExprs.push_back(aex);
            }
    } else {
        // Overload: evaluate without expected, then select
        if (auto* al = call->argList())
            for (auto* a : al->arg()) {
                Value av = genExpr(a->expr());
                if (!av.valid()) return Value();
                rootGcOperand(av);
                args.push_back(av);
                argExprs.push_back(a->expr());
            }
        mi = selectStaticOverload(instCands, args, ctx, ci->name + "." + method);
        if (!mi) return Value();
    }

    // 类名（recv.ir 为空）调用实例方法非法
    if (recvR.ir.empty()) {
        error(ctx, "静态上下文不能调用实例方法 '" + method +
                   "'（请改用静态方法或持有实例对象调用）");
        return Value();
    }
    if (!canAccessMember(mi->visibility, mi->ownerClass)) {
        error(ctx, std::string("不能访问 ") + visName(mi->visibility) +
                   " 方法 '" + mi->ownerClass + "." + method + "'");
        return Value();
    }

    if (args.size() != mi->paramTypes.size()) {
        error(ctx, "方法 '" + ci->name + "." + method + "' 需要 " +
                   std::to_string(mi->paramTypes.size()) + " 个参数，实际提供 " +
                   std::to_string(args.size()) + " 个");
        return Value();
    }

    // 抽象方法没有实现体，只能通过虚表分派到子类实现
    if (mi->isAbstract && (mi->vtableSlot < 0 || !ci->hasVTable)) {
        error(ctx, "抽象方法 '" + ci->name + "." + method + "' 没有可分派的实现");
        return Value();
    }

    std::string argStr = "ptr " + recvR.ir;
    std::string sigTypes = "ptr";
    for (size_t i = 0; i < args.size(); ++i) {
        if (!isAssignable(args[i].type, mi->paramTypes[i])) {
            error(ctx, "方法 '" + ci->name + "." + method + "' 第 " +
                       std::to_string(i + 1) + " 个参数类型不匹配：期望 " +
                       mi->paramTypes[i]->toString() +
                       "，实际 " + args[i].type->toString());
            return Value();
        }
        argStr  += ", " + formatCallArg(mi->paramTypes[i], argExprs[i], args[i]);
        sigTypes += ", " + mi->paramTypes[i]->llvmType();
    }

    pinRuntimeCallSite(ctx);

    // ---- 虚方法：经虚表分派 ----
    //  有虚表槽位说明该方法处在继承体系中且可能被覆写，必须动态分派，
    //  否则父类型引用会调到父类实现。super.m() 由专门分支处理。
    if (mi->vtableSlot >= 0 && ci->hasVTable) {
        std::string vtp = emitGep("ptr", recvR.ir, "i64", "0");
        std::string vt = emitLoad("ptr", vtp);
        std::string mp = emitGep("ptr", vt, "i64", std::to_string(mi->vtableSlot));
        std::string fp = emitLoad("ptr", mp);

        std::string fnTy = mi->returnType->llvmType() + " (" + sigTypes + ")";
        if (mi->returnType->isUnit()) {
            emitCallTyped(fnTy, fp, argStr);
            return Value("", Type::makeUnit());
        }
        std::string reg = emitCall(fnTy, fp, argStr);
        return Value(reg, mi->returnType);
    }

    // ---- 非虚方法：静态调用，零开销 ----
    if (mi->returnType->isUnit()) {
        emitCallVoid(mi->irName, argStr);
        return Value("", Type::makeUnit());
    }
    std::string reg = emitCall(mi->returnType->llvmType(), mi->irName, argStr);
    return Value(reg, mi->returnType);
}

// ============================================================
//  空安全：?.  !!  以及装箱值的拆箱
// ============================================================

// 生成对 ptr 非空性的 i1 检查（1 = 非空）。可空值类型已被装箱为 ptr，
// 因此引用类型与值类型的空检查统一为指针与 0 比较。
std::string IRGen::genNotNullCheck(const Value& v) {
    std::string r = emitICmp("ne", "ptr", v.ir, "null");
    return r;
}

// base!! —— 断言非空。失败调用 hao_panic_null（不返回）；
// 成功时把可空类型"去可空"后传回（引用类型直接放宽，装箱值类型拆箱）。
Value IRGen::applyNotNullAssert(const Value& base, antlr4::ParserRuleContext* ctx) {
    // smart cast 后已非空：!! 幂等（stdlib 可逐步去掉多余 !!）
    if (!base.type->nullable) {
        return base;
    }

    // 控制流：非空继续，为空则 panic。
    // 用 br 拆分基本块，panic 块 call 后 unreachable。
    std::string okL   = em_.nextLabel("nn.ok");
    std::string panicL = em_.nextLabel("nn.panic");
    std::string contL  = em_.nextLabel("nn.cont");
    std::string isNull = genNotNullCheck(base);
    std::string entryBlock = em_.currentBlock();
    emitCondBr(isNull, okL, panicL);

    em_.emitLabel(panicL);
    emitCallVoid("@hao_panic_null", "");
    emitUnreachable();

    em_.emitLabel(okL);
    emitBr(contL);

    em_.emitLabel(contL);
    (void)entryBlock;

    auto nonNull = std::make_shared<Type>(*base.type);
    nonNull->nullable = false;
    return unboxNullableKnown(base, nonNull);
}

// 把值类型值装箱为可空指针；引用类型仅标记可空。
// 调用方在跨 safepoint 前须 rootGcOperand（formatCallArg / ?.phi 后）。
Value IRGen::boxToNullable(const Value& v) {
    if (!v.type) return v;
    // 已是可空（含 Int?/Bool? 等已装箱 ptr）：勿二次 hao_box_*（会把 ptr 当 i32）
    if (v.type->nullable)
        return Value(v.ir, v.type->asNullable());
    auto t = std::make_shared<Type>(*v.type);
    t->nullable = true;
    if (v.type->kind == TypeKind::Long || v.type->kind == TypeKind::ULong ||
        v.type->kind == TypeKind::UIntPtr) {
        std::string reg = emitCall("ptr", "@hao_box_i64", "i64 " + v.ir);
        return Value(reg, t);
    }
    if (v.type->kind == TypeKind::Int || v.type->kind == TypeKind::UInt ||
        v.type->kind == TypeKind::Char) {
        std::string reg = emitCall("ptr", "@hao_box_i32", "i32 " + v.ir);
        return Value(reg, t);
    }
    if (v.type->kind == TypeKind::Bool) {
        std::string wide = emitCast("zext", "i8", v.ir, "i32");
        std::string reg = emitCall("ptr", "@hao_box_i32", "i32 " + wide);
        return Value(reg, t);
    }
    if (v.type->kind == TypeKind::Short) {
        std::string wide = emitCast("sext", "i16", v.ir, "i32");
        std::string reg = emitCall("ptr", "@hao_box_i32", "i32 " + wide);
        return Value(reg, t);
    }
    if (v.type->kind == TypeKind::UShort) {
        std::string wide = emitCast("zext", "i16", v.ir, "i32");
        std::string reg = emitCall("ptr", "@hao_box_i32", "i32 " + wide);
        return Value(reg, t);
    }
    if (v.type->kind == TypeKind::SByte) {
        std::string wide = emitCast("sext", "i8", v.ir, "i32");
        std::string reg = emitCall("ptr", "@hao_box_i32", "i32 " + wide);
        return Value(reg, t);
    }
    if (v.type->kind == TypeKind::Byte) {
        std::string wide = emitCast("zext", "i8", v.ir, "i32");
        std::string reg = emitCall("ptr", "@hao_box_i32", "i32 " + wide);
        return Value(reg, t);
    }
    if (v.type->kind == TypeKind::Double) {
        std::string reg = emitCall("ptr", "@hao_box_f64", "double " + v.ir);
        return Value(reg, t);
    }
    if (v.type->kind == TypeKind::Float) {
        std::string reg = emitCall("ptr", "@hao_box_f32", "float " + v.ir);
        return Value(reg, t);
    }
    return Value(v.ir, t);
}

// base?.field —— 安全成员访问：base 为空时整个表达式为 null，否则取成员。
// 结果类型恒为成员类型的可空版本（值类型装箱为 ptr，引用类型同为 ptr），
// 因此空分支与非空分支都产生 ptr，phi 统一为 ptr。
Value IRGen::applySafeMemberAccess(const Value& base, const std::string& field,
                                   antlr4::ParserRuleContext* ctx) {
    Value baseR = base;
    rootGcOperand(baseR); /* base 跨 ensureInit / 装箱 */
    if (!baseR.type->nullable) {
        // 非空接收者用 ?. 等价于普通 .，但仍把结果标记为可空并装箱，
        // 使链式调用的类型一致（a?.b?.c 在 a 非空时同样成立）。
        Value m = applyMemberAccess(baseR, field, ctx);
        if (!m.valid()) return m;
        Value out = boxToNullable(m);
        rootGcOperand(out);
        return out;
    }

    std::string nonNullL = em_.nextLabel("smem.nn");
    std::string nullL    = em_.nextLabel("smem.null");
    std::string contL    = em_.nextLabel("smem.cont");
    std::string isNull = genNotNullCheck(baseR);
    emitCondBr(isNull, nonNullL, nullL);

    // 非空分支：取成员并装箱
    em_.emitLabel(nonNullL);
    auto nonNullType = std::make_shared<Type>(*baseR.type);
    nonNullType->nullable = false;
    Value nonNullBase(baseR.ir, nonNullType);
    Value member = applyMemberAccess(nonNullBase, field, ctx);
    if (!member.valid()) return Value();
    Value boxed = boxToNullable(member);
    std::string nnBlock = em_.currentBlock();
    if (!blockTerminated_) emitBr(contL);

    // 空分支：直接给 null
    em_.emitLabel(nullL);
    auto resultType = std::make_shared<Type>(*member.type);
    resultType->nullable = true;
    std::string nullBlock = em_.currentBlock();
    emitBr(contL);

    em_.emitLabel(contL);
    std::string phi = emitPhi("ptr",
        "[ " + boxed.ir + ", %" + nnBlock +
        " ], [ null, %" + nullBlock + " ]");
    Value out(phi, resultType);
    rootGcOperand(out); /* phi 结果跨后续调用 */
    return out;
}

// base?.method(args) —— 安全调用：base 为空时跳过调用，结果为 null/0。
Value IRGen::applySafeMethodCall(const Value& base, const std::string& method,
                                 HaoLangParser::CallOpContext* call,
                                 antlr4::ParserRuleContext* ctx) {
    Value baseR = base;
    rootGcOperand(baseR);
    if (!baseR.type->nullable) {
        return applyMethodCall(baseR, method, call, ctx);
    }

    std::string nonNullL = em_.nextLabel("scall.nn");
    std::string nullL    = em_.nextLabel("scall.null");
    std::string contL    = em_.nextLabel("scall.cont");
    std::string isNull = genNotNullCheck(baseR);
    emitCondBr(isNull, nonNullL, nullL);

    em_.emitLabel(nonNullL);
    auto nonNullType = std::make_shared<Type>(*baseR.type);
    nonNullType->nullable = false;
    Value nonNullBase(baseR.ir, nonNullType);
    Value result = applyMethodCall(nonNullBase, method, call, ctx);
    if (!result.valid()) return Value();

    // Unit 返回类型：空分支也是无值，合并后仍为 Unit（不可空）
    if (result.type->isUnit()) {
        if (!blockTerminated_) emitBr(contL);
        em_.emitLabel(nullL);
        emitBr(contL);
        em_.emitLabel(contL);
        return Value("", Type::makeUnit());
    }

    // 值类型结果装箱为可空 ptr，使两分支都产生 ptr（须在记录 nnBlock 之后，
    // 因 box/spill 会继续往当前块写指令）
    Value boxed = boxToNullable(result);
    auto resultType = std::make_shared<Type>(*result.type);
    resultType->nullable = true;
    std::string nnBlock = em_.currentBlock();
    if (!blockTerminated_) emitBr(contL);

    em_.emitLabel(nullL);
    std::string nullBlock = em_.currentBlock();
    emitBr(contL);

    em_.emitLabel(contL);
    std::string phi = emitPhi("ptr",
        "[ " + boxed.ir + ", %" + nnBlock +
        " ], [ null, %" + nullBlock + " ]");
    Value out(phi, resultType);
    rootGcOperand(out);
    return out;
}

// ---------- 后缀链驱动 ----------
Value IRGen::genPostfix(HaoLangParser::PostfixExprContext* e) {
    auto ops = e->postfixOp();
    if (ops.empty()) return genPrimary(e->primary());

    // ===== 特判 1b：跨包限定 pkg.member… =====
    //  支持 pkg.fn(args)，以及 pkg.Type.field / pkg.Type.m()（允许 package 与 class 同名）。
    size_t startOp = 0;
    Value cur;
    bool curReady = false;

    if (auto* idp =
            dynamic_cast<HaoLangParser::IdentPrimaryContext*>(e->primary())) {
        if (!ops.empty()) {
            if (auto* mem0 =
                    dynamic_cast<HaoLangParser::MemberAccessContext*>(ops[0])) {
                std::string alias = idp->IDENT()->getText();
                bool isImportedPkg = false;
                for (const auto& im : currentImports_)
                    if (im.alias == alias && !im.wildcard) {
                        isImportedPkg = true;
                        break;
                    }
                if (isImportedPkg) {
                    std::string member = mem0->IDENT()->getText();
                    auto sym = resolveQualifiedName(alias, member, e);
                    if (!sym) return Value();

                    // pkg.Type.… → 静态接收者，后续后缀走通用归约
                    if (sym->kind == SymbolKind::Class && sym->classInfo) {
                        cur = Value("", Type::makeClass(sym->classInfo->name));
                        startOp = 1;
                        curReady = true;
                    } else if (sym->kind == SymbolKind::Function && ops.size() == 2) {
                        auto* call = dynamic_cast<HaoLangParser::CallOpContext*>(ops[1]);
                        if (!call) {
                            error(e, "跨包函数 '" + alias + "." + member +
                                     "' 只能以调用形式使用");
                            return Value();
                        }
                        auto* al = call->argList();
                        size_t nargs = al ? al->arg().size() : 0;

                        auto& oc = overloads_[sym->name];
                        if (oc.size() > 1) {
                            std::vector<Value> args;
                            args.reserve(nargs);
                            for (size_t k = 0; k < nargs; ++k) {
                                Value av = genExpr(al->arg(k)->expr());
                                if (!av.valid()) return Value();
                                rootGcOperand(av);
                                args.push_back(av);
                            }
                            auto chosen = selectOverload(oc, args, e, alias + "." + member);
                            if (!chosen) return Value();
                            std::string argStr;
                            for (size_t k = 0; k < args.size(); ++k) {
                                args[k] = coerce(args[k], chosen->paramTypes[k], 0, 0);
                                if (k) argStr += ", ";
                                argStr += formatCallArg(chosen->paramTypes[k],
                                                        al->arg(k)->expr(), args[k],
                                                        !chosen->isExtern);
                            }
                            if (chosen->returnType->isUnit()) {
                                emitCallVoid(chosen->irName, argStr);
                                return Value("", Type::makeUnit());
                            }
                            std::string reg = emitCall(chosen->returnType->llvmType(), chosen->irName, argStr);
                            return Value(reg, chosen->returnType);
                        }

                        if (genericFns_.count(sym->name)) {
                            std::vector<Value> args;
                            args.reserve(nargs);
                            for (size_t k = 0; k < nargs; ++k) {
                                Value av = genExpr(al->arg(k)->expr());
                                if (!av.valid()) return Value();
                                rootGcOperand(av);
                                args.push_back(av);
                            }
                            auto inst = instantiateFunction(sym->name, args, e);
                            if (!inst || diags_.hasErrors()) return Value();
                            std::string argStr;
                            for (size_t k = 0; k < args.size(); ++k) {
                                if (!isAssignable(args[k].type, inst->paramTypes[k])) {
                                    error(e, alias + "." + member + " 第 " +
                                             std::to_string(k+1) + " 个参数类型不匹配：期望 " +
                                             inst->paramTypes[k]->toString() + "，实际 " +
                                             args[k].type->toString());
                                    return Value();
                                }
                                args[k] = coerce(args[k], inst->paramTypes[k], 0, 0);
                                if (k) argStr += ", ";
                                argStr += formatCallArg(inst->paramTypes[k],
                                                        al->arg(k)->expr(), args[k]);
                            }
                            if (inst->returnType->isUnit()) {
                                emitCallVoid(inst->irName, argStr);
                                return Value("", Type::makeUnit());
                            }
                            std::string reg = emitCall(inst->returnType->llvmType(), inst->irName, argStr);
                            return Value(reg, inst->returnType);
                        }

                        std::vector<Value> args;
                        args.reserve(nargs);
                        for (size_t k = 0; k < nargs; ++k) {
                            if (k < sym->paramTypes.size())
                                expectedTypes_.push_back(sym->paramTypes[k]);
                            Value av = genExpr(al->arg(k)->expr());
                            if (k < sym->paramTypes.size()) expectedTypes_.pop_back();
                            if (!av.valid()) return Value();
                            rootGcOperand(av);
                            args.push_back(av);
                        }
                        if (args.size() != sym->paramTypes.size()) {
                            error(e, alias + "." + member + " 需要 " +
                                     std::to_string(sym->paramTypes.size()) +
                                     " 个参数，实际 " + std::to_string(args.size()));
                            return Value();
                        }
                        std::string argStr;
                        for (size_t k = 0; k < args.size(); ++k) {
                            if (!isAssignable(args[k].type, sym->paramTypes[k])) {
                                error(e, alias + "." + member + " 第 " +
                                         std::to_string(k+1) + " 个参数类型不匹配：期望 " +
                                         sym->paramTypes[k]->toString() + "，实际 " +
                                         args[k].type->toString());
                                return Value();
                            }
                            args[k] = coerce(args[k], sym->paramTypes[k], 0, 0);
                            if (k) argStr += ", ";
                            argStr += formatCallArg(sym->paramTypes[k],
                                                    al->arg(k)->expr(), args[k],
                                                    !sym->isExtern);
                        }
                        pinRuntimeCallSite(e);
                        if (sym->returnType->isUnit()) {
                            emitCallVoid(sym->irName, argStr);
                            return Value("", Type::makeUnit());
                        }
                        std::string reg = emitCall(sym->returnType->llvmType(), sym->irName, argStr);
                        return Value(reg, sym->returnType);
                    } else if (sym->kind == SymbolKind::Function) {
                        error(e, "跨包函数 '" + alias + "." + member +
                                 "' 只能以调用形式使用");
                        return Value();
                    } else {
                        error(e, "包 '" + alias + "' 的成员 '" + member +
                                 "' 不能这样使用");
                        return Value();
                    }
                }
            }
        }
    }

    // ===== 特判 2：super —— 构造链 super(args) / 方法 super.method(...) =====
    //  构造链：调用父类构造函数（new 已写好父类字段默认值，super 按实参初始化）。
    //  方法：静态绑定到父类实现，否则会分派回子类覆写版本造成无限递归。
    if (!curReady &&
        dynamic_cast<HaoLangParser::SuperPrimaryContext*>(e->primary())) {

        // ---- super(args)：调用父类构造函数（仅构造器内）----
        if (!ops.empty() && dynamic_cast<HaoLangParser::CallOpContext*>(ops[0])) {
            if (!currentClass_ || thisAddr_.empty() || !inConstructor_) {
                error(e, "super(args) 只能在构造函数中调用父类构造函数");
                return Value();
            }
            if (!currentClass_->base) {
                error(e, "类 '" + currentClass_->name + "' 没有基类，不能调用 super 构造");
                return Value();
            }
            auto* bc = currentClass_->base;
            if (bc->ctorIRName.empty()) {
                error(e, "基类 '" + bc->name + "' 没有构造函数");
                return Value();
            }
            auto* sCall = static_cast<HaoLangParser::CallOpContext*>(ops[0]);
            std::vector<Value> args;
            std::vector<antlr4::tree::ParseTree*> argExprs;
            if (auto* al = sCall->argList())
                for (auto* a : al->arg()) {
                    Value av = genExpr(a->expr());
                    if (!av.valid()) return Value();
                    rootGcOperand(av);
                    args.push_back(av);
                    argExprs.push_back(a->expr());
                }
            if (args.size() != bc->ctorParamTypes.size()) {
                error(e, "基类构造 '" + bc->name + "' 需要 " +
                           std::to_string(bc->ctorParamTypes.size()) +
                           " 个参数，实际提供 " + std::to_string(args.size()) + " 个");
                return Value();
            }
            std::string thisReg = emitLoad("ptr", thisAddr_);
            std::string argStr = "ptr " + thisReg;
            for (size_t i = 0; i < args.size(); ++i) {
                if (!isAssignable(args[i].type, bc->ctorParamTypes[i])) {
                    error(e, "基类构造第 " + std::to_string(i + 1) +
                               " 个参数类型不匹配：期望 " +
                               bc->ctorParamTypes[i]->toString() + "，实际 " +
                               args[i].type->toString());
                    return Value();
                }
                args[i] = coerce(args[i], bc->ctorParamTypes[i], 0, 0);
                argStr += ", " + formatCallArg(bc->ctorParamTypes[i], argExprs[i],
                                              args[i]);
            }
            emitCallVoid(bc->ctorIRName, argStr);
            cur = Value("", Type::makeUnit());
            startOp = 1;
        } else {
        // ---- super.method(...)：调用父类实现 ----
        if (ops.size() < 2) {
            error(e, "super 必须用于调用父类方法，如 super.method()");
            return Value();
        }
        auto* mem  = dynamic_cast<HaoLangParser::MemberAccessContext*>(ops[0]);
        auto* call = dynamic_cast<HaoLangParser::CallOpContext*>(ops[1]);
        if (!mem || !call) {
            error(e, "super 只支持方法调用形式 super.method(...)");
            return Value();
        }
        if (!currentClass_ || thisAddr_.empty()) {
            error(e, "super 只能在类的方法或构造函数中使用");
            return Value();
        }
        if (!currentClass_->base) {
            error(e, "类 '" + currentClass_->name + "' 没有基类，不能使用 super");
            return Value();
        }

        std::string method = mem->IDENT()->getText();
        const MethodInfo* bm = currentClass_->base->findMethod(method);
        if (!bm) {
            error(e, "基类 '" + currentClass_->base->name +
                     "' 没有方法 '" + method + "'");
            return Value();
        }
        if (!canAccessMember(bm->visibility, bm->ownerClass)) {
            error(e, std::string("不能通过 super 访问 ") + visName(bm->visibility) +
                       " 方法 '" + bm->ownerClass + "." + method + "'");
            return Value();
        }
        if (bm->isAbstract) {
            error(e, "不能通过 super 调用抽象方法 '" + method + "'");
            return Value();
        }

        std::vector<Value> args;
        std::vector<antlr4::tree::ParseTree*> argExprs;
        if (auto* al = call->argList())
            for (auto* a : al->arg()) {
                Value av = genExpr(a->expr());
                if (!av.valid()) return Value();
                rootGcOperand(av);
                args.push_back(av);
                argExprs.push_back(a->expr());
            }
        if (args.size() != bm->paramTypes.size()) {
            error(e, "基类方法 '" + method + "' 需要 " +
                     std::to_string(bm->paramTypes.size()) + " 个参数，实际提供 " +
                     std::to_string(args.size()) + " 个");
            return Value();
        }

        std::string thisReg = emitLoad("ptr", thisAddr_);
        std::string argStr = "ptr " + thisReg;
        for (size_t i = 0; i < args.size(); ++i) {
            if (!isAssignable(args[i].type, bm->paramTypes[i])) {
                error(e, "基类方法 '" + method + "' 第 " + std::to_string(i + 1) +
                         " 个参数类型不匹配：期望 " + bm->paramTypes[i]->toString() +
                         "，实际 " + args[i].type->toString());
                return Value();
            }
            args[i] = coerce(args[i], bm->paramTypes[i], 0, 0);
            argStr += ", " + formatCallArg(bm->paramTypes[i], argExprs[i], args[i]);
        }

        // 直接调用父类实现的符号名，绕过虚表
        if (bm->returnType->isUnit()) {
            emitCallVoid(bm->irName, argStr);
            cur = Value("", Type::makeUnit());
        } else {
            std::string reg = emitCall(bm->returnType->llvmType(), bm->irName, argStr);
            cur = Value(reg, bm->returnType);
        }
        startOp = 2;   // 前两个后缀已消耗
        }
    } else if (!curReady && startOp < ops.size() &&
               dynamic_cast<HaoLangParser::CallOpContext*>(ops[startOp]) &&
               dynamic_cast<HaoLangParser::IdentPrimaryContext*>(e->primary())) {
        // 具名函数直接调用 f(...)：不在此求值 primary（否则函数名会被
        // 包装成闭包对象，产生无用分配），交给循环里的 CallOp 分支直连。
        auto* idp = static_cast<HaoLangParser::IdentPrimaryContext*>(e->primary());
        std::string fname = idp->IDENT()->getText();
        auto s = resolveTopLevelName(fname, e);
        if ((s && s->kind == SymbolKind::Function) ||
            (s && genericFns_.count(s->name)))
            cur = Value("", Type::makeUnit());   // 占位，CallOp 分支会处理
        else {
            cur = genPrimary(e->primary());
            if (!cur.valid()) return Value();
        }
    } else if (!curReady) {
        cur = genPrimary(e->primary());
        if (!cur.valid()) return Value();
    }

    // ===== 通用归约：逐个后缀作用于前一步结果 =====
    for (size_t i = startOp; i < ops.size(); ++i) {
        auto* op = ops[i];

        // 成员访问后紧跟调用括号 => 方法调用，两个后缀一起消耗
        if (auto* mem = dynamic_cast<HaoLangParser::MemberAccessContext*>(op)) {
            if (i + 1 < ops.size()) {
                if (auto* call =
                        dynamic_cast<HaoLangParser::CallOpContext*>(ops[i + 1])) {
                    cur = applyMethodCall(cur, mem->IDENT()->getText(), call, e);
                    if (!cur.valid()) return Value();
                    ++i;                     // 额外消耗调用括号
                    continue;
                }
            }
            cur = applyMemberAccess(cur, mem->IDENT()->getText(), e);
            if (!cur.valid()) return Value();
            continue;
        }

        if (auto* io = dynamic_cast<HaoLangParser::IndexOpContext*>(op)) {
            cur = applyIndex(cur, io, e);
            if (!cur.valid()) return Value();
            continue;
        }

        // 函数调用：f(...) 或 funcValue(...)
        if (auto* call = dynamic_cast<HaoLangParser::CallOpContext*>(op)) {
            auto* al = call->argList();
            size_t nargs = al ? al->arg().size() : 0;

            // 先确定目标是否为泛型函数；若是，交给 callGenericFunction
            // 自己两遍求值实参（避免重复求值、并支持从 lambda 推断 R）。
            bool isGeneric = false;
            std::string genericTplName;
            std::vector<TypePtr> paramHints;
            SymbolPtr directSym;
            if (i == startOp) {
                if (auto* idp =
                        dynamic_cast<HaoLangParser::IdentPrimaryContext*>(e->primary())) {
                    std::string fname = idp->IDENT()->getText();
                    auto s = resolveTopLevelName(fname, e);
                    if (s) {
                        if (genericFns_.count(s->name)) {
                            isGeneric = true;
                            genericTplName = s->name;
                        } else if (s->kind == SymbolKind::Function) {
                            directSym = s;
                            paramHints = s->paramTypes;
                        }
                    }
                    if (directSym && directSym->kind == SymbolKind::Variable &&
                        directSym->type && directSym->type->kind == TypeKind::Func)
                        paramHints = directSym->type->params;
                }
            } else if (cur.type && cur.type->kind == TypeKind::Func) {
                paramHints = cur.type->params;
            }

            // ---- A2. 具名泛型函数：T/R 由实参推断（含 (T)->R lambda）后实例化并调用 ----
            if (isGeneric) {
                Value r = callGenericFunction(genericTplName, al, e);
                if (diags_.hasErrors()) return Value();
                if (!r.valid()) {
                    error(e, "无法推断泛型函数的类型参数");
                    return Value();
                }
                cur = r;
                continue;
            }

            // 求值实参（每个实参压入对应形参类型作为期望类型）
            /* Func 值跨实参求值须先挂根 */
            if (cur.valid() && cur.type && cur.type->kind == TypeKind::Func)
                rootGcOperand(cur);
            std::vector<Value> args;
            std::vector<antlr4::tree::ParseTree*> argExprs;
            args.reserve(nargs);
            for (size_t k = 0; k < nargs; ++k) {
                if (k < paramHints.size()) expectedTypes_.push_back(paramHints[k]);
                auto* aex = al->arg(k)->expr();
                Value av = genExpr(aex);
                if (k < paramHints.size()) expectedTypes_.pop_back();
                if (!av.valid()) return Value();
                rootGcOperand(av);
                args.push_back(av);
                argExprs.push_back(aex);
            }

            // ---- A. 具名非泛型函数直接调用（零开销，绕过闭包）----
            if (i == startOp) {
                if (auto* idp =
                        dynamic_cast<HaoLangParser::IdentPrimaryContext*>(e->primary())) {
                    std::string fname = idp->IDENT()->getText();
                    auto sym = directSym;
                    if (sym && sym->kind == SymbolKind::Function) {
                        // ---- 函数重载：同名有多个候选时按实参选最佳（v0.9.0）----
                        auto& oc = overloads_[sym->name];
                        if (oc.size() > 1) {
                            sym = selectOverload(oc, args, e, fname);
                            if (!sym) return Value();
                        }
                        if (args.size() != sym->paramTypes.size()) {
                            error(e, "函数 '" + fname + "' 需要 " +
                                     std::to_string(sym->paramTypes.size()) +
                                     " 个参数，实际提供 " +
                                     std::to_string(args.size()) + " 个");
                            return Value();
                        }
                        std::string argStr;
                        for (size_t k = 0; k < args.size(); ++k) {
                            if (!isAssignable(args[k].type, sym->paramTypes[k])) {
                                error(e, "函数 '" + fname + "' 第 " +
                                         std::to_string(k + 1) +
                                         " 个参数类型不匹配：期望 " +
                                         sym->paramTypes[k]->toString() + "，实际 " +
                                         args[k].type->toString());
                                return Value();
                            }
                            if (k) argStr += ", ";
                            argStr += formatCallArg(sym->paramTypes[k], argExprs[k],
                                                    args[k], !sym->isExtern);
                        }
                        std::string callName = sym->irName;
                        if (callName.empty()) callName = "@" + fname;
                        if (callName[0] != '@') callName = "@" + callName;

                        pinRuntimeCallSite(e);
                        if (sym->returnType->isUnit()) {
                            emitCallVoid(callName, argStr);
                            cur = Value("", Type::makeUnit());
                        } else {
                            std::string reg = emitCall(sym->returnType->llvmType(), callName, argStr);
                            cur = Value(reg, sym->returnType);
                        }
                        continue;
                    }
                }
            }

            // ---- B. 间接调用函数值（lambda / 函数变量 / 返回函数的表达式）----
            //  函数值是 env 指针，槽 0 存实现函数指针，签名 ret(env, args...)
            if (!cur.type || cur.type->kind != TypeKind::Func) {
                error(e, "该值不是函数，不能调用");
                return Value();
            }
            TypePtr fnRet = cur.type->elem ? cur.type->elem : Type::makeUnit();
            const auto& fnParams = cur.type->params;

            if (args.size() != fnParams.size()) {
                error(e, "函数值需要 " + std::to_string(fnParams.size()) +
                         " 个参数，实际提供 " + std::to_string(args.size()) + " 个");
                return Value();
            }

            // 从 env 槽 0 取出函数指针
            std::string fpp = fieldPtr(cur.ir, 0);
            std::string fp = emitLoad("ptr", fpp);

            std::string argStr = "ptr " + cur.ir;   // 首参为 env
            for (size_t k = 0; k < args.size(); ++k) {
                if (!isAssignable(args[k].type, fnParams[k])) {
                    error(e, "函数值第 " + std::to_string(k + 1) +
                             " 个参数类型不匹配：期望 " + fnParams[k]->toString() +
                             "，实际 " + args[k].type->toString());
                    return Value();
                }
                argStr += ", " + formatCallArg(fnParams[k], argExprs[k], args[k]);
            }

            if (fnRet->isUnit()) {
                emitCallVoid(fp, argStr);
                cur = Value("", Type::makeUnit());
            } else {
                std::string reg = emitCall(fnRet->llvmType(), fp, argStr);
                cur = Value(reg, fnRet);
            }
            continue;
        }

        if (auto* safe = dynamic_cast<HaoLangParser::SafeMemberAccessContext*>(op)) {
            // ?.method(...)：安全调用，两个后缀一起消耗
            if (i + 1 < ops.size()) {
                if (auto* call =
                        dynamic_cast<HaoLangParser::CallOpContext*>(ops[i + 1])) {
                    cur = applySafeMethodCall(cur, safe->IDENT()->getText(), call, e);
                    if (!cur.valid()) return Value();
                    ++i;
                    continue;
                }
            }
            cur = applySafeMemberAccess(cur, safe->IDENT()->getText(), e);
            if (!cur.valid()) return Value();
            continue;
        }
        if (dynamic_cast<HaoLangParser::NotNullAssertContext*>(op)) {
            cur = applyNotNullAssert(cur, e);
            if (!cur.valid()) return Value();
            continue;
        }
        if (dynamic_cast<HaoLangParser::PostIncrContext*>(op) ||
            dynamic_cast<HaoLangParser::PostDecrContext*>(op)) {
            error(e, "当前版本尚不支持后缀 ++ / --");
            return Value();
        }

        error(e, "无法识别的后缀操作");
        return Value();
    }

    return cur;
}


// ---------- 基本表达式 ----------
Value IRGen::genPrimary(HaoLangParser::PrimaryContext* e) {
    if (auto* lp = dynamic_cast<HaoLangParser::LitPrimaryContext*>(e))
        return genLiteral(lp->literal());

    if (auto* ip = dynamic_cast<HaoLangParser::IdentPrimaryContext*>(e)) {
        std::string name = ip->IDENT()->getText();
        auto sym = resolveTopLevelName(name, e);
        if (!sym) {
            // 保留与旧版一致的错误信息（未导入或不存在）
            error(e, "未定义的标识符 '" + name + "'");
            return Value();
        }
        if (sym->kind == SymbolKind::Variable)
            return loadVar(sym);

        // 顶层函数名作为值：生成一个闭包 env（无捕获，槽 0 指向
        // 统一约定的包装函数），使其能作为函数值传递、返回、存入集合。
        if (sym->kind == SymbolKind::Function) {
            if (genericFns_.count(sym->name)) {
                error(e, "泛型函数 '" + name +
                         "' 不能直接作为函数值，请先以具体类型调用实例化");
                return Value();
            }
            std::string wname = ensureFuncWrapper(sym);
            std::string envRaw = emitObjectNew(1, 0); // 仅 fnptr
            std::string envSlot = emitSpillGcRoot("fnval.env", envRaw);
            std::string env = emitLoad("ptr", envSlot);
            std::string sp = fieldPtr(env, 0);
            emitStore("ptr", wname, sp);
            return Value(env, Type::makeFunc(sym->paramTypes, sym->returnType));
        }

        // 类名作为"静态接收者"：访问静态成员 ClassName.X / ClassName.f() 时
        // 使用（无实际值，仅携带类类型，供 applyMemberAccess/applyMethodCall
        // 解析静态成员并忽略实例指针）。
        if (sym->kind == SymbolKind::Class && sym->classInfo)
            return Value("", Type::makeClass(sym->classInfo->name));

        // 类名当前不可作为值
        return Value("", Type::makeFunc(sym->paramTypes, sym->returnType));
    }

    if (auto* pp = dynamic_cast<HaoLangParser::ParenPrimaryContext*>(e))
        return genExpr(pp->expr());

    if (auto* ap = dynamic_cast<HaoLangParser::ArrayPrimaryContext*>(e))
        return genArrayLiteral(ap->arrayLiteral());

    if (auto* wp = dynamic_cast<HaoLangParser::WhenPrimaryContext*>(e))
        return genWhenExpr(wp->whenStmt());

    // ---------- this ----------
    if (dynamic_cast<HaoLangParser::ThisPrimaryContext*>(e)) {
        if (!currentClass_ || thisAddr_.empty()) {
            error(e, "this 只能在类的方法或构造函数中使用");
            return Value();
        }
        std::string reg = emitLoad("ptr", thisAddr_);
        return Value(reg, Type::makeClass(currentClass_->name));
    }

    if (dynamic_cast<HaoLangParser::SuperPrimaryContext*>(e)) {
        error(e, "当前版本尚不支持 super（需要继承支持）");
        return Value();
    }

    // ---------- new 表达式 ----------
    if (auto* np = dynamic_cast<HaoLangParser::NewPrimaryContext*>(e)) {
        TypePtr t = resolveType(np->type());
        // v0.32：new [T](n) / new [T](n, fill) —— 定长数组（零填充或统一初值）
        if (t->kind == TypeKind::Array) {
            std::vector<Value> args;
            if (auto* al = np->argList()) {
                for (auto* a : al->arg()) {
                    Value av = genExpr(a->expr());
                    if (!av.valid()) return Value();
                    args.push_back(av);
                }
            }
            if (args.empty() || args.size() > 2) {
                error(e, "定长数组语法为 new [T](长度) 或 new [T](长度, 填充值)");
                return Value();
            }
            if (!t->elem) {
                error(e, "定长数组缺少元素类型");
                return Value();
            }
            const Value* fill = args.size() == 2 ? &args[1] : nullptr;
            return genSizedArray(t->elem, args[0], fill, e);
        }
        if (t->kind != TypeKind::Class) {
            error(e, "new 只能用于类或数组类型，实际为 " + t->toString());
            return Value();
        }

        // 泛型类必须提供类型参数；resolveType 已在解析 Box<Int> 时
        // 触发实例化，此处按单态化后的实例名查找
        ClassInfoPtr ci;
        if (!t->typeArgs.empty()) {
            ci = lookupClass(t->monoName());
            if (!ci) {
                error(e, "无法实例化 '" + t->toString() + "'");
                return Value();
            }
        } else {
            ci = lookupClass(t->className);
            if (!ci) {
                error(e, "未定义的类 '" + t->className + "'");
                return Value();
            }
            if (ci->isGenericTemplate()) {
                error(e, "泛型类 '" + ci->name + "' 必须提供类型参数，"
                         "例如 new " + ci->name + "<Int>(...)");
                return Value();
            }
        }

        if (ci->isAbstract) {
            error(e, "不能实例化抽象类 '" + ci->name + "'");
            return Value();
        }
        if (ci->isEnum) {
            error(e, "不能直接实例化枚举类 '" + ci->name +
                       "'，请访问其常量（如 " + ci->name + ".RED）");
            return Value();
        }

        // 首次 new 前触发类的静态初始化（若有静态构造器/非常量静态字段）
        emitStaticEnsureInit(ci);

        // 实例的静态类型用带实参的形式，便于错误信息显示 Box<Int>
        TypePtr objType = t->typeArgs.empty()
            ? Type::makeClass(ci->name)
            : t;

        // 求值实参；GC 实参立刻 spill，避免后续实参/分配 safepoint 假死
        std::vector<Value> args;
        std::vector<antlr4::tree::ParseTree*> argExprs;
        if (auto* al = np->argList()) {
            for (auto* a : al->arg()) {
                Value av = genExpr(a->expr());
                if (!av.valid()) return Value();
                if (isGcPointerType(av.type)) {
                    std::string slot = emitSpillGcRoot("new.arg", av.ir);
                    std::string p = emitLoad("ptr", slot);
                    av.ir = p;
                }
                args.push_back(av);
                argExprs.push_back(a->expr());
            }
        }

        // 分配对象内存（有虚表时槽位 0 留给虚表指针；位图不含 vtable）
        std::string objRaw = emitObjectNew(ci->slotCount(), objectPtrBitmap(ci.get()));
        // 字段默认 / ctor 求值可能 safepoint：obj 须进 shadow
        std::string objSlot = emitSpillGcRoot("new.obj", objRaw);
        std::string obj = emitLoad("ptr", objSlot);

        // 写入虚表指针，使该对象可通过接口动态分派
        if (ci->hasVTable) {
            std::string vtp = emitGep("ptr", obj, "i64", "0");
            emitStore("ptr", ci->vtableIRName, vtp);
        }

        // 字段默认值：在构造函数之前写入，语义同 Java / C#。
        // ci->fields 已扁平化（含继承来的字段），因此这里一次性
        // 覆盖整条继承链的默认值，无需显式的构造函数链。
        // Func 字段默认 lambda：须先 analyzeLambdas（默认值不在 new 所在函数体 AST 内）。
        for (const auto& f : ci->fields) {
            if (!f.defaultExpr) continue;
            obj = emitLoad("ptr", objSlot);
            auto* dexpr = static_cast<HaoLangParser::ExprContext*>(f.defaultExpr);
            expectedTypes_.push_back(f.type);
            ExpectedTypeGuard eg{this};
            analyzeLambdas(dexpr);
            Value dv = genExpr(dexpr);
            if (!dv.valid()) return Value();
            if (!isAssignable(dv.type, f.type)) {
                error(dexpr, "字段 '" + f.name + "' 的初始值类型 " +
                             dv.type->toString() + " 与声明类型 " +
                             f.type->toString() + " 不匹配");
                return Value();
            }
            dv = coerce(dv, f.type, 0, 0);
            std::string fp = fieldPtr(obj, f.slot);
            emitHeapStore(fp, dv.ir, f.type, obj);
            /* D17：new 字段默认写回薄 dbg.value（完整类型仍开） */
            emitDbgValueIf(f.type->llvmType(), dv.ir, f.name, 0, 0);
        }

        // 基类构造函数：子类未声明构造函数时，若基类有构造函数则报错，
        // 因为没有途径传递参数（Java 会要求显式 super(...) 调用）
        if (ci->ctorIRName.empty() && ci->base) {
            for (const ClassInfo* b = ci->base; b; b = b->base) {
                if (!b->ctorIRName.empty() && !b->ctorParamTypes.empty()) {
                    error(e, "类 '" + ci->name + "' 继承自带参构造函数的 '" +
                             b->name + "'，必须自行声明构造函数");
                    return Value();
                }
            }
        }

        obj = emitLoad("ptr", objSlot);

        // 调用构造函数
        if (!ci->ctorIRName.empty()) {
            if (args.size() != ci->ctorParamTypes.size()) {
                error(e, "类 '" + ci->name + "' 的构造函数需要 " +
                         std::to_string(ci->ctorParamTypes.size()) +
                         " 个参数，实际提供 " + std::to_string(args.size()) + " 个");
                return Value();
            }
            std::string argStr = "ptr " + obj;
            for (size_t i = 0; i < args.size(); ++i) {
                if (!isAssignable(args[i].type, ci->ctorParamTypes[i])) {
                    error(e, "构造函数第 " + std::to_string(i + 1) +
                             " 个参数类型不匹配：期望 " +
                             ci->ctorParamTypes[i]->toString() +
                             "，实际 " + args[i].type->toString());
                    return Value();
                }
                args[i] = coerce(args[i], ci->ctorParamTypes[i], 0, 0);
                argStr += ", " + formatCallArg(ci->ctorParamTypes[i], argExprs[i],
                                              args[i]);
            }
            emitCallVoid(ci->ctorIRName, argStr);
            obj = emitLoad("ptr", objSlot);
        } else if (!args.empty()) {
            error(e, "类 '" + ci->name + "' 没有构造函数，不能传递参数");
            return Value();
        }

        /* 勿在此处清 objSlot：返回 SSA 仍可能跨 safepoint；
           循环内由 spill 池清 null，函数级由 root_unwind / 调用方根槽接管 */
        return Value(obj, objType);
    }

    if (auto* lam = dynamic_cast<HaoLangParser::LambdaPrimaryContext*>(e))
        return genLambda(lam->lambda());

    error(e, "无法识别的表达式");
    return Value();
}
} // namespace hao
