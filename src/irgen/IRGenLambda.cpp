// ============================================================
//  HaoLang Lambda / 闭包实现
// ------------------------------------------------------------
//  闭包的运行时表示：一个由 GC 管理的堆对象（env），布局为
//
//        [ 0] fnptr      实现函数指针（签名 ret(env, args...)）
//        [ 1] cap0       第 0 个捕获
//        [ 2] cap1       ...
//
//  所有 lambda 实现函数的第一个参数都是 ptr %env.arg。函数值本身
//  就是 env 指针；调用时从槽 0 取出 fnptr，以 (env, 实参...) 发起
//  间接 call。无捕获 lambda、有捕获 lambda、顶层函数包装器三者
//  的调用约定完全一致，调用点无需区分。
//
//  可变捕获（外层 var / 参数）按引用共享：外层变量被「装箱」到一个
//  8 字节堆 cell，env 槽里存的是 cell 指针；外层与所有捕获它的
//  lambda 都通过同一个 cell 读写。不可变 val / this 按值捕获。
// ============================================================

#include "irgen/IRGen.h"

#include <algorithm>
#include <functional>

namespace hao {

// 判断名字是否是全局函数/类（不需要捕获）
static bool isGlobalName(SymbolTable& syms, const std::string& name) {
    auto s = syms.global()->lookup(name);
    if (!s) return false;
    return s->kind == SymbolKind::Function || s->kind == SymbolKind::Class;
}

// 可通过当前作用域 / 包前缀 / 通配导入解析到的类或顶层函数（如 lang.Integer）
// —— 与 genPrimary 的 resolveTopLevelName 对齐，勿当闭包自由变量。
bool IRGen::isResolvableTypeOrFuncName(const std::string& name) {
    if (isGlobalName(syms_, name)) return true;
    auto sym = resolveTopLevelName(name, /*ctx=*/nullptr);
    if (!sym) return false;
    return sym->kind == SymbolKind::Class || sym->kind == SymbolKind::Function;
}

// ------------------------------------------------------------
//  自由变量收集（scope-aware）
// ------------------------------------------------------------
//  返回子树中未被 declared 及遍历途中声明绑定的标识符。
//  - declared 跨「顺序语句」累积（后语句可见前语句的 var 声明）；
//  - 进入嵌套作用域（block/if/while/for 体/catch 块）时复制一份，
//    使内层声明不泄漏到外层；
//  - 遇到嵌套 lambda 不深入（它由自己的 analyzeLambdas 处理）。
// ------------------------------------------------------------
std::set<std::string> IRGen::collectFreeNames(antlr4::tree::ParseTree* node,
                                              std::set<std::string>& declared) {
    std::set<std::string> free;
    auto add = [&](const std::set<std::string>& f) {
        free.insert(f.begin(), f.end());
    };

    if (!node) return free;

    // ---- 叶子：标识符引用 ----
    if (auto* id = dynamic_cast<HaoLangParser::IdentPrimaryContext*>(node)) {
        std::string name = id->IDENT()->getText();
        // 包名（import 别名 / 隐式预导入的 fmt / 全局函数类）不是自由变量，
        // 不需要捕获。否则 lambda 里 `fmt.println(...)` 会把 fmt 误当捕获变量。
        bool isPkg = (name == "fmt" || name == "lang" || name == "object");
        for (const auto& im : currentImports_)
            if (im.alias == name || im.importPath == name) { isPkg = true; break; }
        // 类名静态接收者（Integer.toStr / String.valueOf）与 Java/C# 一致：不捕获。
        // 旧逻辑只查 global() 裸名，漏掉 lang.* 通配导出的包装类。
        if (!declared.count(name) && !isPkg && !isResolvableTypeOrFuncName(name))
            free.insert(name);
        return free;
    }
    // this 引用：在类上下文中作为特殊捕获 "$this"
    if (dynamic_cast<HaoLangParser::ThisPrimaryContext*>(node)) {
        if (currentClass_) free.insert("$this");
        return free;
    }
    // 嵌套 lambda：不深入
    if (dynamic_cast<HaoLangParser::LambdaContext*>(node)) return free;

    // ---- var 声明：初始化式可引用先前声明，但不能引用自身 ----
    if (auto* vd = dynamic_cast<HaoLangParser::VarDeclContext*>(node)) {
        if (vd->expr()) add(collectFreeNames(vd->expr(), declared));
        declared.insert(vd->IDENT()->getText());
        return free;
    }

    // ---- 嵌套块：复制作用域 ----
    if (auto* blk = dynamic_cast<HaoLangParser::BlockContext*>(node)) {
        std::set<std::string> inner = declared;
        for (auto* st : blk->statement()) add(collectFreeNames(st, inner));
        return free;
    }

    // ---- if：条件在外层作用域，两个分支各自复制 ----
    if (auto* ifs = dynamic_cast<HaoLangParser::IfStmtContext*>(node)) {
        if (ifs->expr()) add(collectFreeNames(ifs->expr(), declared));
        std::set<std::string> t = declared;
        add(collectFreeNames(ifs->statement(0), t));
        if (ifs->statement().size() > 1) {
            std::set<std::string> e = declared;
            add(collectFreeNames(ifs->statement(1), e));
        }
        return free;
    }

    // ---- while：条件在外层，循环体复制 ----
    if (auto* ws = dynamic_cast<HaoLangParser::WhileStmtContext*>(node)) {
        if (ws->expr()) add(collectFreeNames(ws->expr(), declared));
        std::set<std::string> b = declared;
        add(collectFreeNames(ws->statement(), b));
        return free;
    }

    // ---- for：序列表达式在外层，循环变量只在循环体作用域内 ----
    if (auto* fs = dynamic_cast<HaoLangParser::ForStmtContext*>(node)) {
        if (fs->expr()) add(collectFreeNames(fs->expr(), declared));
        std::set<std::string> b = declared;
        b.insert(fs->IDENT()->getText());
        add(collectFreeNames(fs->statement(), b));
        return free;
    }

    // ---- catch：异常变量只在 catch 块内绑定 ----
    if (auto* cc = dynamic_cast<HaoLangParser::CatchClauseContext*>(node)) {
        std::set<std::string> b = declared;
        b.insert(cc->IDENT()->getText());
        if (cc->block()) add(collectFreeNames(cc->block(), b));
        return free;
    }

    // ---- 通用递归 ----
    for (auto* child : node->children) add(collectFreeNames(child, declared));
    return free;
}

// ------------------------------------------------------------
//  分析函数体内所有「直接内嵌」的 lambda
// ------------------------------------------------------------
void IRGen::analyzeLambdas(antlr4::tree::ParseTree* body) {
    if (!body) return;

    // 收集本层（不跨越嵌套 lambda）的所有 lambda 节点
    std::vector<HaoLangParser::LambdaContext*> lams;
    std::function<void(antlr4::tree::ParseTree*)> walk =
        [&](antlr4::tree::ParseTree* n) {
            if (!n) return;
            if (auto* lam = dynamic_cast<HaoLangParser::LambdaContext*>(n)) {
                // 字段默认值等共享 AST：已分析过则跳过，避免重复 @lambda$N
                bool exists = false;
                for (const auto& li : lambdas_)
                    if (li.ctx == lam) { exists = true; break; }
                if (!exists) lams.push_back(lam);
                return;
            }
            for (auto* c : n->children) walk(c);
        };
    walk(body);

    for (auto* lam : lams) {
        LambdaInfo li;
        li.ctx = lam;
        li.implName = "@lambda$" + std::to_string(++lambdaCounter_);

        // declared 只放 lambda「自己的形参」。外层函数/外层 lambda 的形参
        // 与局部变量不在其中，因此会被识别为需捕获的自由变量。
        std::set<std::string> declared;
        bool noParamList = (lam->lambdaParams() == nullptr);
        if (auto* lp = lam->lambdaParams())
            for (auto* id : lp->IDENT()) {
                li.params.push_back(id->getText());
                declared.insert(id->getText());
            }

        // 收集函数体内自由变量
        std::set<std::string> capSet;
        for (auto* st : lam->statement()) {
            auto f = collectFreeNames(st, declared);
            capSet.insert(f.begin(), f.end());
        }

        // 无参数列表且体里引用了 it => 隐式单参数 it（类型由期望类型推断）
        if (noParamList && capSet.count("it")) {
            capSet.erase("it");
            li.params.insert(li.params.begin(), "it");
            declared.insert("it");
        }

        if (capSet.count("$this")) {
            li.capturesThis = true;
            li.thisClassName = currentClass_ ? currentClass_->name : "";
            capSet.erase("$this");
        }
        for (const auto& n : capSet) {
            LambdaCapture cap;
            cap.name = n;
            li.captures.push_back(cap);
            capturedVarNames_.insert(n);   // 供外层决定是否装箱
        }

        lambdas_.push_back(std::move(li));
    }
}

// ------------------------------------------------------------
//  纯 AST 推断 lambda 类型（供 inferNodeType 在不发射代码时使用）
// ------------------------------------------------------------
TypePtr IRGen::inferLambdaType(HaoLangParser::LambdaContext* lam) {
    std::vector<std::string> pnames;
    std::vector<TypePtr> ptypes;
    if (auto* lp = lam->lambdaParams()) {
        for (size_t i = 0; i < lp->IDENT().size(); ++i) {
            pnames.push_back(lp->IDENT(i)->getText());
            if (i < lp->type().size() && lp->type(i))
                ptypes.push_back(resolveType(lp->type(i)));
            else
                ptypes.push_back(Type::makeUnknown());
        }
    }

    // 返回类型：return 语句优先，否则取末尾表达式语句
    TypePtr ret;
    auto stmts = lam->statement();
    for (auto* st : stmts)
        if (auto* rs = st->returnStmt())
            if (rs->expr()) { ret = inferExprType(rs->expr()); break; }
    if (!ret && !stmts.empty())
        if (auto* es = stmts.back()->exprStmt())
            ret = inferExprType(es->expr());
    if (!ret || ret->isUnknown()) ret = Type::makeUnit();

    return Type::makeFunc(std::move(ptypes), ret);
}

// ------------------------------------------------------------
//  生成一个 lambda 表达式（值 = env 指针）
// ------------------------------------------------------------
Value IRGen::genLambda(HaoLangParser::LambdaContext* lam) {
    const LambdaInfo* info = nullptr;
    for (auto& li : lambdas_)
        if (li.ctx == lam) { info = &li; break; }
    if (!info) {
        error(lam, "内部错误：lambda 未经过自由变量分析");
        return Value();
    }

    // 期望的函数类型（来自 var 标注或实参形参），用于推断
    TypePtr expected = peekExpectedType();
    std::vector<TypePtr> expParams;
    TypePtr expRet;
    if (expected && expected->kind == TypeKind::Func) {
        expParams = expected->params;
        expRet = expected->elem;
    }

    LambdaInfo mi = *info;
    mi.paramTypes.clear();

    // ---- 1. 参数类型 ----
    //  mi.params 已由 analyzeLambdas 填好：有参数列表时是显式形参名；
    //  无参数列表但体里用到 it 时为 ["it"]；否则为空。
    auto* lp = lam->lambdaParams();
    if (lp) {
        for (size_t i = 0; i < lp->IDENT().size(); ++i) {
            std::string pn = lp->IDENT(i)->getText();
            TypePtr pt;
            if (i < lp->type().size() && lp->type(i))
                pt = resolveType(lp->type(i));
            else if (i < expParams.size())
                pt = expParams[i];
            if (!pt || pt->isUnknown()) {
                error(lam, "无法推断 lambda 参数 '" + pn + "' 的类型，"
                           "请显式标注或通过函数类型上下文推断");
                return Value();
            }
            mi.paramTypes.push_back(pt);
        }
    } else if (!mi.params.empty()) {
        // 隐式 it（体里引用了 it）：必须有恰好一个期望形参来确定其类型
        if (expParams.size() != 1) {
            error(lam, "无法推断隐式参数 'it' 的类型：lambda 需要单参数的函数类型上下文");
            return Value();
        }
        mi.paramTypes.push_back(expParams[0]);
    } else if (expParams.size() == 1) {
        // 无参列表、体里也没引用 it，但期望类型是单参数：补一个忽略的 it
        // （如 val f: (Int)->Int = { 5 }）
        mi.params.push_back("it");
        mi.paramTypes.push_back(expParams[0]);
    }
    // 否则为零参数 lambda

    // ---- 2. 返回类型 ----
    TypePtr retType = expRet;
    if (!retType) {
        auto stmts = lam->statement();

        // 收集 return 语句类型
        std::vector<TypePtr> rets;
        std::function<void(antlr4::tree::ParseTree*)> scan =
            [&](antlr4::tree::ParseTree* n) {
                if (!n) return;
                if (dynamic_cast<HaoLangParser::LambdaContext*>(n)) return;
                if (auto* r = dynamic_cast<HaoLangParser::ReturnStmtContext*>(n)) {
                    if (r->expr()) rets.push_back(inferExprType(r->expr()));
                    return;
                }
                for (auto* c : n->children) scan(c);
            };
        for (auto* st : stmts) scan(st);

        // 末尾表达式语句类型（隐式返回）
        TypePtr trailing;
        if (!stmts.empty())
            if (auto* es = stmts.back()->exprStmt())
                trailing = inferExprType(es->expr());

        if (!rets.empty()) {
            retType = rets[0];
            for (size_t i = 1; i < rets.size(); ++i)
                if (!rets[i]->sameShape(*retType)) retType = nullptr;
        } else if (trailing && !trailing->isUnknown()) {
            retType = trailing;
        } else {
            retType = Type::makeUnit();
        }
        if (!retType) {
            error(lam, "无法推断 lambda 的返回类型（多个 return 类型不一致），"
                       "请把它赋给带函数类型标注的变量以提供上下文");
            return Value();
        }
    }
    mi.returnType = retType;

    // ---- 3. 解析捕获，查外层符号 ----
    for (auto& cap : mi.captures) {
        auto sym = syms_.lookup(cap.name);
        if (!sym || sym->kind != SymbolKind::Variable) {
            error(lam, "lambda 捕获了未定义的变量 '" + cap.name + "'");
            return Value();
        }
        cap.type = sym->type;
        cap.boxed = sym->boxed;
    }

    // ---- 4. 生成实现函数（同 AST 多次求值时只 emit 一次）----
    if (emittedLambdaImpls_.insert(mi.implName).second)
        genLambdaImpl(mi);

    // ---- 5. 构造 env 闭包对象 ----
    size_t nslots = 1 + (mi.capturesThis ? 1 : 0) + mi.captures.size();
    int64_t envBm = 0;
    size_t slotBm = 1;
    if (mi.capturesThis) {
        envBm |= (int64_t(1) << slotBm);
        ++slotBm;
    }
    for (auto& cap : mi.captures) {
        if (cap.boxed || isGcPointerType(cap.type))
            envBm |= (int64_t(1) << slotBm);
        ++slotBm;
    }
    std::string env = emitObjectNew((int64_t)nslots, envBm);
    std::string envSlot = emitSpillGcRoot("lambda.env", env);
    env = emitLoad("ptr", envSlot);
    std::string fp0 = fieldPtr(env, 0);
    emitStore("ptr", mi.implName, fp0);

    size_t slot = 1;
    if (mi.capturesThis) {
        std::string sp = fieldPtr(env, (int)slot);
        std::string treg = emitLoad("ptr", thisAddr_);
        emitHeapStore(sp, treg, Type::makeClass("Object"), env);
        ++slot;
    }
    for (auto& cap : mi.captures) {
        auto sym = syms_.lookup(cap.name);
        std::string sp = fieldPtr(env, (int)slot);
        if (cap.boxed) {
            std::string cell = emitLoad("ptr", sym->irAddr);
            // cell 本身是堆对象指针
            TypePtr cellTy = Type::makeClass("Object");
            emitHeapStore(sp, cell, cellTy, env);
        } else {
            Value v = loadVar(sym);
            emitHeapStore(sp, v.ir, cap.type, env);
        }
        ++slot;
    }

    return Value(env, Type::makeFunc(mi.paramTypes, mi.returnType));
}

// ------------------------------------------------------------
//  生成 lambda 实现函数的完整 define（写入独立缓冲后登记）
// ------------------------------------------------------------
void IRGen::genLambdaImpl(const LambdaInfo& mi) {
    em_.pushFunctionState();
    ++emitFnDepth_;

    auto savedClass = currentClass_;
    std::string savedThisAddr = thisAddr_;
    auto savedReturn = currentReturn_;
    bool savedSawReturn = sawReturn_;
    bool savedBlockTerm = blockTerminated_;
    bool savedInMain = inMain_;
    const LambdaInfo* savedLambda = currentLambda_;
    auto savedLoops = loops_;
    auto savedHoist = loopHoisted_;
    auto savedSpillPools = loopSpillPools_;
    int savedSpillDepth = loopSpillDepth_;
    int savedTryCounter = tryCounter_;
    auto savedTryStack = tryStack_;
    int savedCatchDepth = catchDepth_;

    if (mi.capturesThis)
        currentClass_ = classOfType(Type::makeClass(mi.thisClassName));
    currentLambda_ = &mi;
    currentReturn_ = mi.returnType;
    sawReturn_ = false;
    blockTerminated_ = false;
    /* U8：lambda 不是 @main；try/finally 的 emitUnwindRet 勿跟外层 inMain_ 发 ret i32 */
    inMain_ = false;
    loops_.clear();
    loopHoisted_.clear();
    loopSpillPools_.clear();
    loopSpillDepth_ = 0;
    tryCounter_ = 0;

    // 签名
    std::string sig = "define " + mi.returnType->llvmType() + " " + mi.implName +
                      "(ptr %env.arg";
    for (size_t i = 0; i < mi.params.size(); ++i)
        sig += ", " + mi.paramTypes[i]->llvmType() + " %" + mi.params[i] + ".arg";
    sig += ") {";

    em_.emitBlank();
    em_.emitRaw(sig);
    em_.emitLabel("entry");
    {
        std::string spName = mi.implName;
        if (!spName.empty() && spName[0] == '@') spName = spName.substr(1);
        beginDebugFunction(mi.ctx, spName);
    }

    // try/finally 函数级槽位（含 GC 返回/异常根）
    emitAllocUnwindSlots();
    tryStack_.clear();
    catchDepth_ = 0;
    beginFunctionGcRoots();
    emitPushUnwindGcRoot();

    {
        SymbolTable::Guard guard(syms_);

        // 先分析本 lambda 体内直接内嵌的 lambda，确定本 lambda 的形参/
        // 局部变量哪些会被嵌套 lambda 捕获（需装箱）。这一步只需 AST。
        auto savedLambdas = lambdas_;
        auto savedCapNames = capturedVarNames_;
        lambdas_.clear();
        capturedVarNames_.clear();
        // 注意：不能把 mi.ctx（它本身就是 LambdaContext）直接传给
        // analyzeLambdas——那会被当成嵌套 lambda 立即返回，导致体内真正的
        // 嵌套 lambda 没被分析。逐条遍历语句即可。
        for (auto* st : mi.ctx->statement()) analyzeLambdas(st);
        // 注意：lambdaCounter_ 不恢复——@lambda$N 必须全局唯一递增，
        // 否则不同外层 lambda 里的嵌套 lambda 会撞名。

        // 从 env 加载捕获
        size_t slot = 1;
        if (mi.capturesThis) {
            thisAddr_ = "%this.lambda.addr";
            emitAllocaAt(thisAddr_, "ptr");
            // 先发射 getelementptr（fieldPtr 内部取号），再取 load 寄存器，
            // 保证编号与发射顺序一致（否则 %N 倒退导致 clang 报错）。
            std::string fp = fieldPtr("%env.arg", (int)slot);
            std::string tv = emitLoad("ptr", fp);
            emitStore("ptr", tv, thisAddr_);
            emitGcRootPush(thisAddr_);
            /* D16：capturesThis 薄 DI */
            {
                int pl = mi.ctx->getStart()
                    ? static_cast<int>(mi.ctx->getStart()->getLine()) : 1;
                emitDbgDeclareIf(thisAddr_, "this", pl, 1);
                emitDbgValueIf("ptr", tv, "this", pl, 1);
            }
            ++slot;
        }
        for (auto& cap : mi.captures) {
            std::string addr = "%" + cap.name + ".cap.addr";
            std::string fp = fieldPtr("%env.arg", (int)slot);
            std::string cv;
            if (cap.boxed) {
                emitAllocaAt(addr, "ptr");
                cv = emitLoad("ptr", fp);
                emitStore("ptr", cv, addr);
                emitGcRootPush(addr);
            } else {
                emitAllocaAt(addr, cap.type->llvmType());
                cv = emitLoad(cap.type->llvmType(), fp);
                emitStore(cap.type->llvmType(), cv, addr);
                if (isGcPointerType(cap.type))
                    emitGcRootPush(addr);
            }
            auto cs = std::make_shared<Symbol>();
            cs->kind = SymbolKind::Variable;
            cs->name = cap.name;
            cs->type = cap.type;
            cs->isMutable = true;
            cs->irAddr = addr;
            cs->boxed = cap.boxed;
            hao::symDeclare(syms_, cs);
            /* D16：捕获 unpack 薄 declare/value */
            {
                int pl = mi.ctx->getStart()
                    ? static_cast<int>(mi.ctx->getStart()->getLine()) : 1;
                emitDbgDeclareIf(addr, cap.name, pl, 0);
                emitDbgValueIf(cap.boxed ? "ptr" : cap.type->llvmType(), cv,
                               cap.name, pl, 0);
            }
            ++slot;
        }

        // 参数绑定到栈（被嵌套 lambda 捕获的参数需装箱到 cell；数组按引用）
        for (size_t i = 0; i < mi.params.size(); ++i) {
            std::string addr = "%" + mi.params[i] + ".addr";
            std::string argSrc = "%" + mi.params[i] + ".arg";
            bool boxed = capturedVarNames_.count(mi.params[i]) > 0;
            // 与顶层函数一致：[T]? 不可 by-ref（调用方传的是数据指针/null）
            bool arrayByRef = (mi.paramTypes[i]->kind == TypeKind::Array &&
                               !mi.paramTypes[i]->nullable && !boxed);
            auto ps = std::make_shared<Symbol>();
            ps->kind = SymbolKind::Variable;
            ps->name = mi.params[i];
            ps->type = mi.paramTypes[i];
            ps->isMutable = true;
            if (arrayByRef) {
                ps->irAddr = argSrc;
                ps->byRefParam = true;
                emitGcRootPush(argSrc);
            } else if (boxed) {
                emitAllocaAt(addr, "ptr");
                if (isGcPointerType(mi.paramTypes[i])) {
                    emitStore("ptr", argSrc, addr);
                    emitGcRootPush(addr);
                    std::string held = emitLoad("ptr", addr);
                    std::string cell = emitObjectNew(1, 1);
                    emitHeapStore(cell, held, mi.paramTypes[i], cell);
                    emitStore("ptr", cell, addr);
                } else {
                    std::string cell = emitObjectNew(1, 0);
                    emitHeapStore(cell, argSrc, mi.paramTypes[i], cell);
                    emitStore("ptr", cell, addr);
                    emitGcRootPush(addr);
                }
                ps->irAddr = addr;
                ps->boxed = true;
            } else {
                emitAllocaAt(addr, mi.paramTypes[i]->llvmType());
                emitStore(mi.paramTypes[i]->llvmType(), argSrc, addr);
                ps->irAddr = addr;
                if (isGcPointerType(mi.paramTypes[i]))
                    emitGcRootPush(addr);
            }
            hao::symDeclare(syms_, ps);
            /* D13：lambda 参数薄 declare/value（对齐顶层函数参数） */
            {
                int pl = mi.ctx->getStart()
                    ? static_cast<int>(mi.ctx->getStart()->getLine()) : 1;
                emitDbgDeclareIf(ps->irAddr, ps->name, pl,
                                 static_cast<unsigned>(i) + 1);
                if (!arrayByRef)
                    emitDbgValueIf(mi.paramTypes[i]->llvmType(), argSrc, ps->name,
                                   pl, static_cast<unsigned>(i) + 1);
            }
        }

        emitRuntimeFrameArgs(/*hasThis=*/false, mi.params, mi.paramTypes);

        // 生成语句；末尾表达式语句在非 Unit 返回时作为隐式返回值
        auto stmts = mi.ctx->statement();
        bool trailingIsReturn = false;
        if (!mi.returnType->isUnit() && !stmts.empty()) {
            if (auto* es = stmts.back()->exprStmt())
                trailingIsReturn = true;
        }
        size_t genCount = trailingIsReturn ? stmts.size() - 1 : stmts.size();
        /* v0.53.3：lambda 入口 safepoint */
        emitSafepoint();
        pushSmartCastFrame();
        beginBlockGcScope();
        for (size_t i = 0; i < genCount; ++i) {
            genStatement(stmts[i]);
            if (blockTerminated_) break;   // 已 ret/br，后续不可达
        }
        endBlockGcScope();

        if (!blockTerminated_) {
            if (mi.returnType->isUnit()) {
                emitGcRootUnwind();
                emitRuntimePopFrame();
                emitRetVoid();
            } else if (trailingIsReturn) {
                auto* es = stmts.back()->exprStmt();
                Value v = genExpr(es->expr());
                emitGcRootUnwind();
                emitRuntimePopFrame();
                if (!v.valid()) {
                    emitRet(mi.returnType->llvmType(), "zeroinitializer");
                } else {
                    if (!isAssignable(v.type, mi.returnType)) {
                        error(mi.ctx, "lambda 返回值类型 " + v.type->toString() +
                                      " 与推断的 " + mi.returnType->toString() + " 不匹配");
                    }
                    v = coerce(v, mi.returnType, 0, 0);
                    emitRet(mi.returnType->llvmType(), v.ir);
                }
            } else {
                error(mi.ctx, "lambda 返回 " + mi.returnType->toString() +
                              "，但存在没有 return 的执行路径");
                emitGcRootUnwind();
                emitRuntimePopFrame();
                emitRet(mi.returnType->llvmType(), zeroValueFor(mi.returnType));
            }
        }
        popSmartCastFrame();

        lambdas_ = savedLambdas;
        capturedVarNames_ = savedCapNames;
    }
    gcRootWm_.clear();
    loopHoisted_.clear();
    blockGcSlots_.clear();

    em_.flushEntryAllocas();
    em_.emitRaw("}");
    std::string def = em_.popFunctionState();
    em_.addFunctionDef(def);
    --emitFnDepth_;

    currentClass_ = savedClass;
    thisAddr_ = savedThisAddr;
    currentReturn_ = savedReturn;
    sawReturn_ = savedSawReturn;
    blockTerminated_ = savedBlockTerm;
    inMain_ = savedInMain;
    currentLambda_ = savedLambda;
    loops_ = savedLoops;
    loopHoisted_ = savedHoist;
    loopSpillPools_ = savedSpillPools;
    loopSpillDepth_ = savedSpillDepth;
    tryCounter_ = savedTryCounter;
    tryStack_ = savedTryStack;
    catchDepth_ = savedCatchDepth;
}

// ------------------------------------------------------------
//  顶层函数包装为闭包值（统一 env 调用约定）
// ------------------------------------------------------------
std::string IRGen::ensureFuncWrapper(const SymbolPtr& fnSym) {
    auto it = funcWrappers_.find(fnSym->irName);
    if (it != funcWrappers_.end()) return it->second;

    std::string wname = fnSym->irName + "$wf";
    funcWrappers_[fnSym->irName] = wname;

    std::string def;
    def += "\n; 函数值包装器：" + fnSym->name + "\n";
    def += "define " + fnSym->returnType->llvmType() + " " + wname + "(ptr %env.arg";
    for (size_t i = 0; i < fnSym->paramTypes.size(); ++i)
        def += ", " + fnSym->paramTypes[i]->llvmType() + " %" +
               fnSym->paramNames[i] + ".arg";
    def += ") {\nentry:\n";

    std::string callArgs;
    for (size_t i = 0; i < fnSym->paramTypes.size(); ++i) {
        if (i) callArgs += ", ";
        callArgs += fnSym->paramTypes[i]->llvmType() + " %" +
                    fnSym->paramNames[i] + ".arg";
    }
    if (fnSym->returnType->isUnit()) {
        def += "  call void " + fnSym->irName + "(" + callArgs + ")\n";
        def += "  ret void\n";
    } else {
        def += "  %r = call " + fnSym->returnType->llvmType() + " " +
               fnSym->irName + "(" + callArgs + ")\n";
        def += "  ret " + fnSym->returnType->llvmType() + " %r\n";
    }
    def += "}\n";

    em_.addFunctionDef(def);
    return wname;
}

// ------------------------------------------------------------
//  方法组转换：实例方法绑定为函数值（obj.method）
// ------------------------------------------------------------
//  方法实现函数签名是 ret(this, args...)，而闭包 env 约定是
//  ret(env, args...)。本包装器从 env 槽 1 取出绑定的 this，再按
//  虚表（保持多态）或静态方式调用原方法，从而把 obj.method 变成
//  一个可直接调用/传递的 (args...)->ret 函数值。
std::string IRGen::ensureMethodWrapper(const ClassInfoPtr& ci,
                                       const MethodInfo& mi) {
    auto it = methodWrappers_.find(mi.irName);
    if (it != methodWrappers_.end()) return it->second;

    std::string wname = mi.irName + "$wf";   // 带 @ 前缀，与 define 名一致
    methodWrappers_[mi.irName] = wname;

    std::string def;
    def += "\n; 方法组转换包装器：" + mi.name + "\n";
    def += "define " + mi.returnType->llvmType() + " " + wname + "(ptr %env.arg";
    for (size_t i = 0; i < mi.paramTypes.size(); ++i)
        def += ", " + mi.paramTypes[i]->llvmType() + " %p" +
               std::to_string(i) + ".arg";
    def += ") {\nentry:\n";
    // 取绑定的 this（env 槽 1）
    def += "  %thisp = getelementptr ptr, ptr %env.arg, i64 1\n";
    def += "  %thisv = load ptr, ptr %thisp\n";

    std::string argStr = "ptr %thisv";
    std::string sigTypes = "ptr";
    for (size_t i = 0; i < mi.paramTypes.size(); ++i) {
        argStr += ", " + mi.paramTypes[i]->llvmType() + " %p" +
                  std::to_string(i) + ".arg";
        sigTypes += ", " + mi.paramTypes[i]->llvmType();
    }
    std::string fnTy = mi.returnType->llvmType() + " (" + sigTypes + ")";

    // 虚方法：经 this 的虚表分派，保持多态语义
    if (mi.vtableSlot >= 0 && ci->hasVTable) {
        def += "  %vtp = getelementptr ptr, ptr %thisv, i64 0\n";
        def += "  %vt = load ptr, ptr %vtp\n";
        def += "  %mp = getelementptr ptr, ptr %vt, i64 " +
               std::to_string(mi.vtableSlot) + "\n";
        def += "  %fp = load ptr, ptr %mp\n";
        if (mi.returnType->isUnit()) {
            def += "  call " + fnTy + " %fp(" + argStr + ")\n";
            def += "  ret void\n";
        } else {
            def += "  %r = call " + fnTy + " %fp(" + argStr + ")\n";
            def += "  ret " + mi.returnType->llvmType() + " %r\n";
        }
    } else {
        if (mi.returnType->isUnit()) {
            def += "  call void " + mi.irName + "(" + argStr + ")\n";
            def += "  ret void\n";
        } else {
            def += "  %r = call " + mi.returnType->llvmType() + " " +
                   mi.irName + "(" + argStr + ")\n";
            def += "  ret " + mi.returnType->llvmType() + " %r\n";
        }
    }
    def += "}\n";

    em_.addFunctionDef(def);
    return wname;
}

} // namespace hao
