// ============================================================
//  HaoLang IR 生成 —— return / 异常（try-catch-finally / throw）
// ------------------------------------------------------------
//  从 IRGen.cpp 拆分而来，逻辑保持不变。
//
//  异常模型：运行时用 setjmp/longjmp 维护异常帧链（见 stdlib/
//  runtime_exception.c）。IR 层面为每个 try 生成 setjmp 分派 +
//  异常类型匹配 + cleanup（弹帧 + finally + 按 reason 分派）。
//
//  return / break / continue / throw 可能穿过 finally，因此不直接
//  发射终结指令，而是把「原因 + 待返回值/异常」写入函数级 alloca 槽
//  （unwindReasonAddr_ / unwindRetAddr_ / unwindStopAddr_），跳到
//  最近的 cleanup，由 cleanup 链逐层执行 finally 后在最外层完成真正的
//  ret / br / rethrow。这套机制的完整设计见项目记忆文档第六节第 7 条。
// ============================================================

#include "irgen/IRGen.h"

namespace hao {

// ------------------------------------------------------------
//  return
// ------------------------------------------------------------
void IRGen::genReturn(HaoLangParser::ReturnStmtContext* st) {
    sawReturn_ = true;

    if (!st->expr()) {
        if (!currentReturn_->isUnit()) {
            error(st, "函数声明返回 " + currentReturn_->toString() + "，但 return 没有返回值");
            return;
        }
        // 在 try 内：先执行 finally 再返回
        if (!tryStack_.empty()) { emitUnwind(1); return; }
        emitGcRootUnwind();
        em_.emit(inMain_ ? "ret i32 0" : "ret void");
        blockTerminated_ = true;
        return;
    }

    // 压入函数声明的返回类型，供 return 的 lambda 推断参数/返回类型
    if (!currentReturn_->isUnit()) expectedTypes_.push_back(currentReturn_);
    Value v = genExpr(st->expr());
    if (!currentReturn_->isUnit()) expectedTypes_.pop_back();
    if (!v.valid()) return;

    if (currentReturn_->isUnit()) {
        error(st, "函数无返回值声明，但 return 返回了 " + v.type->toString());
        return;
    }
    if (!isAssignable(v.type, currentReturn_)) {
        error(st, "返回值类型 " + v.type->toString() +
                  " 与函数声明的 " + currentReturn_->toString() + " 不匹配");
        return;
    }

    v = coerce(v, currentReturn_,
               st->getStart()->getLine(), st->getStart()->getCharPositionInLine());

    // 在 try 内：保存返回值，串过 finally 后再真正 ret
    if (!tryStack_.empty()) {
        storeUnwindRet(v);
        emitUnwind(1);
        return;
    }

    emitGcRootUnwind();
    if (inMain_) {
        // main 的 C 入口返回 int(i32)。v0.25+ Int 已是 i32，直接 ret，勿再 trunc i64。
        if (currentReturn_->kind == TypeKind::Int) {
            em_.emit("ret i32 " + v.ir);
        } else {
            em_.emit("ret i32 0");
        }
        blockTerminated_ = true;
        return;
    }
    em_.emit("ret " + currentReturn_->llvmType() + " " + v.ir);
    blockTerminated_ = true;
}

// ------------------------------------------------------------
//  unwind 槽读写
// ------------------------------------------------------------

// 把返回值/异常对象按类型转成 i64 存入 unwind 槽；
// GC 指针同时写入 unwindGcRootAddr_，保证 finally/safepoint 期间可达。
void IRGen::storeUnwindRet(const Value& v) {
    if (!v.valid() || v.type->isUnit()) {
        em_.emit("store i64 0, ptr " + unwindRetAddr_);
        clearUnwindGcRoot();
        return;
    }
    if (isGcPointerType(v.type))
        storeUnwindGcRootPtr(v.ir);
    else
        clearUnwindGcRoot();
    std::string val64 = em_.boxToI64(v.ir, v.type);
    em_.emit("store i64 " + val64 + ", ptr " + unwindRetAddr_);
}

// 从 unwind 槽读回返回值并发射 ret（函数尾声用）
void IRGen::emitUnwindRet() {
    std::string raw = em_.nextTemp();
    em_.emit(raw + " = load i64, ptr " + unwindRetAddr_);
    emitGcRootUnwind();
    if (currentReturn_->isUnit()) {
        em_.emit(inMain_ ? "ret i32 0" : "ret void");
        return;
    }
    std::string r = em_.unboxFromI64(raw, currentReturn_);
    if (inMain_) {
        // main 的 C 入口返回 int(i32)；Int 已是 i32。
        if (currentReturn_->kind == TypeKind::Int) {
            em_.emit("ret i32 " + r);
        } else {
            em_.emit("ret i32 0");
        }
        return;
    }
    em_.emit("ret " + currentReturn_->llvmType() + " " + r);
}

// 从 try/catch 体离开：写入 reason，跳到最近的 cleanup（若有），
// 否则（无 try 包裹）执行真正的 return/break/continue/throw。
//   reason: 0=正常 1=return 2=break 3=continue 4=rethrow
void IRGen::emitUnwind(int reason, const Value& retVal) {
    if (reason == 1) {
        if (retVal.valid()) storeUnwindRet(retVal);
        em_.emit("store i32 1, ptr " + unwindReasonAddr_);
    } else if (reason == 2 || reason == 3) {
        em_.emit("store i32 " + std::to_string(reason) + ", ptr " + unwindReasonAddr_);
        if (loops_.empty()) {
            diags_.error(0, 0, reason == 2 ? "break 只能出现在循环内部"
                                             : "continue 只能出现在循环内部");
            return;
        }
        const auto& loop = loops_.back();
        int loopDepth = static_cast<int>(loop.tryDepth);
        int curDepth = static_cast<int>(tryStack_.size());
        if (curDepth <= loopDepth) {
            // 没有需要穿过的 finally，直接跳到循环目标
            em_.emit("br label %" + (reason == 2 ? loop.breakLabel : loop.continueLabel));
            blockTerminated_ = true;
            return;
        }
        // 记录停止深度：cleanup 链执行到该深度时，按 reason 跳到循环目标
        em_.emit("store i32 " + std::to_string(loopDepth) + ", ptr " + unwindStopAddr_);
    } else if (reason == 4) {
        // catch 体内 throw：存异常对象，走 cleanup 链后重抛
        em_.emit("store i32 4, ptr " + unwindReasonAddr_);
        if (retVal.valid()) storeUnwindRet(retVal);
    }

    if (tryStack_.empty()) {
        if (reason == 1) { emitUnwindRet(); blockTerminated_ = true; }
        else if (reason == 4) {
            std::string raw = em_.nextTemp();
            em_.emit(raw + " = load i64, ptr " + unwindRetAddr_);
            std::string p = em_.nextTemp();
            em_.emit(p + " = inttoptr i64 " + raw + " to ptr");
            em_.emit("call void @hao_rethrow(ptr " + p + ")");
            em_.emit("unreachable");
            blockTerminated_ = true;
        }
        return;
    }
    em_.emit("br label %" + tryStack_.back().cleanupLabel);
    blockTerminated_ = true;
}

// ------------------------------------------------------------
//  try / catch / finally
// ------------------------------------------------------------
//  控制流结构（以一个 try 为例，深度 d）：
//
//    %r = call i32 @hao_try_begin()        ; 压运行时帧 + setjmp
//    switch i32 %r, label %try.body [i32 1, label %try.dispatch]
//  try.body:
//    <try 体>
//    br label %try.cleanup                 ; 正常落入 cleanup
//  try.dispatch:                          ; longjmp 到达
//    %ex = call ptr @hao_except_capture()  ; 弹运行时帧
//    <逐一 hao_type_is 匹配 catch；匹配则 br catchN>
//    <都不匹配: br label %try.cleanup（reason=4 已存 ex）>
//  catchN:
//    <catch 体>
//    br label %try.cleanup                 ; catch 正常结束，reason=0
//  try.cleanup:                           ; 所有路径汇聚
//    call void @hao_try_end()              ; 正常路径弹帧（dispatch 已弹，重复调用安全）
//    <finally 体>
//    %rsn = load i32 ...
//    switch i32 %rsn, label %try.end [
//       1 -> label %outer.cleanup / epilogue
//       2/3 -> (若 d==stopDepth) indirectbr 目标; else outer.cleanup
//       4 -> call @hao_rethrow; unreachable
//    ]
//  try.end:                               ; 正常继续
//
void IRGen::genTry(HaoLangParser::TryStmtContext* st) {
    auto catches = st->catchClause();
    auto* fin = st->finallyClause();

    // 用函数内唯一编号，避免同一深度的多个 try 标签重名
    std::string idx = std::to_string(++tryCounter_);
    int depth = static_cast<int>(tryStack_.size()) + 1;
    std::string bodyL     = "try" + idx + ".body";
    std::string dispatchL = "try" + idx + ".dispatch";
    std::string cleanupL  = "try" + idx + ".cleanup";
    std::string endL      = "try" + idx + ".end";

    TryContext tc;
    tc.cleanupLabel = cleanupL;
    tc.endLabel = endL;
    tryStack_.push_back(tc);

    // catch 绑定槽：循环内走 spill 池；否则 setjmp 前 alloca+push 一次
    struct CatchEntry {
        HaoLangParser::CatchClauseContext* node;
        std::string label;
        TypePtr type;
        std::string bindAddr;
    };
    std::vector<CatchEntry> catchEntries;
    for (size_t i = 0; i < catches.size(); ++i) {
        auto* cc = catches[i];
        TypePtr t = resolveType(cc->type());
        std::string bindHint =
            "try" + idx + ".c" + std::to_string(i) + ".addr";
        std::string bindAddr;
        if (inLoopSpillPool()) {
            bindAddr = acquireLoopGcSlot(bindHint);
            em_.emit("store ptr null, ptr " + bindAddr);
        } else {
            bindAddr = em_.nextNamed(bindHint);
            em_.emit(bindAddr + " = alloca ptr");
            em_.emit("store ptr null, ptr " + bindAddr);
            emitGcRootPush(bindAddr);
        }
        catchEntries.push_back(
            {cc, "try" + idx + ".catch" + std::to_string(i), t, bindAddr});
    }

    // ---- setjmp 分派 ----
    //  关键：setjmp 必须在用户函数自身的栈帧里调用，不能包在会返回的运行时
    //  辅助函数中（否则 longjmp 跳回一个已失效的帧会崩溃）。因此分两步：
    //    1) hao_try_alloc 在静态运行时栈上分配一帧，返回 jmp_buf 指针；
    //    2) 在 IR 里直接 call @setjmp(buf)，用 returns_twice 标记。
    std::string idAddr = em_.nextNamed("try" + idx + ".id.addr");
    em_.emit(idAddr + " = alloca i32");
    std::string buf = em_.nextTemp();
    em_.emit(buf + " = call ptr @hao_try_alloc(ptr " + idAddr + ")");

    // Windows x64 的 setjmp 是 _setjmp(buf, frame_pointer) 双参数，
    // 必须传入当前帧指针（@llvm.frameaddress），longjmp 才能正确展开；
    // Linux/musl 的 setjmp 只需 buf 一个参数。
    bool isWin = em_.targetTriple().find("windows") != std::string::npos ||
                 em_.targetTriple().find("msvc") != std::string::npos;
    std::string r;
    if (isWin) {
        std::string fp = em_.nextTemp();
        em_.emit(fp + " = call ptr @llvm.frameaddress.p0(i32 0)");
        r = em_.nextTemp();
        em_.emit(r + " = call i32 @_setjmp(ptr " + buf + ", ptr " + fp + ") #0");
    } else {
        r = em_.nextTemp();
        em_.emit(r + " = call i32 @setjmp(ptr " + buf + ") #0");
    }
    std::string sw = "switch i32 " + r + ", label %" + bodyL +
                     " [ i32 1, label %" + dispatchL + " ]";
    em_.emit(sw);

    // ---- try 体 ----
    em_.emitLabel(bodyL);
    {
        SymbolTable::Guard g(syms_);
        beginBlockGcScope();
        for (auto* s : st->block()->statement()) genStatement(s);
        endBlockGcScope();
    }
    bool bodyFallsThrough = !blockTerminated_;
    if (bodyFallsThrough) em_.emit("br label %" + cleanupL);
    bool tryTerminated = blockTerminated_;
    blockTerminated_ = false;
    bool anyCatchFallsThrough = false;

    // ---- 异常分派 ----
    em_.emitLabel(dispatchL);
    std::string exc = em_.nextTemp();
    em_.emit(exc + " = call ptr @hao_except_capture()");
    // 立刻写入函数级 unwind GC 根：dispatch 匹配 / finally 期间异常对象须可达
    storeUnwindGcRootPtr(exc);

    // 全都没匹配上的块：把异常存入 unwind 槽、reason=4，然后进 cleanup（重抛）
    std::string noMatchL = "try" + idx + ".nomatch";

    // 没有 catch：dispatch 直接落入 noMatch（标记重抛）
    if (catchEntries.empty()) {
        em_.emit("br label %" + noMatchL);
    }

    // 逐一类型匹配（复用 is 的 typeids 机制）
    for (size_t i = 0; i < catchEntries.size(); ++i) {
        const auto& ce = catchEntries[i];
        std::string nextL = (i + 1 < catchEntries.size())
            ? catchEntries[i + 1].label
            : noMatchL;

        // 异常对象必须是类/接口类型才能匹配
        if (ce.type->kind != TypeKind::Class && ce.type->kind != TypeKind::Interface) {
            error(ce.node, "catch 类型必须是类或接口，实际为 " + ce.type->toString());
            tryStack_.pop_back();
            return;
        }
        auto it = typeIdLists_.find(ce.type->className);
        if (it == typeIdLists_.end()) {
            // 没有任何可实例化类型满足，恒不匹配
            em_.emit("br label %" + nextL);
        } else {
            std::string chk = em_.nextTemp();
            em_.emit(chk + " = call i8 @hao_type_is(ptr " + exc +
                     ", ptr " + it->second + ")");
            std::string cond = em_.nextTemp();
            em_.emit(cond + " = icmp ne i8 " + chk + ", 0");
            em_.emit("br i1 " + cond + ", label %" + ce.label + ", label %" + nextL);
        }

        // catch 体
        em_.emitLabel(ce.label);
        catchDepth_++;
        {
            // catch 成功处理异常：复位 reason（内层可能把它设为 4=rethrow），
            // 否则外层 cleanup 会把已处理的异常重新抛出。
            em_.emit("store i32 0, ptr " + unwindReasonAddr_);
            em_.emit("store ptr " + exc + ", ptr " + ce.bindAddr);
            SymbolTable::Guard g(syms_);
            auto sym = std::make_shared<Symbol>();
            sym->kind = SymbolKind::Variable;
            sym->name = ce.node->IDENT()->getText();
            sym->type = ce.type;
            sym->isMutable = false;
            sym->irAddr = ce.bindAddr;
            syms_.declare(sym);

            beginBlockGcScope();
            for (auto* s : ce.node->block()->statement()) genStatement(s);
            endBlockGcScope();
        }
        catchDepth_--;
        if (!blockTerminated_) {
            anyCatchFallsThrough = true;
            em_.emit("br label %" + cleanupL);
        }
        blockTerminated_ = false;
    }

    // 没有 catch 或全都不匹配：标记重抛后进入 cleanup
    em_.emitLabel(noMatchL);
    {
        std::string ei = em_.nextTemp();
        em_.emit(ei + " = ptrtoint ptr " + exc + " to i64");
        em_.emit("store i64 " + ei + ", ptr " + unwindRetAddr_);
        storeUnwindGcRootPtr(exc);
        em_.emit("store i32 4, ptr " + unwindReasonAddr_);
    }
    em_.emit("br label %" + cleanupL);

    // ---- cleanup：弹帧 + finally + 分派 ----
    em_.emitLabel(cleanupL);
    {
        std::string id = em_.nextTemp();
        em_.emit(id + " = load i32, ptr " + idAddr);
        em_.emit("call void @hao_try_end(i32 " + id + ")");
    }

    // 生成 finally 前先把本 try 从 IR 栈弹出：这样 finally 内的
    // return/throw 会向外层 cleanup 传播，而不是跳回本 cleanup 造成死循环。
    // （运行时帧已由上面的 hao_try_end / except_capture 处理。）
    tryStack_.pop_back();

    bool finTerminated = false;
    if (fin) {
        SymbolTable::Guard g(syms_);
        for (auto* s : fin->block()->statement()) genStatement(s);
        finTerminated = blockTerminated_;
        blockTerminated_ = false;
    }

    if (finTerminated) {
        // finally 自身 return/throw 终结了控制流，不再分派
        return;
    }

    std::string rsn = em_.nextTemp();
    em_.emit(rsn + " = load i32, ptr " + unwindReasonAddr_);

    std::string retL    = "try" + idx + ".onreturn";
    std::string loopL   = "try" + idx + ".onloop";
    std::string throwL  = "try" + idx + ".onrethrow";
    em_.emit("switch i32 " + rsn + ", label %" + endL +
             " [ i32 1, label %" + retL +
             " i32 2, label %" + loopL +
             " i32 3, label %" + loopL +
             " i32 4, label %" + throwL + " ]");

    // reason=1：向外层清理（或尾声）传递
    em_.emitLabel(retL);
    if (!tryStack_.empty()) {
        em_.emit("br label %" + tryStack_.back().cleanupLabel);
    } else {
        emitUnwindRet();
    }

    // reason=2/3：若已到达停止深度（循环外的 try 层数），跳到循环目标；
    // 否则继续向外层 cleanup。此时仍在循环作用域内，loops_.back() 有效。
    em_.emitLabel(loopL);
    {
        std::string stop = em_.nextTemp();
        em_.emit(stop + " = load i32, ptr " + unwindStopAddr_);
        std::string here = std::to_string(depth - 1);  // 当前 try 之外的深度
        std::string isStop = em_.nextTemp();
        em_.emit(isStop + " = icmp eq i32 " + stop + ", " + here);
        std::string doBr = "try" + idx + ".doloop";
        if (!tryStack_.empty()) {
            em_.emit("br i1 " + isStop + ", label %" + doBr +
                     ", label %" + tryStack_.back().cleanupLabel);
        } else {
            em_.emit("br i1 " + isStop + ", label %" + doBr + ", label %" + endL);
        }
        em_.emitLabel(doBr);
        if (loops_.empty()) {
            em_.emit("br label %" + endL);
        } else {
            // 按 reason（2=break / 3=continue）选目标
            std::string which = em_.nextTemp();
            em_.emit(which + " = load i32, ptr " + unwindReasonAddr_);
            std::string isBreak = em_.nextTemp();
            em_.emit(isBreak + " = icmp eq i32 " + which + ", 2");
            em_.emit("br i1 " + isBreak + ", label %" + loops_.back().breakLabel +
                     ", label %" + loops_.back().continueLabel);
        }
    }

    // reason=4：重抛（longjmp 到外层运行时帧）
    em_.emitLabel(throwL);
    {
        std::string raw = em_.nextTemp();
        em_.emit(raw + " = load i64, ptr " + unwindRetAddr_);
        std::string p = em_.nextTemp();
        em_.emit(p + " = inttoptr i64 " + raw + " to ptr");
        em_.emit("call void @hao_rethrow(ptr " + p + ")");
        em_.emit("unreachable");
    }

    // ---- 正常继续 ----
    em_.emitLabel(endL);
    // 正常退出后复位 reason（避免上次的残留影响后续——虽然正常路径 reason 应为 0）
    em_.emit("store i32 0, ptr " + unwindReasonAddr_);
    /* 放弃 catch 绑定与 unwind GC 根，避免 while{try/catch} 假活 */
    for (const auto& ce : catchEntries)
        em_.emit("store ptr null, ptr " + ce.bindAddr);
    clearUnwindGcRoot();

    // 若 try 体与所有 catch 都不向下落入 cleanup（都 return/throw 了），
    // 则 endL 实际不可达，但仍需一条终结指令；整个 try 不会正常继续。
    bool tryFallsThrough = bodyFallsThrough || anyCatchFallsThrough;
    if (!tryFallsThrough) {
        em_.emit("unreachable");
        blockTerminated_ = true;
    } else {
        blockTerminated_ = false;
    }

    (void)tryTerminated;
}

// ------------------------------------------------------------
//  throw
// ------------------------------------------------------------
void IRGen::genThrow(HaoLangParser::ThrowStmtContext* st) {
    Value v = genExpr(st->expr());
    if (!v.valid()) return;
    if (v.type->kind != TypeKind::Class && v.type->kind != TypeKind::Interface) {
        error(st, "throw 只能抛出类对象，实际为 " + v.type->toString());
        return;
    }
    if (v.type->nullable) {
        error(st, "throw 不能抛出可空类型 " + v.type->toString() +
                  "，请先用 !! 或 ??");
        return;
    }

    // 在 catch 体内：运行时帧已被 except_capture 弹出，不能再 longjmp 回本帧，
    // 用 IR 展开（reason=4）穿过外层 finally 后由最外层 hao_rethrow 抛出。
    // 其余情况（try 体、finally、无 try）都直接 longjmp：运行时帧仍在栈上，
    // 会正确跳到本 try 的 dispatch，进而执行 finally。
    if (catchDepth_ > 0) {
        storeUnwindRet(v);
        emitUnwind(4);
        return;
    }

    em_.emit("call void @hao_throw(ptr " + v.ir + ")");
    em_.emit("unreachable");
    blockTerminated_ = true;
}

} // namespace hao
