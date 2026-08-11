// ============================================================
//  HaoLang IR 生成 —— 语句与控制流
// ------------------------------------------------------------
//  从 IRGen.cpp 拆分而来，逻辑保持不变。涵盖：
//    - 语句分派（genStatement）、块（genBlock）
//    - 变量声明（genVarDecl，含被 lambda 捕获的可变变量装箱）
//    - if / while / for-in / when
//    - break / continue（与异常 cleanup 链的交互见 IRGenExcept.cpp）
// ============================================================

#include "irgen/IRGen.h"

#include <functional>

namespace hao {

TypePtr IRGen::peekVarDeclType(HaoLangParser::VarDeclContext* vd) {
    if (!vd) return nullptr;
    if (vd->type()) return resolveType(vd->type());
    if (vd->expr()) return inferExprType(vd->expr());
    return nullptr;
}

void IRGen::hoistVarDeclsInLoopBody(HaoLangParser::StatementContext* body) {
    if (!body) return;
    /* blockDepth：0=循环体自身；>=1 的嵌套 { } 不提升（交给块尾清槽） */
    std::function<void(antlr4::tree::ParseTree*, int)> walk =
        [&](antlr4::tree::ParseTree* n, int blockDepth) {
            if (!n) return;
            /* 闭包另有函数帧；嵌套 while/for 自管提升 */
            if (dynamic_cast<HaoLangParser::LambdaContext*>(n)) return;
            if (dynamic_cast<HaoLangParser::WhileStmtContext*>(n)) return;
            if (dynamic_cast<HaoLangParser::ForStmtContext*>(n)) return;
            if (auto* st = dynamic_cast<HaoLangParser::StatementContext*>(n)) {
                if (st->whileStmt() || st->forStmt()) return;
                /* if/when/try/select 分支自管块作用域，勿提升进去 */
                if (st->ifStmt() || st->whenStmt() || st->tryStmt() ||
                    st->selectStmt())
                    return;
                if (auto* vd = st->varDecl()) {
                    if (loopHoisted_.count(vd)) return;
                    std::string name = vd->IDENT()->getText();
                    TypePtr type = peekVarDeclType(vd);
                    if (!type || type->isUnit() || type->isUnknown()) return;
                    bool isVal = vd->VAL() != nullptr;
                    bool boxed = !isVal && capturedVarNames_.count(name) > 0;
                    HoistedLocal h;
                    h.type = type;
                    h.boxed = boxed;
                    h.addr = em_.nextNamed(name + ".addr");
                    if (boxed) {
                        emitAllocaAt(h.addr, "ptr");
                        int64_t cbm = isGcPointerType(type) ? 1 : 0;
                        std::string cell = emitObjectNew(1, cbm);
                        emitStore("ptr", cell, h.addr);
                        emitGcRootPush(h.addr);
                    } else {
                        emitAllocaAt(h.addr, type->llvmType());
                        if (isGcPointerType(type)) {
                            emitStore("ptr", "null", h.addr);
                            emitGcRootPush(h.addr);
                        }
                    }
                    loopHoisted_[vd] = std::move(h);
                    return;
                }
                if (auto* blk = st->block()) {
                    if (blockDepth >= 1) return; /* 嵌套裸块：不提升 */
                    for (auto* s : blk->statement()) walk(s, blockDepth + 1);
                    return;
                }
                return; /* 其它语句：不深挖 */
            }
            for (auto* c : n->children) walk(c, blockDepth);
        };
    walk(body, 0);
}

void IRGen::clearHoistedGcSince(
    const std::unordered_map<HaoLangParser::VarDeclContext*, HoistedLocal>& saved) {
    for (auto& kv : loopHoisted_) {
        if (saved.count(kv.first)) continue;
        if (kv.second.boxed || isGcPointerType(kv.second.type))
            emitStore("ptr", "null", kv.second.addr);
    }
}

void IRGen::genBlock(HaoLangParser::BlockContext* blk, bool newScope) {
    if (!blk) return;
    if (newScope) {
        SymbolTable::Guard g(syms_);
        beginBlockGcScope();
        for (auto* st : blk->statement()) genStatement(st);
        endBlockGcScope();
    } else {
        for (auto* st : blk->statement()) genStatement(st);
    }
}

void IRGen::genStatement(HaoLangParser::StatementContext* st) {
    if (!st) return;
    setDebugLoc(st);
    emitRuntimeSrcLoc(locFrom(st));

    // 已终结的基本块后面的语句不可达，跳过以保证 IR 合法
    if (blockTerminated_) return;

    if (auto* v = st->varDecl())      { genVarDecl(v);   return; }
    if (auto* i = st->ifStmt())       { genIf(i);        return; }
    if (auto* w = st->whileStmt())    { genWhile(w);     return; }
    if (auto* f = st->forStmt())      { genFor(f);       return; }
    if (auto* wh = st->whenStmt())    { genWhenStmt(wh); return; }
    if (auto* t = st->tryStmt())      { genTry(t);       return; }
    if (auto* r = st->returnStmt())   { genReturn(r);    return; }
    if (auto* b = st->block())        { genBlock(b);     return; }
    if (auto* e = st->exprStmt())     { genExprStmt(e);  return; }

    // ---------- break / continue ----------
    //  在带 finally 的 try 内时，不能直接跳走，必须先穿过 finally。
    //  禁止在 emitUnwind 之前 clearLoopSpillSlots：会杀掉本层 try 的 catch
    //  池槽，finally/后续迭代假死 → 套件 AV（对 0.55.5 回归，坑债 CE）。
    if (st->breakStmt()) {
        if (loops_.empty()) { error(st, "break 只能出现在循环内部"); return; }
        if (!tryStack_.empty()) { emitUnwind(2); return; }
        /* 无 try：end 标签 leaveLoopSpillScope 负责清槽 */
        emitBr(loops_.back().breakLabel);
        blockTerminated_ = true;
        return;
    }
    if (st->continueStmt()) {
        if (loops_.empty()) { error(st, "continue 只能出现在循环内部"); return; }
        if (!tryStack_.empty()) { emitUnwind(3); return; }
        /* 无 try：while 清槽；for 走 step 前 recycle（step 上还会再 recycle） */
        if (loops_.back().clearSpillOnContinue)
            clearLoopSpillSlots();
        else
            recycleLoopSpillSlots();
        emitBr(loops_.back().continueLabel);
        blockTerminated_ = true;
        return;
    }
    if (st->throwStmt()) { genThrow(st->throwStmt()); return; }
    if (auto* hr = st->haoroutineStmt()) { genHaoroutine(hr); return; }
    if (auto* sel = st->selectStmt()) { genSelect(sel); return; }
    // 空语句 ';' 无需生成代码
}

// haoroutine { ... }：体为无参 ()->Unit 闭包，hao_thread_start 火即忘
void IRGen::genHaoroutine(HaoLangParser::HaoroutineStmtContext* st) {
    if (!st || !st->lambda()) return;
    auto* lam = st->lambda();
    if (lam->lambdaParams()) {
        error(st, "haoroutine 体不能带参数列表，请写 haoroutine { ... }");
        return;
    }
    TypePtr unitFn = Type::makeFunc({}, Type::makeUnit());
    expectedTypes_.push_back(unitFn);
    Value env = genLambda(lam);
    expectedTypes_.pop_back();
    if (!env.valid()) return;
    rootGcOperand(env); /* env 跨 thread_start（内含 alloc/safepoint） */
    // 必须接住返回值：裸 call i64 仍占用 SSA 编号，否则后续 %N 冲突
    (void)emitCall("i64", "@hao_thread_start", "ptr " + env.ir);
}

// select { case x = ch.recv(): ... case ch.send(v): ... default: ... }
// 用 try_send/try_recv 轮询；无 default 时短 sleep + safepoint（协作等待）。
void IRGen::genSelect(HaoLangParser::SelectStmtContext* st) {
    if (!st) return;
    auto cases = st->selectCase();
    if (cases.empty()) {
        error(st, "select 至少需要一个 case 或 default");
        return;
    }

    struct CaseInfo {
        bool isDefault = false;
        bool isRecv = false;
        std::string bindName;          // recv 绑定变量
        std::string method;           // recv/recvInt/recvStr/send/...
        HaoLangParser::ExprContext* chExpr = nullptr;
        HaoLangParser::ExprContext* sendArg = nullptr;
        std::vector<HaoLangParser::StatementContext*> body;
        // 预求值
        std::string hPtr;             // Long? 盒 ptr（channel.h）
        std::string sendBits;         // i64
        std::string outAlloca;        // recv 用 i64 alloca
        TypePtr bindType;
        std::string bodyLabel;
        std::string nextTryLabel;
    };

    std::vector<CaseInfo> infos;
    int defaultCount = 0;
    for (auto* c : cases) {
        CaseInfo ci;
        ci.body = c->statement();
        if (c->DEFAULT()) {
            ci.isDefault = true;
            ++defaultCount;
            infos.push_back(std::move(ci));
            continue;
        }
        auto* comm = c->selectComm();
        if (!comm) {
            error(c, "select case 缺少通信操作");
            return;
        }
        if (comm->ASSIGN()) {
            // x = ch.method()
            ci.isRecv = true;
            ci.bindName = comm->IDENT(0)->getText();
            ci.method = comm->IDENT(1)->getText();
            ci.chExpr = comm->expr(0);
            if (ci.method != "recv" && ci.method != "recvInt" && ci.method != "recvStr") {
                error(comm, "select recv 仅支持 ch.recv() / recvInt() / recvStr()，实际为 "
                            + ci.method + "()");
                return;
            }
            if (ci.method == "recv") ci.bindType = Type::makeLong();
            else if (ci.method == "recvInt") ci.bindType = Type::makeInt();
            else {
                auto t = Type::makeString();
                t->nullable = true;
                ci.bindType = t;
            }
        } else {
            ci.isRecv = false;
            ci.method = comm->IDENT(0)->getText();
            ci.chExpr = comm->expr(0);
            ci.sendArg = comm->expr(1);
            if (ci.method != "send" && ci.method != "sendInt" && ci.method != "sendStr") {
                error(comm, "select send 仅支持 ch.send(e) / sendInt(e) / sendStr(e)，实际为 "
                            + ci.method + "(...)");
                return;
            }
        }
        infos.push_back(std::move(ci));
    }
    if (defaultCount > 1) {
        error(st, "select 最多一个 default");
        return;
    }

    // 预求值各 case 的 channel / send 载荷（在循环外，避免重复副作用）
    std::vector<std::string> selSpillSlots;
    for (auto& ci : infos) {
        if (ci.isDefault) continue;
        Value ch = genExpr(ci.chExpr);
        if (!ch.valid()) return;
        if (!ch.type || ch.type->kind != TypeKind::Class) {
            error(ci.chExpr, "select 操作对象必须是 channel.Channel");
            return;
        }
        auto cls = classOfType(ch.type);
        if (!cls) {
            error(ci.chExpr, "找不到类型 " + ch.type->toString());
            return;
        }
        int hSlot = -1;
        for (const auto& f : cls->fields) {
            if (f.name == "h" && !f.isStatic) { hSlot = f.slot; break; }
        }
        if (hSlot < 0) {
            error(ci.chExpr, "类型 " + ch.type->toString() + " 不是 Channel（无 h 句柄字段）");
            return;
        }
        if (!ensureNonNullOperand(ch, ci.chExpr, "select channel")) return;
        std::string fp = fieldPtr(ch.ir, hSlot);
        std::string hLoaded = emitLoad("ptr", fp);
        // 句柄盒跨 select 循环 safepoint：spill 进 shadow
        std::string hSlotAddr = emitSpillGcRoot("sel.h", hLoaded);
        selSpillSlots.push_back(hSlotAddr);
        ci.hPtr = emitLoad("ptr", hSlotAddr);

        if (ci.isRecv) {
            ci.outAlloca = em_.nextNamed("sel.out");
            emitAllocaAt(ci.outAlloca, "i64");
        } else {
            Value arg = genExpr(ci.sendArg);
            if (!arg.valid()) return;
            if (ci.method == "sendStr") {
                if (!arg.type || arg.type->kind != TypeKind::String || arg.type->nullable) {
                    error(ci.sendArg, "sendStr 需要非空 String");
                    return;
                }
                if (!ensureNonNullOperand(arg, ci.sendArg, "sendStr")) return;
                // String 跨循环 safepoint：先 spill 再 ptrtoint
                std::string strSlot = emitSpillGcRoot("sel.sstr", arg.ir);
                selSpillSlots.push_back(strSlot);
                std::string sp = emitLoad("ptr", strSlot);
                ci.sendBits = em_.nextTemp();
                ci.sendBits = emitPtrToInt("i64", sp);
            } else if (ci.method == "sendInt") {
                if (!arg.type || arg.type->kind != TypeKind::Int) {
                    error(ci.sendArg, "sendInt 需要 Int");
                    return;
                }
                arg = coerce(arg, Type::makeLong(),
                             ci.sendArg->getStart()->getLine(),
                             ci.sendArg->getStart()->getCharPositionInLine());
                ci.sendBits = arg.ir;
            } else {
                // send(Long)
                if (!arg.type ||
                    (arg.type->kind != TypeKind::Long && arg.type->kind != TypeKind::Int)) {
                    error(ci.sendArg, "send 需要 Long 或 Int");
                    return;
                }
                if (arg.type->kind == TypeKind::Int)
                    arg = coerce(arg, Type::makeLong(),
                                 ci.sendArg->getStart()->getLine(),
                                 ci.sendArg->getStart()->getCharPositionInLine());
                ci.sendBits = arg.ir;
            }
        }
    }

    std::string rot = em_.nextNamed("sel.rot");
    emitAllocaAt(rot, "i32");
    emitStore("i32", "0", rot);

    std::string loopL = em_.nextLabel("sel.loop");
    std::string endL = em_.nextLabel("sel.end");
    int nComm = 0;
    for (auto& ci : infos) if (!ci.isDefault) ++nComm;
    for (size_t i = 0; i < infos.size(); ++i) {
        infos[i].bodyLabel = em_.nextLabel("sel.body" + std::to_string(i));
        infos[i].nextTryLabel = em_.nextLabel("sel.try" + std::to_string(i));
    }
    std::string defaultL;
    for (auto& ci : infos)
        if (ci.isDefault) { defaultL = ci.bodyLabel; break; }
    std::string sleepL = em_.nextLabel("sel.sleep");

    emitBr(loopL);
    em_.emitLabel(loopL);
    blockTerminated_ = false;
    emitSafepoint();

    // 公平轮转起点
    std::string startV = emitLoad("i32", rot);
    if (nComm > 0) {
        std::string startMod = emitBinOp("srem", "i32", startV, std::to_string(nComm));
        // 更新 rot
        std::string nextRot = emitBinOp("add", "i32", startV, "1");
        emitStore("i32", nextRot, rot);

        // 按 (start+k)%nComm 顺序尝试通信 case（跳过 default）
        std::vector<int> commIdx;
        for (int i = 0; i < (int)infos.size(); ++i)
            if (!infos[i].isDefault) commIdx.push_back(i);

        // 展开：对每个 offset 生成比较链太重；直接固定顺序尝试但用 rot 选第一个检查的
        // 简化：顺序尝试所有 comm，rot 仅作扰动记录；仍保证有进展
        (void)startMod;
        std::string firstTry = infos[commIdx[0]].nextTryLabel;
        // 用 switch on startMod 跳到不同起点（L5：整条 switch 附 !dbg）
        std::string swDef = em_.nextLabel("sel.sw.def");
        std::string sw = "switch i32 " + startMod + ", label %" + swDef + " [";
        for (int k = 0; k < (int)commIdx.size(); ++k) {
            sw += " i32 " + std::to_string(k) + ", label %" +
                  infos[commIdx[k]].nextTryLabel;
        }
        sw += " ]" + ops_.dbgSuffix();
        em_.emit(sw);
        em_.emitLabel(swDef);
        emitBr(firstTry);

        for (int k = 0; k < (int)commIdx.size(); ++k) {
            int idx = commIdx[k];
            int nextK = (k + 1) % (int)commIdx.size();
            bool last = (k + 1 == (int)commIdx.size());
            auto& ci = infos[idx];
            em_.emitLabel(ci.nextTryLabel);
            blockTerminated_ = false;
            std::string ok;
            if (ci.isRecv) {
                ok = emitCall("i32", "@hao_chan_try_recv",
                              "ptr " + ci.hPtr + ", ptr " + ci.outAlloca);
            } else {
                ok = emitCall("i32", "@hao_chan_try_send",
                              "ptr " + ci.hPtr + ", i64 " + ci.sendBits);
                // try_send: 0 成功，非 0 失败 —— 与 recv 相反！
                // recv: 1 成功 0 失败；send: 0 成功 1 失败
            }
            std::string ready;
            if (ci.isRecv) {
                ready = emitICmp("ne", "i32", ok, "0");
            } else {
                ready = emitICmp("eq", "i32", ok, "0");
            }
            std::string failL = last
                ? (defaultL.empty() ? sleepL : defaultL)
                : infos[commIdx[nextK]].nextTryLabel;
            // last 失败时：若有 default 走 default，否则 sleep 重试
            // 但 nextK 在 last 时绕回 0 —— 不能绕回，应去 sleep/default
            if (last)
                failL = defaultL.empty() ? sleepL : defaultL;
            else
                failL = infos[commIdx[k + 1]].nextTryLabel;

            emitCondBr(ready, ci.bodyLabel, failL);
        }
    } else {
        // 只有 default
        if (defaultL.empty()) {
            error(st, "select 没有可通信的 case");
            return;
        }
        emitBr(defaultL);
    }

    if (!defaultL.empty() && nComm > 0) {
        // default 标签在 infos 里，下面统一生成 body；sleep 用于无 default
    }
    em_.emitLabel(sleepL);
    blockTerminated_ = false;
    if (defaultL.empty()) {
        emitCallVoid("@hao_thread_sleep_ms", "i32 1");
        emitBr(loopL);
    } else if (nComm == 0) {
        emitBr(defaultL);
    } else {
        // 有 default 时失败路径已直接进 default，sleep 不可达；占位
        emitBr(endL);
    }

    // ---- 各 case 体 ----
    for (size_t i = 0; i < infos.size(); ++i) {
        auto& ci = infos[i];
        em_.emitLabel(ci.bodyLabel);
        blockTerminated_ = false;
        SymbolTable::Guard g(syms_);
        beginBlockGcScope();
        if (!ci.isDefault && ci.isRecv) {
            std::string bits = emitLoad("i64", ci.outAlloca);
            std::string addr;
            bool poolBind = isGcPointerType(ci.bindType) && inLoopSpillPool();
            if (poolBind)
                addr = acquireLoopGcSlot(ci.bindName + ".addr");
            else {
                addr = em_.nextNamed(ci.bindName + ".addr");
                emitAllocaAt(addr, ci.bindType->llvmType());
            }
            if (ci.method == "recv") {
                emitStore("i64", bits, addr);
            } else if (ci.method == "recvInt") {
                std::string tr = emitCast("trunc", "i64", bits, "i32");
                emitStore("i32", tr, addr);
            } else {
                // recvStr → String?
                std::string p = emitIntToPtr("i64", bits);
                emitStore("ptr", p, addr);
            }
            if (isGcPointerType(ci.bindType) && !poolBind) {
                emitGcRootPush(addr);
                noteBlockGcSlot(addr);
            }
            /* poolBind：池槽勿 noteBlock */
            auto sym = std::make_shared<Symbol>();
            sym->kind = SymbolKind::Variable;
            sym->name = ci.bindName;
            sym->type = ci.bindType;
            sym->isMutable = true;
            sym->irAddr = addr;
            sym->line = st->getStart()->getLine();
            syms_.declare(sym);
        }
        for (auto* s : ci.body) genStatement(s);
        endBlockGcScope();
        if (!blockTerminated_) emitBr(endL);
    }

    em_.emitLabel(endL);
    blockTerminated_ = false;
    for (const auto& slot : selSpillSlots)
        emitStore("ptr", "null", slot);
}

void IRGen::genVarDecl(HaoLangParser::VarDeclContext* vd) {
    std::string name = vd->IDENT()->getText();
    bool isVal = vd->VAL() != nullptr;

    // 标注类型（可无）
    TypePtr declared = vd->type() ? resolveType(vd->type()) : nullptr;

    // 初始值（可无）
    Value init;
    bool hasInit = vd->expr() != nullptr;
    if (hasInit) {
        // 把标注类型压栈，供 lambda 推断参数/返回类型
        if (declared) expectedTypes_.push_back(declared);
        // 带类型标注的空数组字面量 []：按标注的元素类型生成，
        // 避免无标注时退化为 [Int]，导致后续 push 其它类型报类型错。
        if (declared && declared->kind == TypeKind::Array &&
            isEmptyArrayLiteral(vd->expr())) {
            TypePtr elem = declared->elem ? declared->elem : Type::makeInt();
            init = genEmptyArray(elem);
        } else {
            init = genExpr(vd->expr());
        }
        if (declared) expectedTypes_.pop_back();
        if (!init.valid()) return;
    }

    // 确定最终类型：优先标注，否则由初始值推断
    TypePtr type;
    if (declared) {
        type = declared;
        if (hasInit) {
            if (!isAssignable(init.type, declared)) {
                error(vd, "无法将 " + init.type->toString() +
                          " 赋值给 " + declared->toString() + " 类型的变量 '" + name + "'");
                return;
            }
            init = coerce(init, declared,
                          vd->getStart()->getLine(),
                          vd->getStart()->getCharPositionInLine());
        }
    } else if (hasInit) {
        type = init.type;
    } else {
        error(vd, "变量 '" + name + "' 既无类型标注也无初始值，无法推断类型");
        return;
    }

    if (type->isUnit()) {
        error(vd, "变量 '" + name + "' 不能为 Unit 类型");
        return;
    }

    // 当前层重复声明检查（允许遮蔽外层）
    if (syms_.lookupLocal(name)) {
        error(vd, "变量 '" + name + "' 在当前作用域中重复声明");
        return;
    }

    // 循环提升：复用循环前 alloca/push，本轮只写初值（覆盖即放弃上轮对象）
    auto hit = loopHoisted_.find(vd);
    if (hit != loopHoisted_.end()) {
        const HoistedLocal& h = hit->second;
        if (h.boxed) {
            std::string cell = emitLoad("ptr", h.addr);
            if (hasInit)
                emitHeapStore(cell, init.ir, type, cell);
        } else if (hasInit) {
            emitStore(type->llvmType(), init.ir, h.addr);
        } else if (isGcPointerType(type)) {
            emitStore("ptr", "null", h.addr);
        }
        auto sym = std::make_shared<Symbol>();
        sym->kind = SymbolKind::Variable;
        sym->name = name;
        sym->type = type;
        sym->isMutable = !isVal;
        sym->irAddr = h.addr;
        sym->boxed = h.boxed;
        sym->line = vd->getStart()->getLine();
        syms_.declare(sym);
        emitDbgDeclareIf(h.addr, name, sym->line, 0);
        if (hasInit)
            emitDbgValueIf(type->llvmType(), init.ir, name, sym->line, 0);
        return;
    }

    // 未提升但仍在循环 spill 池内的 GC 局部：走池槽（补 Unknown 等 hoist 缺口）
    bool boxed = !isVal && capturedVarNames_.count(name) > 0;
    if (!boxed && isGcPointerType(type) && inLoopSpillPool()) {
        std::string addr = acquireLoopGcSlot(name + ".addr");
        if (hasInit)
            emitStore(type->llvmType(), init.ir, addr);
        else
            emitStore("ptr", "null", addr);
        /* 池槽勿 noteBlockGcSlot：块尾 null 会误杀 spill 池（与 continue 清槽同类） */
        auto sym = std::make_shared<Symbol>();
        sym->kind = SymbolKind::Variable;
        sym->name = name;
        sym->type = type;
        sym->isMutable = !isVal;
        sym->irAddr = addr;
        sym->boxed = false;
        sym->line = vd->getStart()->getLine();
        syms_.declare(sym);
        emitDbgDeclareIf(addr, name, sym->line, 0);
        if (hasInit)
            emitDbgValueIf(type->llvmType(), init.ir, name, sym->line, 0);
        return;
    }

    std::string addr = em_.nextNamed(name + ".addr");

    // 被 lambda 捕获的可变 var 装箱到堆 cell，与闭包共享；
    // val 不可变，按值捕获即可，无需装箱。
    if (boxed) {
        emitAllocaAt(addr, "ptr");
        int64_t cbm = isGcPointerType(type) ? 1 : 0;
        std::string cell = emitObjectNew(1, cbm);
        if (hasInit)
            emitHeapStore(cell, init.ir, type, cell);
        emitStore("ptr", cell, addr);
        emitGcRootPush(addr);
        noteBlockGcSlot(addr);
    } else {
        emitAllocaAt(addr, type->llvmType());
        if (hasInit) {
            emitStore(type->llvmType(), init.ir, addr);
        }
        if (isGcPointerType(type)) {
            emitGcRootPush(addr);
            noteBlockGcSlot(addr);
        }
    }

    auto sym = std::make_shared<Symbol>();
    sym->kind = SymbolKind::Variable;
    sym->name = name;
    sym->type = type;
    sym->isMutable = !isVal;
    sym->irAddr = addr;
    sym->boxed = boxed;
    sym->line = vd->getStart()->getLine();
    syms_.declare(sym);
    emitDbgDeclareIf(addr, name, sym->line, 0);
    if (hasInit && !boxed)
        emitDbgValueIf(type->llvmType(), init.ir, name, sym->line, 0);
}

void IRGen::genIf(HaoLangParser::IfStmtContext* st) {
    Value cond = genExpr(st->expr());
    if (!cond.valid()) return;

    if (cond.type->kind != TypeKind::Bool) {
        error(st->expr(), "if 条件必须是 Bool 类型，实际为 " + cond.type->toString());
        return;
    }
    if (!ensureNonNullOperand(cond, st->expr(), "if 条件")) return;

    NullCheckFact fact = analyzeNullCheck(st->expr());
    TypePtr narrowed;
    if (!fact.name.empty()) {
        if (auto sym = syms_.lookup(fact.name)) {
            narrowed = std::make_shared<Type>(*sym->type);
            narrowed->nullable = false;
        }
    }

    std::string i1 = toI1(cond);
    bool hasElse = st->statement().size() > 1;

    std::string thenL = em_.nextLabel("if.then");
    std::string elseL = hasElse ? em_.nextLabel("if.else") : "";
    std::string endL  = em_.nextLabel("if.end");

    emitCondBr(i1, thenL, hasElse ? elseL : endL);

    // ---- then 分支 ----
    em_.emitLabel(thenL);
    blockTerminated_ = false;
    pushSmartCastFrame();
    if (narrowed && fact.isNotNull) addSmartCast(fact.name, narrowed);
    {
        SymbolTable::Guard gThen(syms_);
        beginBlockGcScope();
        genStatement(st->statement(0));
        endBlockGcScope();
    }
    bool thenTerm = blockTerminated_;
    popSmartCastFrame();
    if (!thenTerm) emitBr(endL);

    // ---- else 分支 ----
    bool elseTerm = false;
    if (hasElse) {
        em_.emitLabel(elseL);
        blockTerminated_ = false;
        pushSmartCastFrame();
        // if (x == null) ... else { x 非空 }
        if (narrowed && !fact.isNotNull) addSmartCast(fact.name, narrowed);
        // if (x != null) ... else { 无收窄 }
        {
            SymbolTable::Guard gElse(syms_);
            beginBlockGcScope();
            genStatement(st->statement(1));
            endBlockGcScope();
        }
        elseTerm = blockTerminated_;
        popSmartCastFrame();
        if (!elseTerm) emitBr(endL);
    }

    // 两条分支都返回了，if.end 不可达；但仍需发射标签以保证 IR 结构合法，
    // 并追加 unreachable 让 LLVM 知道此处不可达。
    em_.emitLabel(endL);
    if (hasElse && thenTerm && elseTerm) {
        emitUnreachable();
        blockTerminated_ = true;
    } else {
        blockTerminated_ = false;
        // if (x == null) return; —— then 终结且无 else：后续代码中 x 非空
        if (!hasElse && thenTerm && narrowed && !fact.isNotNull)
            addSmartCast(fact.name, narrowed);
    }
}

void IRGen::genWhile(HaoLangParser::WhileStmtContext* st) {
    std::string condL = em_.nextLabel("while.cond");
    std::string bodyL = em_.nextLabel("while.body");
    std::string endL  = em_.nextLabel("while.end");

    /* 提升体内 var + 进入 spill 池：同一槽每轮 store，旧对象失去根 */
    enterLoopSpillScope();
    auto savedHoist = loopHoisted_;
    hoistVarDeclsInLoopBody(st->statement());
    /* 保护提升槽：cond clear 不得抹 hoist */
    pinLoopSpillCheckpoint();
    markLoopSpillStickyFloor();

    emitBr(condL);

    // ---- 条件：先求值（挂 GC 根）再 safepoint；条件 spill sticky 至循环结束 ----
    em_.emitLabel(condL);
    blockTerminated_ = false;
    /*
     * sticky 层：enter 时 pin 保护 hoist；每轮再 pin 条件根。
     * 只卸本层 floor 以上的条件 pin，保留 hoist + 外层 sticky。
     * 旧逻辑 size>1 会在嵌套 while 生成期剥掉外层，把块内 junk 池槽
     * store null 写进内层 cond（每轮杀 List → add AV）。
     */
    if (!loopSpillPools_.empty()) {
        auto& pool = loopSpillPools_.back();
        size_t floor = pool.stickyFloorStack.empty()
            ? 1
            : pool.stickyFloorStack.back();
        while (pool.stickyStack.size() > floor)
            unpinLoopSpillCheckpoint();
    }
    /* continue 穿过 finally 会跳过 body 尾 clear；在此补清本层非 sticky spill */
    clearLoopSpillSlots();
    Value cond = genExpr(st->expr());
    if (!cond.valid()) {
        clearHoistedGcSince(savedHoist);
        loopHoisted_ = std::move(savedHoist);
        leaveLoopSpillScope();
        return;
    }
    if (cond.type->kind != TypeKind::Bool) {
        error(st->expr(), "while 条件必须是 Bool 类型，实际为 " + cond.type->toString());
        clearHoistedGcSince(savedHoist);
        loopHoisted_ = std::move(savedHoist);
        leaveLoopSpillScope();
        return;
    }
    if (!ensureNonNullOperand(cond, st->expr(), "while 条件")) {
        clearHoistedGcSince(savedHoist);
        loopHoisted_ = std::move(savedHoist);
        leaveLoopSpillScope();
        return;
    }
    /* 条件 rootGcOperand spill 钉住（如 while (i < s.length) 的 s） */
    pinLoopSpillCheckpoint();
    emitSafepoint();
    emitCondBr(toI1(cond), bodyL, endL);

    // ---- 循环体 ----
    // continue 回到条件判断，break 跳出循环
    em_.emitLabel(bodyL);
    blockTerminated_ = false;
    LoopContext lc;
    lc.breakLabel = endL;
    lc.continueLabel = condL;
    lc.tryDepth = static_cast<int>(tryStack_.size());
    lc.clearSpillOnContinue = true;
    loops_.push_back(lc);
    NullCheckFact wfact = analyzeNullCheck(st->expr());
    pushSmartCastFrame();
    if (!wfact.name.empty() && wfact.isNotNull) {
        if (auto sym = syms_.lookup(wfact.name)) {
            auto nn = std::make_shared<Type>(*sym->type);
            nn->nullable = false;
            addSmartCast(wfact.name, nn);
        }
    }
    genStatement(st->statement());
    popSmartCastFrame();
    if (!blockTerminated_) {
        /* 本轮合成根 / 未提升局部槽清 null，下轮复用（不 root_push） */
        clearLoopSpillSlots();
        emitBr(condL);
    }
    loops_.pop_back();

    em_.emitLabel(endL);
    blockTerminated_ = false;
    clearHoistedGcSince(savedHoist);
    loopHoisted_ = std::move(savedHoist);
    leaveLoopSpillScope();
}

// ============================================================
//  for ... in ...
// ------------------------------------------------------------
//  当前仅支持遍历数组：
//      for (x in arr) { ... }
//  降级为带索引的 while 循环：
//      var i = 0
//      while (i < arr.length) { val x = arr[i]; ...; i = i + 1 }
// ============================================================

void IRGen::genFor(HaoLangParser::ForStmtContext* st) {
    std::string varName = st->IDENT()->getText();

    Value seq = genExpr(st->expr());
    if (!seq.valid()) return;
    if (seq.type->nullable) {
        error(st->expr(), "for-in 序列不能是可空类型 " + seq.type->toString() +
                          "，请先用 !! 或 ??");
        return;
    }

    // 序列类型在类/接口上查找「可迭代接口」：含 iterator(): Iterator<X> 方法、
    // 返回类型是带 1 个类型参数的接口（如 Iterable<X>/MyIterable<X>）。元素类型 X
    // 取 iterator() 返回类型（Iterator<X>）的 typeArgs[0]。不依赖接口名硬编码。
    auto findIterable = [&](std::string& itfName, TypePtr& elemType) -> bool {
        auto usable = [&](InterfaceInfoPtr ii) -> bool {
            const MethodInfo* im = ii ? ii->findMethod("iterator") : nullptr;
            return im && im->returnType &&
                   im->returnType->kind == TypeKind::Interface &&
                   im->returnType->typeArgs.size() == 1;
        };
        // ---- 接口类型本身是可迭代接口（如 Iterable<X>）----
        if (seq.type->kind == TypeKind::Interface) {
            std::string iname = seq.type->typeArgs.empty()
                ? seq.type->className : seq.type->monoName();
            auto ii = lookupInterface(iname);
            if (!usable(ii)) return false;
            itfName = iname;
            elemType = ii->findMethod("iterator")->returnType->typeArgs[0];
            return true;
        }
        // ---- 类实现可迭代接口（沿继承链）----
        if (seq.type->kind == TypeKind::Class && !seq.type->nullable) {
            auto ci = classOfType(seq.type);
            if (!ci) return false;
            for (const ClassInfo* c = ci.get(); c; c = c->base)
                for (const auto& in : c->interfaceNames) {
                    auto ii = lookupInterface(in);
                    if (!usable(ii)) continue;
                    itfName = in;
                    elemType = ii->findMethod("iterator")->returnType->typeArgs[0];
                    return true;
                }
        }
        return false;
    };

    // ===== 1. 数组快路径 =====
    if (seq.type->kind == TypeKind::Array) {
        genForArray(st, varName, seq);
        return;
    }

    // ===== 2. Iterable 接口路径（v0.18.0，替代 v0.15.0 的 toArray duck-typing）=====
    std::string itfName; TypePtr elemType;
    if (findIterable(itfName, elemType)) {
        genForIterable(st, varName, seq, lookupInterface(itfName), elemType);
        return;
    }

    // ===== 3. toArray 兜底（v0.15.0 兼容：用户自定义类只提供 toArray 也能迭代）=====
    if (seq.type->kind == TypeKind::Class && !seq.type->nullable) {
        auto ci = classOfType(seq.type);
        const MethodInfo* mi = ci ? ci->findMethod("toArray") : nullptr;
        if (mi && mi->returnType && mi->returnType->kind == TypeKind::Array &&
            !mi->isAbstract && canAccessMember(mi->visibility, mi->ownerClass)) {
            /* toArray 内可能 safepoint：序列先进 shadow */
            Value seqRoot = seq;
            rootGcOperand(seqRoot);
            std::string argStr = "ptr " + seqRoot.ir;
            if (mi->vtableSlot >= 0 && ci->hasVTable) {
                std::string vtp = emitGep("ptr", seqRoot.ir, "i64", "0");
                std::string vt = emitLoad("ptr", vtp);
                std::string mp = emitGep("ptr", vt, "i64", std::to_string(mi->vtableSlot));
                std::string fp = emitLoad("ptr", mp);
                std::string reg = emitCall("ptr", fp, argStr);
                genForArray(st, varName, Value(reg, mi->returnType));
            } else {
                std::string reg = emitCall("ptr", mi->irName, argStr);
                genForArray(st, varName, Value(reg, mi->returnType));
            }
            return;
        }
    }

    error(st->expr(), "for ... in 需要遍历数组，或实现 Iterable<T>，或提供 public toArray(): [T] 方法（实际为 " +
           seq.type->toString() + "）");
}

// 数组迭代循环体（快路径 / toArray 兜底共用）
void IRGen::genForArray(HaoLangParser::ForStmtContext* st, const std::string& varName,
                        const Value& seq) {
    TypePtr elemType = seq.type->elem ? seq.type->elem : Type::makeInt();

    enterLoopSpillScope();

    // 序列进 spill 池（嵌套 while 时复用外层槽，勿每轮 root_push）
    std::string seqAddr = emitSpillGcRoot("for.seq", seq.ir);
    pinLoopSpillCheckpoint();

    // 长度也只求一次
    std::string lenReg = emitCall("i64", "@hao_array_len", "ptr " + seq.ir);
    std::string lenAddr = em_.nextNamed("for.len");
    emitAllocaAt(lenAddr, "i64");
    emitStore("i64", lenReg, lenAddr);

    // 索引变量
    std::string idxAddr = em_.nextNamed("for.idx");
    emitAllocaAt(idxAddr, "i64");
    emitStore("i64", "0", idxAddr);

    /* 循环变量：池内复用 / 池外一次 push */
    std::string varAddr;
    if (isGcPointerType(elemType) && inLoopSpillPool()) {
        varAddr = acquireLoopGcSlot(varName + ".addr");
        pinLoopSpillCheckpoint(); /* 保护 seq+var，体 spill 可 recycle */
    } else {
        varAddr = em_.nextNamed(varName + ".addr");
        emitAllocaAt(varAddr, elemType->llvmType());
        if (isGcPointerType(elemType)) {
            emitStore("ptr", "null", varAddr);
            emitGcRootPush(varAddr);
        }
    }

    /* 体内其它 var 一并提升（与 while 同纪律） */
    auto savedHoist = loopHoisted_;
    hoistVarDeclsInLoopBody(st->statement());

    std::string condL = em_.nextLabel("for.cond");
    std::string bodyL = em_.nextLabel("for.body");
    std::string stepL = em_.nextLabel("for.step");
    std::string endL  = em_.nextLabel("for.end");

    emitBr(condL);

    // ---- 条件：idx < len；seq 已在 sticky，safepoint 在判定之后 ----
    em_.emitLabel(condL);
    blockTerminated_ = false;
    std::string iv = emitLoad("i64", idxAddr);
    std::string lv = emitLoad("i64", lenAddr);
    std::string cmp = emitICmp("slt", "i64", iv, lv);
    emitSafepoint();
    emitCondBr(cmp, bodyL, endL);

    // ---- 循环体 ----
    em_.emitLabel(bodyL);
    blockTerminated_ = false;
    {
        // 循环变量作用域限定在循环体内，每轮重新绑定
        SymbolTable::Guard g(syms_);

        std::string iv2 = emitLoad("i64", idxAddr);
        std::string seqv = emitLoad("ptr", seqAddr);

        // 索引已由 cond 保证在范围内，无需再做边界检查
        std::string gepTy = elemType->arrayGepType();
        std::string elemPtr = emitGep(gepTy, seqv, "i64", iv2);
        std::string elemVal = emitLoad(elemType->llvmType(), elemPtr);

        emitStore(elemType->llvmType(), elemVal, varAddr);

        auto sym = std::make_shared<Symbol>();
        sym->kind = SymbolKind::Variable;
        sym->name = varName;
        sym->type = elemType;
        sym->isMutable = false;    // 循环变量不可赋值
        sym->irAddr = varAddr;
        syms_.declare(sym);
        int forLine = st->getStart() ? static_cast<int>(st->getStart()->getLine()) : 1;
        emitDbgDeclareIf(varAddr, varName, forLine, 0);
        /* D11：每轮迭代写回薄 dbg.value */
        emitDbgValueIf(elemType->llvmType(), elemVal, varName, forLine, 0);

        LoopContext flc;
        flc.breakLabel = endL;
        flc.continueLabel = stepL;
        flc.tryDepth = static_cast<int>(tryStack_.size());
        flc.clearSpillOnContinue = false; /* step 上 recycle */
        loops_.push_back(flc);
        genStatement(st->statement());
        loops_.pop_back();
    }
    if (!blockTerminated_) emitBr(stepL);

    // ---- 递增（continue 亦入此；回收体 spill，保护 seq/var）----
    em_.emitLabel(stepL);
    blockTerminated_ = false;
    recycleLoopSpillSlots();
    std::string iv3 = emitLoad("i64", idxAddr);
    std::string inc = emitBinOp("add", "i64", iv3, "1");
    emitStore("i64", inc, idxAddr);
    emitBr(condL);

    em_.emitLabel(endL);
    blockTerminated_ = false;
    if (isGcPointerType(elemType))
        emitStore("ptr", "null", varAddr);
    emitStore("ptr", "null", seqAddr);
    if (isGcPointerType(elemType))
        unpinLoopSpillCheckpoint(); /* var 保护层 */
    unpinLoopSpillCheckpoint();     /* seq 保护层 */
    clearHoistedGcSince(savedHoist);
    loopHoisted_ = std::move(savedHoist);
    leaveLoopSpillScope();
}

// Iterable<X> 接口迭代循环体（v0.18.0）
//   iter = seq.iterator()          （Iterable 接口虚表分派）
//   while (iter.hasNext()) {       （Iterator 接口虚表分派）
//       x = iter.next();
//       ...body...
//   }
void IRGen::genForIterable(HaoLangParser::ForStmtContext* st, const std::string& varName,
                           const Value& seq, InterfaceInfoPtr itf, const TypePtr& elemType) {
    const MethodInfo* im = itf ? itf->findMethod("iterator") : nullptr;
    if (!im || !im->returnType || im->returnType->kind != TypeKind::Interface) {
        error(st->expr(), "Iterable 接口缺少 iterator(): Iterator<T> 方法");
        return;
    }
    // Iterator<X> 接口实例（hasNext/next 槽位）
    std::string iterIname = im->returnType->typeArgs.empty()
        ? im->returnType->className : im->returnType->monoName();
    auto itfI = lookupInterface(iterIname);
    const MethodInfo* hmi = itfI ? itfI->findMethod("hasNext") : nullptr;
    const MethodInfo* nmi = itfI ? itfI->findMethod("next") : nullptr;
    if (!itfI || !hmi || !nmi) {
        error(st->expr(), "Iterator 接口缺少 hasNext()/next() 方法");
        return;
    }

    // 虚表分派：seq.iterator() -> Iterator<X>
    auto dispatch = [&](const std::string& recv, InterfaceInfoPtr ii, const MethodInfo* m,
                        const std::string& retType) -> std::string {
        std::string vtp = emitGep("ptr", recv, "i64", "0");
        std::string vt = emitLoad("ptr", vtp);
        std::string mp = emitGep("ptr", vt, "i64", std::to_string(m->vtableSlot));
        std::string fp = emitLoad("ptr", mp);
        std::string reg = emitCall(retType, fp, "ptr " + recv);
        return reg;
    };

    /* iterator() 内可能 safepoint：序列先进 shadow */
    Value seqRoot = seq;
    rootGcOperand(seqRoot);
    std::string iter = dispatch(seqRoot.ir, itf, im, "ptr");

    enterLoopSpillScope();
    std::string iterAddr = emitSpillGcRoot("for.it", iter);
    pinLoopSpillCheckpoint();

    std::string varAddr;
    if (isGcPointerType(elemType) && inLoopSpillPool()) {
        varAddr = acquireLoopGcSlot(varName + ".addr");
        pinLoopSpillCheckpoint();
    } else {
        varAddr = em_.nextNamed(varName + ".addr");
        emitAllocaAt(varAddr, elemType->llvmType());
        if (isGcPointerType(elemType)) {
            emitStore("ptr", "null", varAddr);
            emitGcRootPush(varAddr);
        }
    }

    auto savedHoist = loopHoisted_;
    hoistVarDeclsInLoopBody(st->statement());

    std::string condL = em_.nextLabel("for.cond");
    std::string bodyL = em_.nextLabel("for.body");
    std::string stepL = em_.nextLabel("for.step");
    std::string endL  = em_.nextLabel("for.end");
    emitBr(condL);

    // ---- 条件：先 load iter（sticky 根）再 hasNext，然后 safepoint ----
    em_.emitLabel(condL);
    blockTerminated_ = false;
    std::string itv = emitLoad("ptr", iterAddr);
    std::string hn = dispatch(itv, itfI, hmi, "i8");
    std::string cmp = emitICmp("ne", "i8", hn, "0");
    emitSafepoint();
    emitCondBr(cmp, bodyL, endL);

    // ---- 循环体：x = iter.next() ----
    em_.emitLabel(bodyL);
    blockTerminated_ = false;
    {
        SymbolTable::Guard g(syms_);

        std::string itv2 = emitLoad("ptr", iterAddr);
        std::string elemVal = dispatch(itv2, itfI, nmi, elemType->llvmType());

        emitStore(elemType->llvmType(), elemVal, varAddr);

        auto sym = std::make_shared<Symbol>();
        sym->kind = SymbolKind::Variable;
        sym->name = varName;
        sym->type = elemType;
        sym->isMutable = false;    // 循环变量不可赋值
        sym->irAddr = varAddr;
        syms_.declare(sym);
        int forLine = st->getStart() ? static_cast<int>(st->getStart()->getLine()) : 1;
        emitDbgDeclareIf(varAddr, varName, forLine, 0);
        /* D11：每轮迭代写回薄 dbg.value */
        emitDbgValueIf(elemType->llvmType(), elemVal, varName, forLine, 0);

        LoopContext flc;
        flc.breakLabel = endL;
        flc.continueLabel = stepL;
        flc.tryDepth = static_cast<int>(tryStack_.size());
        flc.clearSpillOnContinue = false;
        loops_.push_back(flc);
        genStatement(st->statement());
        loops_.pop_back();
    }
    if (!blockTerminated_) emitBr(stepL);

    // ---- 步进：回到条件（hasNext 每次重新判定）----
    em_.emitLabel(stepL);
    blockTerminated_ = false;
    recycleLoopSpillSlots();
    emitBr(condL);

    em_.emitLabel(endL);
    blockTerminated_ = false;
    if (isGcPointerType(elemType))
        emitStore("ptr", "null", varAddr);
    emitStore("ptr", "null", iterAddr);
    if (isGcPointerType(elemType))
        unpinLoopSpillCheckpoint();
    unpinLoopSpillCheckpoint();
    clearHoistedGcSince(savedHoist);
    loopHoisted_ = std::move(savedHoist);
    leaveLoopSpillScope();
}

// ============================================================
//  when 语句
// ------------------------------------------------------------
//      when (x) { 1, 2 -> A;  3 -> B;  else -> C }
//  降级为 if-else 链。带主体表达式时逐分支比较相等；
//  不带主体时各分支条件本身即为 Bool 表达式（类似 Kotlin）。
// ============================================================

void IRGen::genWhenStmt(HaoLangParser::WhenStmtContext* st) {
    bool hasSubject = st->expr() != nullptr;

    Value subject;
    std::string subjAddr;
    TypePtr subjType;

    if (hasSubject) {
        subject = genExpr(st->expr());
        if (!subject.valid()) return;
        if (subject.type->nullable) {
            error(st->expr(), "when 主体不能是可空类型 " + subject.type->toString() +
                              "，请先用 !! 或 ??");
            return;
        }
        subjType = subject.type;
        // 存入临时变量，避免每个分支重复求值；循环内走 spill 池
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

    std::string endL = em_.nextLabel("when.end");
    auto branches = st->whenBranch();

    // else 分支之后所有路径都已跳转到 end，
    // 用此标记避免在末尾重复发射 br（一个基本块只能有一个终结指令）
    bool allBranchesJumped = false;

    for (size_t bi = 0; bi < branches.size(); ++bi) {
        auto* br = branches[bi];

        // else 分支：无条件执行
        if (br->ELSE()) {
            if (auto* blk = br->block()) genBlock(blk);
            else if (auto* ex = br->expr()) genExpr(ex);
            if (!blockTerminated_) emitBr(endL);
            blockTerminated_ = true;      // 当前块已终结
            allBranchesJumped = true;
            continue;
        }

        std::string bodyL = em_.nextLabel("when.body");
        std::string nextL = em_.nextLabel("when.next");

        // 分支条件：多个值用"或"连接（1, 2 -> ...）
        auto* el = br->exprList();
        if (!el) continue;
        auto exprs = el->expr();

        for (size_t ei = 0; ei < exprs.size(); ++ei) {
            Value cv = genExpr(exprs[ei]);
            if (!cv.valid()) return;

            std::string condI1;
            if (hasSubject) {
                if (cv.type->nullable) {
                    error(exprs[ei], "when 分支值不能是可空类型 " +
                                     cv.type->toString() + "，请先用 !! 或 ??");
                    return;
                }
                // 与主体比较相等
                std::string sv = emitLoad(subjType->llvmType(), subjAddr);

                if (subjType->kind == TypeKind::String) {
                    if (cv.type->kind != TypeKind::String) {
                        error(exprs[ei], "when 分支值类型 " + cv.type->toString() +
                                         " 与主体类型 String 不匹配");
                        return;
                    }
                    std::string eqr = emitCall(
                        "i8", "@hao_str_eq",
                        "ptr " + sv + ", ptr " + cv.ir);
                    condI1 = emitICmp("ne", "i8", eqr, "0");
                } else if (subjType->isFloating() || cv.type->isFloating()) {
                    // Float/Double：须 fcmp（icmp float 非法 LLVM）
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
                        return;
                    }
                    // 跨宽度整数（Int vs Long）须提升后再 icmp，否则非法 IR
                    TypePtr ct = subjType;
                    if (subjType->isInteger() && cv.type->isInteger()) {
                        if (Type::isMixedSignedUnsigned64(subjType->kind,
                                                          cv.type->kind)) {
                            error(exprs[ei],
                                  "64 位有符号与无符号不能隐式混合，请显式转换");
                            return;
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
                // 无主体：分支条件本身是 Bool
                if (cv.type->kind != TypeKind::Bool) {
                    error(exprs[ei], "无主体的 when 要求分支条件为 Bool，实际为 " +
                                     cv.type->toString());
                    return;
                }
                if (!ensureNonNullOperand(cv, exprs[ei], "when 条件")) return;
                condI1 = toI1(cv);
            }

            // 命中即执行分支体；否则继续检查下一个值
            bool lastValue = (ei + 1 == exprs.size());
            std::string failL = lastValue ? nextL : em_.nextLabel("when.or");
            emitCondBr(condI1, bodyL, failL);
            if (!lastValue) {
                em_.emitLabel(failL);
                blockTerminated_ = false;
            }
        }

        // ---- 分支体 ----
        em_.emitLabel(bodyL);
        blockTerminated_ = false;
        beginBlockGcScope();
        if (auto* blk = br->block()) genBlock(blk);
        else if (auto* ex = br->expr()) genExpr(ex);
        endBlockGcScope();
        if (!blockTerminated_) emitBr(endL);

        em_.emitLabel(nextL);
        blockTerminated_ = false;
    }

    // 无 else 且所有分支都不匹配时，直接落到 end。
    // blockTerminated_ 为真说明当前块已有终结指令，不能再发射 br
    // （一个基本块只允许一个终结指令）。
    if (!blockTerminated_) emitBr(endL);

    em_.emitLabel(endL);
    blockTerminated_ = false;
    if (hasSubject && isGcPointerType(subjType) && !subjAddr.empty())
        emitStore("ptr", "null", subjAddr);
    (void)allBranchesJumped;
}

} // namespace hao
