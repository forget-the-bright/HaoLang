// ============================================================
//  HaoLang IR 生成 —— 字面量与复合字面量
// ------------------------------------------------------------
//  从 IRGenExpr.cpp 拆分而来，逻辑保持不变：
//    - 数组字面量、带标注的空数组、空数组判定
//    - when 表达式（结果值）
//    - 字面量（整数/浮点/字符串/字符/bool/null）
//    - 模板字符串 $"...{expr}..."
//  v0.32：数组展开 [...a]；定长 new [T](n[, fill]) 见 genSizedArray
// ============================================================

#include "irgen/IRGen.h"

#include "util/StringUtil.h"

#include <climits>
#include <cstdint>
#include <functional>

namespace hao {

// ---------- 数组字面量 [a, b, ...xs] ----------
Value IRGen::genArrayLiteral(HaoLangParser::ArrayLiteralContext* al) {
    struct Elem {
        bool spread = false;
        Value v;
    };
    std::vector<Elem> elems;

    if (auto* list = al->arrayElementList()) {
        for (auto* ae : list->arrayElement()) {
            Value v = genExpr(ae->expr());
            if (!v.valid()) return Value();
            bool spread = ae->ELLIPSIS() != nullptr;
            if (spread) {
                if (v.type->kind != TypeKind::Array) {
                    error(ae, "展开运算符 ... 只能用于数组，实际为 " +
                              v.type->toString());
                    return Value();
                }
                if (v.type->nullable) {
                    error(ae, "不能展开可空数组 " + v.type->toString() +
                              "，请先用 !! 或 ??");
                    return Value();
                }
                if (!v.type->elem) {
                    error(ae, "无法展开元素类型未知的数组");
                    return Value();
                }
            }
            /* 元素/展开源跨后续 genExpr、array_new、写入循环 */
            rootGcOperand(v);
            elems.push_back({spread, v});
        }
    }

    // 元素类型推断：期望类型 > 首个非展开值 > 首个展开数组元素类型 > Int
    TypePtr elemType;
    if (!expectedTypes_.empty() && expectedTypes_.back() &&
        expectedTypes_.back()->kind == TypeKind::Array &&
        expectedTypes_.back()->elem) {
        elemType = expectedTypes_.back()->elem;
    } else if (elems.empty()) {
        elemType = Type::makeInt();
    } else {
        for (const auto& e : elems) {
            if (!e.spread) { elemType = e.v.type; break; }
        }
        if (!elemType) {
            for (const auto& e : elems) {
                if (e.spread && e.v.type->elem) {
                    elemType = e.v.type->elem;
                    break;
                }
            }
        }
        if (!elemType) elemType = Type::makeInt();

        for (const auto& e : elems) {
            TypePtr et = e.spread ? e.v.type->elem : e.v.type;
            if (et && et->isNumeric() && elemType->isNumeric()) {
                if (Type::isMixedSignedUnsigned64(elemType->kind, et->kind)) {
                    error(al, "64 位有符号与无符号不能隐式混合，请显式转换");
                    return Value();
                }
                TypePtr p = Type::binaryNumericPromote(elemType->kind, et->kind);
                if (p) elemType = p;
            }
        }

        // 收集非展开值做共同接口推断
        std::vector<Value> plain;
        for (const auto& e : elems)
            if (!e.spread) plain.push_back(e.v);
        bool allMatch = true;
        for (const auto& v : plain)
            if (!isAssignable(v.type, elemType)) { allMatch = false; break; }
        if (!allMatch && !plain.empty()) {
            std::string common = findCommonSupertype(plain);
            if (!common.empty()) {
                elemType = classes_.count(common)
                    ? Type::makeClass(common)
                    : Type::makeInterface(common);
            }
        }
    }

    // 校验
    for (size_t i = 0; i < elems.size(); ++i) {
        TypePtr et = elems[i].spread ? elems[i].v.type->elem : elems[i].v.type;
        if (!et || !isAssignable(et, elemType)) {
            error(al, "数组元素类型不一致：第 " + std::to_string(i + 1) +
                      " 个" + (elems[i].spread ? "展开源" : "元素") + "为 " +
                      (et ? et->toString() : "?") +
                      "，期望 " + elemType->toString());
            return Value();
        }
    }

    int64_t esz = elemType->arrayElemSize();
    std::string gepTy = elemType->arrayGepType();
    std::string lt = elemType->llvmType();

    // 运行时求和长度（展开源长度动态）；带溢出检测防回绕后欠分配
    auto addLenChecked = [&](const std::string& a, const std::string& b) -> std::string {
        std::string ov = emitCall("{i64, i1}", "@llvm.sadd.with.overflow.i64",
                                  "i64 " + a + ", i64 " + b);
        std::string sum = emitExtractValue("{i64, i1}", ov, 0);
        std::string flag = emitExtractValue("{i64, i1}", ov, 1);
        std::string okL = em_.nextLabel("alen.ok");
        std::string badL = em_.nextLabel("alen.ovf");
        emitCondBr(flag, badL, okL);
        em_.emitLabel(badL);
        blockTerminated_ = false;
        emitCallVoid("@hao_panic_overflow", "");
        emitUnreachable();
        em_.emitLabel(okL);
        blockTerminated_ = false;
        return sum;
    };
    std::string total = "0";
    for (size_t i = 0; i < elems.size(); ++i) {
        if (!elems[i].spread) {
            total = addLenChecked(total, "1");
        } else {
            std::string ln = emitCall("i64", "@hao_array_len", "ptr " + elems[i].v.ir);
            total = addLenChecked(total, ln);
        }
    }

    std::string isPtrLit = isGcPointerType(elemType) ? "1" : "0";
    std::string arrRaw = emitCall("ptr", "@hao_array_new", "i64 " + total + ", i64 " + std::to_string(esz) + ", i64 " + isPtrLit);
    // 目标数组跨合成循环 safepoint
    std::string arrSlot = emitSpillGcRoot("aspread.arr", arrRaw);
    std::string arr = emitLoad("ptr", arrSlot);

    // 写入：维护写指针偏移（i64）
    std::string off = "0";
    for (size_t i = 0; i < elems.size(); ++i) {
        if (!elems[i].spread) {
            arr = emitLoad("ptr", arrSlot);
            Value v = coerce(elems[i].v, elemType, 0, 0);
            std::string ptr = emitGep(gepTy, arr, "i64", off);
            emitHeapStore(ptr, v.ir, elemType, arr);
            std::string noff = emitBinOp("add", "i64", off, "1");
            off = noff;
        } else {
            // for j in 0..len: dest[off+j] = coerce(src[j])
            // 必须按源元素宽度 GEP/load，再 coerce 到目标（防 [Int]→[Long] 越界读）
            TypePtr srcElem = elems[i].v.type->elem;
            std::string srcGep = srcElem->arrayGepType();
            std::string srcLt = srcElem->llvmType();
            std::string srcSlot = emitSpillGcRoot("aspread.src", elems[i].v.ir);
            std::string src0 = emitLoad("ptr", srcSlot);
            std::string slen = emitCall("i64", "@hao_array_len", "ptr " + src0);
            std::string jAlloca = em_.nextTemp();
            emitAllocaAt(jAlloca, "i64");
            emitStore("i64", "0", jAlloca);
            std::string offAlloca = em_.nextTemp();
            emitAllocaAt(offAlloca, "i64");
            emitStore("i64", off, offAlloca);

            std::string loopLbl = em_.nextLabel("aspread.loop");
            std::string bodyLbl = em_.nextLabel("aspread.body");
            std::string endLbl = em_.nextLabel("aspread.end");
            emitBr(loopLbl);
            em_.emitLabel(loopLbl);
            blockTerminated_ = false;
            /* v0.53.5：合成展开循环须 safepoint；活数组指针须在 shadow */
            emitSafepoint();
            arr = emitLoad("ptr", arrSlot);
            std::string src = emitLoad("ptr", srcSlot);
            std::string jv = emitLoad("i64", jAlloca);
            std::string cmp = emitICmp("slt", "i64", jv, slen);
            emitCondBr(cmp, bodyLbl, endLbl);
            em_.emitLabel(bodyLbl);
            blockTerminated_ = false;
            std::string sp = emitGep(srcGep, src, "i64", jv);
            std::string sv = emitLoad(srcLt, sp);
            Value loaded(sv, srcElem);
            Value cv = coerce(loaded, elemType, 0, 0);
            std::string doff = emitLoad("i64", offAlloca);
            std::string dp = emitGep(gepTy, arr, "i64", doff);
            emitHeapStore(dp, cv.ir, elemType, arr);
            std::string doff2 = emitBinOp("add", "i64", doff, "1");
            emitStore("i64", doff2, offAlloca);
            std::string j2 = emitBinOp("add", "i64", jv, "1");
            emitStore("i64", j2, jAlloca);
            emitBr(loopLbl);
            em_.emitLabel(endLbl);
            blockTerminated_ = false;
            off = emitLoad("i64", offAlloca);
            emitStore("ptr", "null", srcSlot);
        }
    }

    arr = emitLoad("ptr", arrSlot);
    /* 勿清 arrSlot：返回 SSA 仍可能跨 safepoint；循环由 spill 池清 */
    return Value(arr, Type::makeArray(elemType));
}

// 生成指定元素类型的空数组
Value IRGen::genEmptyArray(const TypePtr& elemType) {
    int64_t esz = elemType ? elemType->arrayElemSize() : 8;
    std::string isPtr = isGcPointerType(elemType) ? "1" : "0";
    std::string arrRaw = emitCall("ptr", "@hao_array_new", "i64 0, i64 " + std::to_string(esz) + ", i64 " + isPtr);
    /* 空数组亦可能跨后续 safepoint，进 shadow */
    std::string slot = emitSpillGcRoot("aempty.arr", arrRaw);
    std::string arr = emitLoad("ptr", slot);
    return Value(arr, Type::makeArray(elemType));
}

// v0.32：定长数组（calloc 已零填充；可选 fill 覆盖）
Value IRGen::genSizedArray(const TypePtr& elemType, const Value& len,
                           const Value* fill, antlr4::ParserRuleContext* where) {
    if (!elemType) {
        error(where, "定长数组缺少元素类型");
        return Value();
    }
    if (!len.valid() || !len.type || !len.type->isInteger()) {
        error(where, "定长数组长度须为整数类型，实际为 " +
                     (len.valid() && len.type ? len.type->toString() : "?"));
        return Value();
    }
    // 长度按源整数有无符号扩展到 i64（勿经 Int 截断，防 UInt/Long 丢高位）
    std::string len64;
    if (len.type->llvmType() == "i64") {
        len64 = len.ir;
    } else if (len.type->isUnsigned()) {
        len64 = emitCast("zext", len.type->llvmType(), len.ir, "i64");
    } else {
        len64 = emitCast("sext", len.type->llvmType(), len.ir, "i64");
    }

    int64_t esz = elemType->arrayElemSize();
    std::string gepTy = elemType->arrayGepType();
    std::string lt = elemType->llvmType();
    std::string isPtr = isGcPointerType(elemType) ? "1" : "0";
    /* fill 须先于 array_new 挂根（分配可 safepoint） */
    Value fillHeld;
    if (fill) {
        if (!isAssignable(fill->type, elemType)) {
            error(where, "定长数组填充值类型 " + fill->type->toString() +
                         " 不能赋给 " + elemType->toString());
            return Value();
        }
        fillHeld = coerce(*fill, elemType, 0, 0);
        rootGcOperand(fillHeld);
    }
    std::string arrRaw = emitCall("ptr", "@hao_array_new", "i64 " + len64 + ", i64 " + std::to_string(esz) + ", i64 " + isPtr);
    std::string arrSlot = emitSpillGcRoot("asize.arr", arrRaw);

    if (fill) {
        Value fv = fillHeld;
        std::string fillSlot;
        if (isGcPointerType(elemType))
            fillSlot = emitSpillGcRoot("asize.fill", fv.ir);
        std::string iAlloca = em_.nextTemp();
        emitAllocaAt(iAlloca, "i64");
        emitStore("i64", "0", iAlloca);
        std::string loopLbl = em_.nextLabel("asize.loop");
        std::string bodyLbl = em_.nextLabel("asize.body");
        std::string endLbl = em_.nextLabel("asize.end");
        emitBr(loopLbl);
        em_.emitLabel(loopLbl);
        blockTerminated_ = false;
        /* v0.53.5：合成填充循环须 safepoint；arr/fill 从 shadow 重载 */
        emitSafepoint();
        std::string arr = emitLoad("ptr", arrSlot);
        std::string fillIr = fv.ir;
        if (!fillSlot.empty()) {
            fillIr = emitLoad("ptr", fillSlot);
        }
        std::string iv = emitLoad("i64", iAlloca);
        std::string cmp = emitICmp("slt", "i64", iv, len64);
        emitCondBr(cmp, bodyLbl, endLbl);
        em_.emitLabel(bodyLbl);
        blockTerminated_ = false;
        std::string p = emitGep("" + gepTy + "", arr, "i64", iv);
        emitHeapStore(p, fillIr, elemType, arr);
        std::string i2 = emitBinOp("add", "i64", iv, "1");
        emitStore("i64", i2, iAlloca);
        emitBr(loopLbl);
        em_.emitLabel(endLbl);
        blockTerminated_ = false;
        if (!fillSlot.empty())
            emitStore("ptr", "null", fillSlot);
    }

    std::string arr = emitLoad("ptr", arrSlot);
    /* 勿清 arrSlot：返回 SSA 仍可能跨 safepoint */
    return Value(arr, Type::makeArray(elemType));
}

// 表达式是否为空数组字面量 []
bool IRGen::isEmptyArrayLiteral(antlr4::tree::ParseTree* e) {
    if (!e) return false;
    // 沿单子节点链下降到 primary，再在子树里找空的 arrayLiteral
    antlr4::tree::ParseTree* node = e;
    while (node && node->children.size() == 1) node = node->children[0];
    // DFS 找第一个 arrayLiteral
    std::function<HaoLangParser::ArrayLiteralContext*(antlr4::tree::ParseTree*)> find;
    find = [&](antlr4::tree::ParseTree* n) -> HaoLangParser::ArrayLiteralContext* {
        if (!n) return nullptr;
        if (auto* al = dynamic_cast<HaoLangParser::ArrayLiteralContext*>(n))
            return al;
        for (auto* c : n->children)
            if (auto* al = find(c)) return al;
        return nullptr;
    };
    auto* al = find(node);
    return al && al->arrayElementList() == nullptr;
}

// ---------- when 表达式 ----------
//  val s = when (n) { 0 -> "zero"; else -> "many" }
//  每个分支求值后存入同一个栈变量，汇聚到 end 后统一读取。
//  用 alloca 而非 phi，避免分支体内含嵌套控制流时前驱块难以追踪。
Value IRGen::genWhenExpr(HaoLangParser::WhenStmtContext* w) {
    auto branches = w->whenBranch();
    if (branches.empty()) {
        error(w, "when 表达式至少需要一个分支");
        return Value();
    }

    HaoLangParser::WhenBranchContext* elseBranch = nullptr;
    for (auto* b : branches) {
        if (b->ELSE()) {
            if (elseBranch) {
                error(b, "when 表达式只能有一个 else 分支");
                return Value();
            }
            elseBranch = b;
        }
    }
    if (!elseBranch) {
        error(w, "when 表达式必须有 else 分支（否则无匹配时结果未定义）");
        return Value();
    }

    // 结果类型：取第一个「表达式体」分支推断
    TypePtr resultType;
    for (auto* b : branches) {
        if (b->expr()) {
            resultType = inferExprType(b->expr());
            if (resultType && !resultType->isUnknown()) break;
        }
    }
    if (!resultType || resultType->isUnknown()) {
        error(w, "when 表达式无法推断结果类型（分支体须为表达式）");
        return Value();
    }

    bool hasSubject = w->expr() != nullptr;
    Value subject;
    std::string subjAddr;
    TypePtr subjType;
    if (hasSubject) {
        subject = genExpr(w->expr());
        if (!subject.valid()) return Value();
        if (subject.type->nullable) {
            error(w->expr(), "when 主体不能是可空类型 " + subject.type->toString() +
                             "，请先用 !! 或 ??");
            return Value();
        }
        subjType = subject.type;
        if (isGcPointerType(subjType) && inLoopSpillPool()) {
            subjAddr = acquireLoopGcSlot("when.subj");
            emitStore(subjType->llvmType(), subject.ir, subjAddr);
        } else {
            subjAddr = em_.nextNamed("when.subj");
            emitAllocaAt(subjAddr, subjType->llvmType());
            emitStore(subjType->llvmType(), subject.ir, subjAddr);
            if (isGcPointerType(subjType))
                emitGcRootPush(subjAddr);
        }
    }

    std::string resAddr;
    if (isGcPointerType(resultType) && inLoopSpillPool()) {
        resAddr = acquireLoopGcSlot("when.res");
        emitStore("ptr", "null", resAddr);
    } else {
        resAddr = em_.nextNamed("when.res");
        emitAllocaAt(resAddr, resultType->llvmType());
        if (isGcPointerType(resultType)) {
            emitStore("ptr", "null", resAddr);
            emitGcRootPush(resAddr);
        }
    }
    std::string endL = em_.nextLabel("when.end");

    for (size_t bi = 0; bi < branches.size(); ++bi) {
        auto* br = branches[bi];

        auto emitBody = [&]() -> bool {
            if (br->block()) {
                error(br, "when 表达式的分支体必须是表达式，不能是语句块");
                return false;
            }
            if (!br->expr()) {
                error(br, "when 表达式分支缺少结果表达式");
                return false;
            }
            expectedTypes_.push_back(resultType);
            Value rv = genExpr(br->expr());
            expectedTypes_.pop_back();
            if (!rv.valid()) return false;
            if (!isAssignable(rv.type, resultType)) {
                error(br->expr(), "when 分支结果类型 " + rv.type->toString() +
                                  " 与推断类型 " + resultType->toString() + " 不匹配");
                return false;
            }
            rv = coerce(rv, resultType, 0, 0);
            emitStore(resultType->llvmType(), rv.ir, resAddr);
            if (!blockTerminated_) emitBr(endL);
            blockTerminated_ = true;
            return true;
        };

        if (br->ELSE()) {
            if (!emitBody()) return Value();
            continue;
        }

        std::string bodyL = em_.nextLabel("when.body");
        std::string nextL = em_.nextLabel("when.next");
        auto* el = br->exprList();
        if (!el) continue;
        auto exprs = el->expr();

        for (size_t ei = 0; ei < exprs.size(); ++ei) {
            Value cv = genExpr(exprs[ei]);
            if (!cv.valid()) return Value();

            std::string condI1;
            if (hasSubject) {
                if (cv.type->nullable) {
                    error(exprs[ei], "when 分支值不能是可空类型 " +
                                     cv.type->toString() + "，请先用 !! 或 ??");
                    return Value();
                }
                std::string sv = emitLoad(subjType->llvmType(), subjAddr);

                if (subjType->kind == TypeKind::String) {
                    if (cv.type->kind != TypeKind::String) {
                        error(exprs[ei], "when 分支值类型 " + cv.type->toString() +
                                         " 与主体类型 String 不匹配");
                        return Value();
                    }
                    std::string eqr = emitCall("i8", "@hao_str_eq", "ptr " + sv + ", ptr " + cv.ir);
                    condI1 = emitICmp("ne", "i8", eqr, "0");
                } else if (subjType->isFloating() || cv.type->isFloating()) {
                    TypePtr ft = (subjType->kind == TypeKind::Double ||
                                  cv.type->kind == TypeKind::Double)
                                     ? Type::makeDouble() : Type::makeFloat();
                    Value sdv(sv, subjType);
                    sdv = coerce(sdv, ft, 0, 0);
                    cv  = coerce(cv,  ft, 0, 0);
                    condI1 = emitFCmp("oeq", ft->llvmType(), sdv.ir, cv.ir);
                } else {
                    if (!isAssignable(cv.type, subjType) &&
                        !isAssignable(subjType, cv.type)) {
                        error(exprs[ei], "when 分支值类型 " + cv.type->toString() +
                                         " 与主体类型 " + subjType->toString() + " 不匹配");
                        return Value();
                    }
                    TypePtr ct = subjType;
                    if (subjType->isInteger() && cv.type->isInteger()) {
                        if (Type::isMixedSignedUnsigned64(subjType->kind,
                                                          cv.type->kind)) {
                            error(exprs[ei],
                                  "64 位有符号与无符号不能隐式混合，请显式转换");
                            return Value();
                        }
                        TypePtr p = Type::binaryNumericPromote(subjType->kind,
                                                              cv.type->kind);
                        if (p) ct = p;
                    }
                    Value sdv(sv, subjType);
                    sdv = coerce(sdv, ct, 0, 0);
                    cv  = coerce(cv,  ct, 0, 0);
                    condI1 = emitICmp("eq", ct->llvmType(), sdv.ir, cv.ir);
                }
            } else {
                if (cv.type->kind != TypeKind::Bool) {
                    error(exprs[ei], "无主体的 when 要求分支条件为 Bool，实际为 " +
                                     cv.type->toString());
                    return Value();
                }
                if (!ensureNonNullOperand(cv, exprs[ei], "when 条件")) return Value();
                condI1 = toI1(cv);
            }

            bool lastValue = (ei + 1 == exprs.size());
            std::string failL = lastValue ? nextL : em_.nextLabel("when.or");
            emitCondBr(condI1, bodyL, failL);
            if (!lastValue) {
                em_.emitLabel(failL);
                blockTerminated_ = false;
            }
        }

        em_.emitLabel(bodyL);
        blockTerminated_ = false;
        if (!emitBody()) return Value();

        em_.emitLabel(nextL);
        blockTerminated_ = false;
    }

    if (!blockTerminated_) emitBr(endL);
    em_.emitLabel(endL);
    blockTerminated_ = false;

    std::string out = emitLoad(resultType->llvmType(), resAddr);
    Value result(out, resultType);
    /* 先 spill 结果再清 when.res：否则仅 SSA 跨 safepoint 会假死 */
    rootGcOperand(result);
    if (isGcPointerType(resultType) && !resAddr.empty())
        emitStore("ptr", "null", resAddr);
    if (hasSubject && isGcPointerType(subjType) && !subjAddr.empty())
        emitStore("ptr", "null", subjAddr);
    return result;
}

// ---------- 字面量 ----------
Value IRGen::genLiteral(HaoLangParser::LiteralContext* lit) {
    TypePtr expect = expectedTypes_.empty() ? nullptr : expectedTypes_.back();

    if (auto* t = lit->INT_LIT()) {
        std::string s = StringUtil::stripUnderscores(t->getText());
        bool neg = (!s.empty() && s[0] == '-');
        unsigned long long uv = 0;
        long long v = 0;
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
            uv = std::strtoull(s.c_str() + 2, nullptr, 16);
        else if (s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B'))
            uv = std::strtoull(s.c_str() + 2, nullptr, 2);
        else if (neg)
            v = std::strtoll(s.c_str(), nullptr, 10);
        else
            uv = std::strtoull(s.c_str(), nullptr, 10);
        if (!neg) v = static_cast<long long>(uv);

        // 期望整数类型时按期望收窄/放宽（UInt=4000000000 等）
        if (expect && expect->isInteger()) {
            TypePtr et = Type::makeOfKind(expect->kind);
            if (et->isUnsigned() || et->kind == TypeKind::UIntPtr) {
                return Value(std::to_string(neg ? static_cast<unsigned long long>(v)
                                               : uv), et);
            }
            return Value(std::to_string(v), et);
        }

        // 无后缀：能放进 i32 则为 Int，否则 Long；超大无符号→ULong
        if (!neg && uv > static_cast<unsigned long long>(LLONG_MAX))
            return Value(std::to_string(uv), Type::makeULong());
        if (v >= static_cast<long long>(INT32_MIN) &&
            v <= static_cast<long long>(INT32_MAX))
            return Value(std::to_string(v), Type::makeInt());
        return Value(std::to_string(v), Type::makeLong());
    }

    if (auto* t = lit->FLOAT_LIT()) {
        std::string s = StringUtil::stripUnderscores(t->getText());
        if (s.find('.') == std::string::npos &&
            s.find('e') == std::string::npos &&
            s.find('E') == std::string::npos)
            s += ".0";
        if (expect && expect->kind == TypeKind::Float)
            return Value(s, Type::makeFloat());
        return Value(s, Type::makeDouble());
    }

    if (auto* t = lit->STRING_LIT()) {
        std::string content = StringUtil::unescapeStringLiteral(t->getText());
        std::string cstr = em_.internString(content);
        std::string reg = emitCall("ptr", "@hao_str_from_cstr", "ptr " + cstr);
        return Value(reg, Type::makeString());
    }

    if (auto* vs = lit->VERBATIM_STRING()) {
        std::string content = StringUtil::unescapeVerbatimString(vs->getText());
        std::string cstr = em_.internString(content);
        std::string reg = emitCall("ptr", "@hao_str_from_cstr", "ptr " + cstr);
        return Value(reg, Type::makeString());
    }

    if (lit->CHAR_LIT()) {
        uint32_t cp = StringUtil::decodeCharLiteral(lit->CHAR_LIT()->getText());
        return Value(std::to_string(cp), Type::makeChar());
    }

    if (lit->templateString())
        return genTemplateString(lit->templateString());

    if (lit->TRUE())  return Value("1", Type::makeBool());
    if (lit->FALSE()) return Value("0", Type::makeBool());
    if (lit->NULL_LIT()) return Value("null", Type::makeNull());

    error(lit, "无法识别的字面量");
    return Value();
}

// ---------- 模板字符串 ----------
Value IRGen::genTemplateString(HaoLangParser::TemplateStringContext* ts) {
    // $"a{x}b" => concat(concat("a", str(x)), "b")
    // @$" / $@" 逐字：不处理 \ 转义，"" → "
    bool verbatim = ts->VERBATIM_TEMPLATE_START() != nullptr;
    Value acc;
    bool first = true;

    for (auto* part : ts->templatePart()) {
        Value piece;

        if (auto* txt = dynamic_cast<HaoLangParser::TmplTextContext*>(part)) {
            std::string raw = txt->TEMPLATE_TEXT()->getText();
            std::string s;
            if (verbatim) {
                for (size_t i = 0; i < raw.size(); ++i) {
                    if (raw[i] == '"' && i + 1 < raw.size() && raw[i + 1] == '"') {
                        s += '"'; ++i;
                    } else if (raw[i] == '{' && i + 1 < raw.size() && raw[i + 1] == '{') {
                        s += '{'; ++i;
                    } else if (raw[i] == '}' && i + 1 < raw.size() && raw[i + 1] == '}') {
                        s += '}'; ++i;
                    } else {
                        s += raw[i];
                    }
                }
            } else {
                for (size_t i = 0; i < raw.size(); ++i) {
                    if (raw[i] == '{' && i + 1 < raw.size() && raw[i + 1] == '{') {
                        s += '{'; ++i;
                    } else if (raw[i] == '}' && i + 1 < raw.size() && raw[i + 1] == '}') {
                        s += '}'; ++i;
                    } else if (raw[i] == '\\' && i + 1 < raw.size()) {
                        switch (raw[++i]) {
                            case 'n': s += '\n'; break;
                            case 't': s += '\t'; break;
                            case 'r': s += '\r'; break;
                            case '"': s += '"'; break;
                            case '\\': s += '\\'; break;
                            case '$': s += '$'; break;
                            default: s += raw[i]; break;
                        }
                    } else {
                        s += raw[i];
                    }
                }
            }
            std::string cstr = em_.internString(s);
            std::string reg = emitCall("ptr", "@hao_str_from_cstr", "ptr " + cstr);
            piece = Value(reg, Type::makeString());
        } else if (auto* ip = dynamic_cast<HaoLangParser::TmplInterpContext*>(part)) {
            Value v = genExpr(ip->expr());
            if (!v.valid()) return Value();
            if (v.type->nullable) {
                error(ip->expr(), "模板插值不能是可空类型 " + v.type->toString() +
                                  "，请先用 !! 或 ??");
                return Value();
            }
            piece = toStringValue(v);
            if (!piece.valid()) {
                error(ip->expr(), "模板插值类型 " + v.type->toString() +
                                  " 无法转为 String");
                return Value();
            }
        } else {
            error(ts, "无法识别的模板片段");
            return Value();
        }

        if (first) {
            acc = piece;
            first = false;
            rootGcOperand(acc);
        } else {
            rootGcOperand(acc);
            rootGcOperand(piece);
            std::string reg = emitCall("ptr", "@hao_str_concat", "ptr " + acc.ir + ", ptr " + piece.ir);
            acc = Value(reg, Type::makeString());
            rootGcOperand(acc);
        }
    }

    if (first) {
        std::string cstr = em_.internString("");
        std::string reg = emitCall("ptr", "@hao_str_from_cstr", "ptr " + cstr);
        return Value(reg, Type::makeString());
    }
    return acc;
}

} // namespace hao
