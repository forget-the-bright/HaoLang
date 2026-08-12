// ============================================================
//  HaoLang IR 生成器
// ------------------------------------------------------------
//  直接遍历 ANTLR 语法树生成 LLVM IR 文本，边遍历边做类型检查。
//
//  为何不用 ANTLR 的 Visitor 基类：
//    表达式求值需要同时返回「IR 值」和「静态类型」两项信息，
//    而 visitor 接口只能返回单个 std::any，需反复装箱拆箱。
//    手写递归下降更直观，错误定位也更精确。
// ============================================================
#pragma once

#include "HaoLangParser.h"
#include "irgen/IREmitter.h"
#include "irgen/IROps.h"
#include "irgen/SourceLoc.h"
#include "sema/Diagnostic.h"
#include "sema/SymbolTable.h"
#include "sema/Type.h"

#include <map>
#include <set>
#include <unordered_map>
#include <string>
#include <vector>

namespace hao {

// 表达式求值结果：IR 中的值 + 静态类型
struct Value {
    std::string ir;     // 如 "%3"、"42"、"@.str.0"
    TypePtr type;

    Value() : type(Type::makeUnknown()) {}
    Value(std::string v, TypePtr t) : ir(std::move(v)), type(std::move(t)) {}

    bool valid() const { return type && !type->isUnknown(); }
};

class IRGen {
public:
    IRGen(DiagnosticEngine& diags) : diags_(diags) {}

    // 设置目标三元组（交叉编译时需与 clang --target 一致）
    void setTargetTriple(const std::string& triple) { em_.setTargetTriple(triple); }

    // v0.42：hao test —— 跳过非 harness 的业务 main，由 __hao_test_main.hao 提供入口
    void setTestMode(bool v);

    // I0/I3：开启后 IROps 指令附加 !dbg；默认关（套件路径）
    void setEmitDebug(bool v) { ops_.setDebugEnabled(v); }

    // 一个编译单元 = 一个 .hao 文件：所属包、import 路径、语法树。
    // main 包的 importPath 为空，内部符号不加前缀。
    struct SourceUnit {
        std::string path;                    // 源文件路径（报错用）
        std::string package;                 // package 名，如 "main" / "calc"
        std::string importPath;              // ""(main) / "calc" / "util/strings"
        HaoLangParser::CompilationUnitContext* tree = nullptr;
    };

    // 一个 import 记录
    struct Import {
        std::string importPath;   // 如 "calc" / "util/strings"
        std::string alias;        // 限定前缀：默认末段名，as 后为别名；".*" 时为空
        bool wildcard = false;    // import x.*
    };

    // 生成多个编译单元（可能跨包）的 IR；失败返回空串
    std::string generate(const std::vector<SourceUnit>& units);

    // 兼容旧接口：单个编译单元视为 package main
    std::string generate(HaoLangParser::CompilationUnitContext* unit) {
        SourceUnit u;
        u.package = "main";
        u.importPath = "";
        u.tree = unit;
        return generate(std::vector<SourceUnit>{u});
    }

    // 所有 extern 函数 @link("...") 声明的外部链接库（去重），供 Driver 拼链接命令。
    const std::vector<std::string>& linkLibraries() const { return linkLibs_; }

private:
    // ---------- 顶层 ----------
    //  为支持跨包/通配导入的类型名解析，收集分两阶段：
    //    阶段 A（register*）：为所有单元登记类型名（classes_/interfaces_ 的 key、
    //                         pkgExports_），不解析成员；
    //    阶段 B（collect*Members）：解析字段/方法/函数签名（此时所有包的类型名
    //                         都已就位，可互相引用）。
    void registerInterfaceNames(const SourceUnit& u);
    void registerClassNames(const SourceUnit& u);
    void collectInterfaceMembers(const SourceUnit& u);
    void collectClassMembers(const SourceUnit& u);
    void collectFunctionSignatures(const SourceUnit& u);
    void collectGenericFunctions(const SourceUnit& u);
    void collectDelegates(const SourceUnit& u);   // v0.19.0：收集 delegate 类型别名
    void emitClassMeta();   // v0.19.0：为每个类生成反射类型元数据（虚表已生成后调）
    // v0.31：构造工厂 @Class$new(ptr argslots)（0～N 参，槽约定同 invoke）；不可实例化则 "null"
    std::string emitNewFactory(const ClassInfoPtr& ci);
    // v0.19.0：把 @Name(args) 注解使用解析为 AnnotationUse 列表（参数编译期常量字符串化）
    std::vector<AnnotationUse> resolveAnnotationUses(
        const std::vector<HaoLangParser::AnnotationUseContext*>& uses);
    // 求值注解参数为常量字符串（支持字符串/整数/浮点/布尔/标识符）
    std::string annotationArgValue(HaoLangParser::ExprContext* e);
    void genUnitTopLevel(const SourceUnit& u);   // 生成非泛型顶层函数/类代码

    // 设置当前正在处理的单元（包前缀、imports 等），供名字解析使用
    void setCurrentUnit(const SourceUnit& u, const std::vector<Import>& imp);

    // I0：从 ANTLR 节点取 SourceLoc（文件用 currentUnitPath_）
    SourceLoc locFrom(antlr4::ParserRuleContext* ctx) const;
    SourceLoc locFrom(antlr4::Token* tok) const;
    void setDebugLoc(antlr4::ParserRuleContext* ctx) {
        SourceLoc loc = locFrom(ctx);
        ops_.setDebugLoc(loc);
        /* 运行期 TLS：不在 genExpr 路径更新（过频）；见 genStatement */
    }
    void clearDebugLoc() {
        ops_.clearDebugLoc();
        em_.clearDebugSubprogram();
    }
    // 用户函数入口：填 loc + 新建 DISubprogram + push Hao 调用帧（P1）
    void beginDebugFunction(antlr4::ParserRuleContext* ctx,
                            const std::string& name) {
        SourceLoc loc = locFrom(ctx);
        ops_.setDebugLoc(loc);
        em_.beginDebugSubprogram(name, loc.line);
        emitRuntimePushFrame(loc, name);
    }
    // L0b/P1：TLS 源码位；函数帧 push/pop（与 -g 解耦）
    void emitRuntimeSrcLoc(const SourceLoc& loc);
    /* 调用/方法分派前钉 TLS src= 到调用点（禁漂到 callee/native） */
    void pinRuntimeCallSite(antlr4::ParserRuleContext* ctx);
    void emitRuntimePushFrame(const SourceLoc& loc, const std::string& funcName);
    void emitRuntimePopFrame();
    void emitRuntimeFrameArgs(bool hasThis,
                              const std::vector<std::string>& paramNames,
                              const std::vector<TypePtr>& paramTypes);

    // 顶层声明是否带 private 修饰（mods 来自 *DeclContext::modifier()）
    static bool declIsPrivate(const std::vector<HaoLangParser::ModifierContext*>& mods);
    // 顶层声明是否带 extern 修饰（声明外部 C 函数，无函数体）
    static bool declIsExtern(const std::vector<HaoLangParser::ModifierContext*>& mods);

    // ---------- 泛型函数单态化 ----------
    //  按实参推断类型参数，生成如 firstOf$Int 的专用函数。
    //  实例化结果缓存，同一组合只生成一次。
    struct GenericFn {
        std::string name;
        std::string pkgPrefix;       // 模板所属包前缀（main 为 ""）
        std::vector<std::string> typeParams;
        std::vector<TypeParamConstraint> constraints;
        HaoLangParser::FuncDeclContext* decl = nullptr;
    };
    std::map<std::string, GenericFn> genericFns_;   // 模板名 -> 模板
    std::set<std::string> generatedGenericFns_;      // 已生成的实例名
    // 实例化泛型函数并返回符号；args 用于推断 T
    SymbolPtr instantiateFunction(const std::string& tplName,
                                  const std::vector<Value>& args,
                                  antlr4::ParserRuleContext* useSite);

    // 泛型函数调用的统一入口：两遍求值实参以支持从函数类型实参
    // （(T)->R 的 lambda）递归推断嵌套类型参数。tplName 是内部模板名
    // （含包前缀）。返回调用结果值。
    Value callGenericFunction(const std::string& tplName,
                              HaoLangParser::ArgListContext* al,
                              antlr4::ParserRuleContext* ctx);

    // 把模板形参类型与实参类型做合一，把发现的 类型参数->实际类型
    // 写入 subst（递归处理数组、函数类型）。冲突时报错。
    void unifyWithArg(const TypePtr& param, const TypePtr& arg,
                      TypeSubst& subst, antlr4::ParserRuleContext* ctx);
    void genPendingFunctionInstances();
    struct PendingFnInstance {
        HaoLangParser::FuncDeclContext* decl;
        TypeSubst subst;
        std::string instName;     // 含包前缀的完整实例名
        std::string pkgPrefix;    // 模板所属包前缀
    };
    std::vector<PendingFnInstance> pendingFnInstances_;

    // ---------- 泛型方法单态化（v0.9.0）----------
    //  方法级类型参数（如 List<T>.map<R> 的 R）在调用时从 lambda 实参推断，
    //  单态化为「类实例名.方法名$R」（如 List$Int.map$String）。
    //  与顶层泛型函数同路线，但方法多了隐式 this 与类级 T 替换。
    struct GenericMethod {
        std::string className;          // 声明类模板名（如 List / Util）
        std::string methodName;
        std::string pkgPrefix;          // 声明类所属包前缀
        std::vector<std::string> typeParams;   // 方法级类型参数（如 R）
        std::vector<TypeParamConstraint> constraints;
        HaoLangParser::FuncDeclContext* decl = nullptr;
    };
    std::map<std::string, GenericMethod> genericMethods_;   // key = className + "." + methodName

    struct PendingMethodInstance {
        GenericMethod* tmpl = nullptr;
        std::string instClass;      // 实例类名（List$Int）
        TypeSubst classSubst;       // 类级替换（T->Int）
        TypeSubst methodSubst;      // 方法级替换（R->String）
        std::string pkgPrefix;      // 实例类所属包前缀
        std::string instName;       // 如 List$Int.map$String
    };
    std::vector<PendingMethodInstance> pendingMethodInstances_;
    std::set<std::string> generatedMethodInstances_;
    std::map<std::string, MethodInfo> methodInstanceInfos_;   // instName -> 已解析签名

    // 泛型方法调用统一入口（applyMethodCall 的类方法分支调用）：
    // 两遍求值实参推断方法级类型参数，实例化方法并直接静态调用。
    Value callGenericMethod(const Value& recv, const ClassInfoPtr& ci,
                            const GenericMethod& gm,
                            HaoLangParser::CallOpContext* call,
                            antlr4::ParserRuleContext* ctx);
    // 生成所有已实例化泛型方法的方法体（在 genPendingInstantiations 之后）。
    void genPendingMethodInstances();
    // 用显式签名 mi 生成泛型方法实例的方法体（含隐式 this）。
    void genMethodBodyFromMI(HaoLangParser::FuncDeclContext* fn,
                             const MethodInfo& mi, const ClassInfoPtr& ci);
    // 解析继承链：连接 base 指针、检测环、扁平化字段与方法
    void resolveInheritance();
    // 分配虚表槽位（v0.18.0 起拆自 resolveInterfaceImpls）：
    //   ①给接口模板 + 非泛型接口的方法分配全局槽位（泛型接口实例共享模板槽位）
    //   ②给参与继承的类虚方法分配槽位。先于任何接口实例化运行。
    void assignVtableSlots();
    // 遍历 pendingGenericInterfaces_ 逐个 instantiateInterface（模板槽位须已分配）。
    void instantiateAllGenericInterfaces();
    // 逐类校验接口实现并填充 vtableEntries（须在全部泛型类实例生成后运行）。
    void fillVtableEntries();
    void emitVTables();
    // 为 is / as 生成「类型及其所有子类的虚表列表」
    void emitTypeIdLists();
    // is/as/catch 时按需补发 typeids（mono 可能在 codegen 中途才实例化）
    bool ensureTypeIdList(const TypePtr& target);
    // 接口继承展平 + 默认方法冲突检测（collectInterfaceMembers 之后）
    void resolveInterfaceInheritance();
    // 发射接口默认方法实现（@Iface.m.default）
    void genInterfaceDefaultMethods();
    // where 约束：解析 + 实例化时校验
    std::vector<TypeParamConstraint> parseWhereClause(
        HaoLangParser::WhereClauseContext* wc);
    void checkTypeConstraints(const std::vector<TypeParamConstraint>& cs,
                              const TypeSubst& subst,
                              antlr4::ParserRuleContext* useSite);
    // 自动属性：JavaBeans getX/setX 名
    static std::string propGetterName(const std::string& prop);
    static std::string propSetterName(const std::string& prop);
    // 收集类/接口自动属性（字段 + 合成访问器；接口仅抽象访问器）
    bool collectClassAutoProperty(HaoLangParser::PropertyDeclContext* pd,
                                  const ClassInfoPtr& ci, int& slot);
    bool collectInterfaceAutoProperty(HaoLangParser::PropertyDeclContext* pd,
                                      const InterfaceInfoPtr& ii);
    // 发射合成 get/set 方法体
    void genSyntheticPropMethods(const ClassInfoPtr& ci);
    void genFunction(HaoLangParser::FuncDeclContext* fn);
    // 泛型实例化版本：用显式符号生成
    void genFunction(HaoLangParser::FuncDeclContext* fn,
                       const std::string& instName, const SymbolPtr& sym);
    void genClass(HaoLangParser::ClassDeclContext* cls);
    // 生成枚举类代码：构造器 + name/ordinal/toString 方法 + 静态字段全局
    void genEnum(HaoLangParser::EnumDeclContext* ed, const ClassInfoPtr& ci);
    void genMethod(HaoLangParser::FuncDeclContext* fn, const ClassInfoPtr& ci);
    void genConstructor(HaoLangParser::ConstructorDeclContext* ctor,
                        const ClassInfoPtr& ci);

    // 为类的静态字段发射全局变量（@Class.X = global <type> <init>）。
    // 常量立即写进全局；非常量初始化留待静态构造器（genStaticConstructor）。
    void emitStaticFieldGlobals(const ClassInfoPtr& ci);
    // 为静态 GC 指针字段生成 @hao.registerStaticRoots，并在 main 入口调用。
    void emitStaticGcRootRegistration();
    // 求静态字段的常量初始值（返回 LLVM 常量串，如 "42"、"@.str.0"、"null"）；
    // 无法用常量表示返回空串。
    std::string evalStaticInit(antlr4::tree::ParseTree* node, TypeKind kind);
    // 类型的零值常量（全局零初始化用）
    std::string zeroValueFor(const TypePtr& t);
    // 生成 C# 风格静态构造器（static ClassName()）：@Class.initguard 守卫 +
    // @Class.staticinit（非常量静态字段初始化 + 静态构造器体）+ @Class.ensureInit。
    // 无需要的类（无静态构造器且无非零/非常量静态字段）则置 hasStaticInit=false 跳过。
    void genStaticConstructor(const ClassInfoPtr& ci);
    // 代码生成前预置 hasStaticInit（避免「函数声明在类之前」时漏 ensureInit）
    void markStaticInitFlags();
    // v0.50.2：为每个有虚表的类合成 static Class: reflect.Class（对标 Java 类字面量 /
    // Object 类型身份 API；每类型一份常量，经 ensureInit 填单例）
    void ensureClassStaticField(const ClassInfoPtr& ci);
    void synthesizeClassStaticFields();
    // v0.56.1+：TypeName.Class ← classOfMeta(hao_handle_wrap(@T.meta))
    // 禁把裸 HaoClassMeta* 当 NativeHandle 传入（改 Handle ABI 必须同步本路径）
    std::string emitClassOfMetaFromRawMeta(const ClassInfoPtr& ci);
    // 在静态成员访问 / new 前发射惰性初始化调用（仅当 hasStaticInit）
    void emitStaticEnsureInit(const ClassInfoPtr& ci);

    // ---------- 语句 ----------
    void genStatement(HaoLangParser::StatementContext* st);
    void genBlock(HaoLangParser::BlockContext* blk, bool newScope = true);
    void genVarDecl(HaoLangParser::VarDeclContext* vd);
    void genIf(HaoLangParser::IfStmtContext* st);
    void genWhile(HaoLangParser::WhileStmtContext* st);
    void genFor(HaoLangParser::ForStmtContext* st);
    // 数组迭代循环体（genFor 的快路径 / toArray 兜底共用）
    void genForArray(HaoLangParser::ForStmtContext* st, const std::string& varName,
                     const Value& seq);
    // Iterable<X> 接口迭代循环体（v0.18.0）：iterator()/hasNext()/next() 虚表分派
    void genForIterable(HaoLangParser::ForStmtContext* st, const std::string& varName,
                        const Value& seq, InterfaceInfoPtr itf, const TypePtr& elemType);
    void genWhenStmt(HaoLangParser::WhenStmtContext* st);
    void genReturn(HaoLangParser::ReturnStmtContext* st);
    void genTry(HaoLangParser::TryStmtContext* st);
    void genThrow(HaoLangParser::ThrowStmtContext* st);
    void genHaoroutine(HaoLangParser::HaoroutineStmtContext* st);
    void genSelect(HaoLangParser::SelectStmtContext* st);
    void genExprStmt(HaoLangParser::ExprStmtContext* st);

    // ---------- 表达式 ----------
    Value genExpr(HaoLangParser::ExprContext* e);
    Value genAssign(HaoLangParser::AssignExprContext* e);
    Value genTernary(HaoLangParser::TernaryExprContext* e);
    Value genNullCoalesce(HaoLangParser::NullCoalesceExprContext* e);
    Value genOr(HaoLangParser::OrExprContext* e);
    Value genAnd(HaoLangParser::AndExprContext* e);
    Value genBitOr(HaoLangParser::BitOrExprContext* e);
    Value genBitXor(HaoLangParser::BitXorExprContext* e);
    Value genBitAnd(HaoLangParser::BitAndExprContext* e);
    Value genEquality(HaoLangParser::EqualityExprContext* e);
    Value genRelational(HaoLangParser::RelationalExprContext* e);
    Value genShift(HaoLangParser::ShiftExprContext* e);
    Value genAdditive(HaoLangParser::AdditiveExprContext* e);
    Value genMultiplicative(HaoLangParser::MultiplicativeExprContext* e);
    Value genUnary(HaoLangParser::UnaryExprContext* e);
    Value genPostfix(HaoLangParser::PostfixExprContext* e);

    // 把 postfix 链求值为"数组基值"：沿 primary 应用所有成员访问（.field），
    // 用于 obj.field[i] 与 obj.field += item。失败返回无效 Value。
    Value evalPostfixBase(HaoLangParser::PostfixExprContext* pf);

    // ---------- 后缀操作的单步归约 ----------
    //  后缀链（a.b[0].c() 之类）需要逐个操作依次作用于前一步的结果，
    //  因此把每种操作抽成独立函数，由 genPostfix 用循环驱动。
    //  失败时返回无效 Value 并已报错。

    // base.field —— 成员读取（字段 / .length）
    Value applyMemberAccess(const Value& base, const std::string& field,
                            antlr4::ParserRuleContext* ctx);
    // base[idx] —— 数组索引读取
    Value applyIndex(const Value& base, HaoLangParser::IndexOpContext* io,
                     antlr4::ParserRuleContext* ctx);
    // base.method(args) —— 方法调用（含接口动态分派与虚方法）
    Value applyMethodCall(const Value& base, const std::string& method,
                          HaoLangParser::CallOpContext* call,
                          antlr4::ParserRuleContext* ctx);

    // ---------- Lambda / 闭包 ----------
    //  自由变量分析：在生成函数体前扫描整个 body，收集每个 lambda 实际
    //  引用到的外层（非本 lambda 形参）名字。结果用于：
    //    1) 决定哪些外层 var 需要堆装箱（被任意 lambda 引用的可变变量）；
    //    2) 生成闭包对象时知道要捕获哪些值。
    struct LambdaCapture {
        std::string name;       // 外层变量名；this 捕获用 "$this"
        TypePtr type;           // genLambda 时解析填入
        bool boxed = false;     // 是否按引用捕获（外层是 var/参数）
    };
    struct LambdaInfo {
        std::string implName;          // 实现函数的 IR 名，如 @lambda$3
        HaoLangParser::LambdaContext* ctx = nullptr;
        std::vector<std::string> params;
        std::vector<TypePtr> paramTypes;
        TypePtr returnType;
        bool capturesThis = false;
        std::string thisClassName;
        std::vector<LambdaCapture> captures;   // 按出现顺序去重
    };
    // body 所在函数作用域内所有 lambda 的信息（按出现顺序）
    std::vector<LambdaInfo> lambdas_;
    int lambdaCounter_ = 0;
    // 已发射的 lambda 实现名（字段默认值可在多次 new 时复用同一 AST 节点）
    std::set<std::string> emittedLambdaImpls_;
    // 当前函数体内被任意 lambda 捕获的变量名集合（genFunctionBody 分析得出）。
    // genVarDecl / 参数绑定据此决定是否把可变变量装箱到堆 cell。
    std::set<std::string> capturedVarNames_;
    // 当前正在生成的 lambda 的捕获信息（nullptr 表示不在 lambda 内）
    const LambdaInfo* currentLambda_ = nullptr;

    // 扫描一个函数体内的所有直接内嵌 lambda，填充 lambdas_（每条含
    // 捕获名列表）。bound 为本函数/ lambda 自身的形参名（视为已绑定，
    // 不作为自由变量）。嵌套 lambda 在其外层 impl 生成时再单独分析。
    void analyzeLambdas(antlr4::tree::ParseTree* body);

    // 生成期间临时使用的「期望类型」栈：var 声明的标注类型、函数调用
    // 的形参类型会在求值初始化式/实参前压栈，供 lambda 推断参数与
    // 返回类型使用（val f: (Int)->Int = { it*2 }）。
    std::vector<TypePtr> expectedTypes_;
    struct ExpectedTypeGuard {
        IRGen* g;
        ~ExpectedTypeGuard() { if (g) g->expectedTypes_.pop_back(); }
    };
    TypePtr peekExpectedType() const {
        return expectedTypes_.empty() ? nullptr : expectedTypes_.back();
    }

    // 递归收集子树中「未被当前 lambda/函数自身形参及局部声明绑定」的
    // 标识符引用（即自由变量）。进入 lambda 时重置绑定集合，使其形参
    // 与内部局部声明生效，而外层变量成为自由变量。declared 跨语句累积。
    std::set<std::string> collectFreeNames(antlr4::tree::ParseTree* node,
                                           std::set<std::string>& declared);
    // 类名/顶层函数名（含通配导入 lang.Integer 等）不应记入闭包捕获
    bool isResolvableTypeOrFuncName(const std::string& name);

    // 生成一个 lambda 实现函数的完整 define，并登记到 IREmitter。
    void genLambdaImpl(const LambdaInfo& li);

    // 把一个顶层函数符号包装成统一闭包约定（env, args...）的实现函数，
    // 使顶层函数能当作函数值传递；返回 wrapper 的 IR 名（带缓存）。
    std::string ensureFuncWrapper(const SymbolPtr& fnSym);
    std::map<std::string, std::string> funcWrappers_;   // fn irName -> wrapper irName

    // 方法组转换（v0.19.0）：把实例方法绑定为函数值（obj.method）。
    // 生成一个从 env 槽 1 取 this 的 wrapper，返回 wrapper 的 IR 名（带缓存）。
    std::string ensureMethodWrapper(const ClassInfoPtr& ci, const MethodInfo& mi);
    std::map<std::string, std::string> methodWrappers_; // method irName -> wrapper irName

    // 反射 invoke thunk（v0.20.0）：为反射方法生成统一签名
    //   i64 @<clsid>.<mname>$invk(i64 %obj, ptr %argslots)
    // 的调用器，把 [Int] 参数槽按方法真实签名编组后调用（虚方法经虚表分派），
    // 返回 8 字节结果。emitClassMeta 对每个本类声明的方法调用并存入方法元数据。
    std::string emitInvokeThunk(const ClassInfoPtr& ci, const MethodInfo& mi);

    Value genLambda(HaoLangParser::LambdaContext* lam);

    // ---------- 空安全 ----------
    // 生成对 ptr 非空性的 i1 检查（非引用类型的可空值已被装箱为 ptr）
    std::string genNotNullCheck(const Value& v);
    // base?.field
    Value applySafeMemberAccess(const Value& base, const std::string& field,
                                antlr4::ParserRuleContext* ctx);
    // base?.method(args)
    Value applySafeMethodCall(const Value& base, const std::string& method,
                              HaoLangParser::CallOpContext* call,
                              antlr4::ParserRuleContext* ctx);
    // base!! —— 非空断言，失败调 hao_panic_null
    Value applyNotNullAssert(const Value& base, antlr4::ParserRuleContext* ctx);
    // 把值类型（Int/Double/Bool）装箱为可空指针；引用类型原样返回
    Value boxToNullable(const Value& v);

    Value genPrimary(HaoLangParser::PrimaryContext* e);
    Value genLiteral(HaoLangParser::LiteralContext* lit);
    Value genTemplateString(HaoLangParser::TemplateStringContext* ts);
    Value genArrayLiteral(HaoLangParser::ArrayLiteralContext* al);
    // 生成指定元素类型的空数组（用于 var xs: [T] = [] 这类带标注的空字面量）
    Value genEmptyArray(const TypePtr& elemType);
    // v0.32：定长数组 new [T](n) / new [T](n, fill)；len 为整数类型
    Value genSizedArray(const TypePtr& elemType, const Value& len,
                        const Value* fill, antlr4::ParserRuleContext* where);
    // 判断表达式是否为空数组字面量 []（接受任意语法节点）
    static bool isEmptyArrayLiteral(antlr4::tree::ParseTree* e);
    Value genWhenExpr(HaoLangParser::WhenStmtContext* w);

    // ---------- 辅助 ----------
    // 加载变量当前值
    Value loadVar(const SymbolPtr& sym);

    // 返回变量「实际值所在地址」的指针寄存器：
    //   - 普通变量：直接是 sym->irAddr（alloca）
    //   - 被捕获而装箱的变量：先从 holder 读出 cell 指针，cell 才存真实值
    // 调用方据此生成 load/store。
    std::string varValuePtr(const SymbolPtr& sym);

    // 数组实参按引用：返回「存放数组指针的槽」地址（供形参 byRef）。
    // 变量/字段取 lvalue 槽；其它表达式则临时 alloca + store。
    std::string arrayArgSlot(antlr4::tree::ParseTree* expr, const Value& arrVal);

    // 将已求值实参格式化为 call 参数片段。
    // arrayByRef=true（HaoLang 函数/方法）：数组传「指针槽」地址，形参 += 可写回。
    // arrayByRef=false（extern C）：传数组数据指针，与 runtime ABI 一致。
    std::string formatCallArg(const TypePtr& paramTy,
                              antlr4::tree::ParseTree* expr,
                              Value arg,
                              bool arrayByRef = true);

    // 赋值左值：求值 postfix 前缀（不含 endExclusive 及之后），支持 .field / !!。
    bool evalAssignRecv(HaoLangParser::PostfixExprContext* pf,
                        size_t endExclusive, Value& recv);

    // 将 v 转换为目标类型（目前仅 Int -> Double）
    Value coerce(const Value& v, const TypePtr& target, size_t line, size_t col);

    // 运算数禁止可空数值/装箱（无 smart cast，须 !! / ??）；失败已报错
    bool ensureNonNullOperand(const Value& v, antlr4::ParserRuleContext* ctx,
                              const std::string& op);

    // 整数除/模前：除数为 0 则 call @hao_panic_div_zero（浮点不检查）
    void emitIntDivZeroCheck(const Value& divisor);

    // 有符号 / %：MIN/-1 时把除数改成 1，避免 sdiv/srem 毒值（等同 Java 环绕）
    Value emitSafeSignedDivisor(const Value& dividend, const Value& divisor);

    // 把任意类型的值转成 String（模板字符串插值用）
    Value toStringValue(const Value& v);

    // 把 Bool 的 i8 表示取成 i1（用于 br 条件）
    std::string toI1(const Value& v);

    // 计算数组元素地址（含边界检查），返回指针寄存器名
    std::string indexAsI64(const Value& idx);
    std::string arrayElemPtr(const Value& arr, const Value& idx);

    // 纯类型推断：不发射任何指令，仅推断表达式的静态类型。
    // 用于 when 表达式——需要在生成分支代码之前就知道结果类型，
    // 以便把 alloca 放在入口块而非分支块内。
    TypePtr inferExprType(HaoLangParser::ExprContext* e);

    // 上面的通用版本，可作用于表达式层级中的任意节点
    TypePtr inferNodeType(antlr4::tree::ParseTree* node);

    // 纯 AST 推断 lambda 的函数类型（参数类型来自显式标注；返回类型取
    // 末尾表达式或 return）。无显式标注且无上下文时参数为 Unknown。
    TypePtr inferLambdaType(HaoLangParser::LambdaContext* lam);

    // 查找类信息；未找到返回 nullptr
    ClassInfoPtr lookupClass(const std::string& name);

    // 由类型取 ClassInfo。泛型类型 Box<Int> 的实例名是 Box$Int，
    // 因此不能直接用 className 查找，需经 monoName 转换。
    ClassInfoPtr classOfType(const TypePtr& t);

    // ---------- 泛型单态化 ----------
    //  按实际类型参数实例化泛型类，返回实例的 ClassInfo。
    //  同一组合只实例化一次（结果缓存在 classes_ 中）。
    //  失败返回 nullptr 并已报错。
    ClassInfoPtr instantiateClass(const std::string& templateName,
                                  const std::vector<TypePtr>& args,
                                  antlr4::ParserRuleContext* useSite);
    // 实例化泛型接口（v0.18.0）：复制模板 methods + substType 替换，复制模板槽位。
    // 幂等：同名实例已存在直接返回。须在模板槽位分配后调用。
    InterfaceInfoPtr instantiateInterface(const std::string& templateName,
                                          const std::vector<TypePtr>& args,
                                          antlr4::ParserRuleContext* useSite);
    // 递归实例化类型中引用的泛型接口（如 Iterable<T>.iterator() 返回 Iterator<T>，
    // 实例化 Iterable<Int> 时须连带实例化 Iterator<Int>）。
    void ensureInterfaceInstances(const TypePtr& t);

    // 生成所有已实例化泛型类的代码。
    // 必须在遍历完全部代码后调用：实例化可能发生在任意位置，
    // 且实例化过程本身可能触发新的实例化（如 Box<Box<Int>>）。
    void genPendingInstantiations();

    // 当前生效的类型参数替换表（生成泛型实例的成员时非空）
    TypeSubst currentSubst_;

    // 当前作用域可见的类型参数名（处理泛型模板成员时非空）。
    // resolveType 据此把 T 解析为类型参数而非同名的类。
    std::set<std::string> currentTypeParams_;

    // 待生成代码的泛型实例队列
    std::vector<ClassInfoPtr> pendingInstances_;

    // 需实例化的泛型接口，key=实例名(monoName)、value=Type(className+typeArgs)，去重。
    // 由 collectClassMembers 登记，assignVtableSlots 之后由 instantiateAllGenericInterfaces 消费。
    std::map<std::string, TypePtr> pendingGenericInterfaces_;

    // delegate 命名函数类型别名：名 -> 函数类型（v0.19.0）。
    // 仅作类型别名，不产生任何代码。resolveType 命中时返回对应 Func 类型。
    std::map<std::string, TypePtr> delegates_;

    // vtable 全局槽位总数（assignVtableSlots 计算，fillVtableEntries 用它定 vtableEntries 长度）
    size_t vtableTotalSlots_ = 0;

    // 查找接口信息；未找到返回 nullptr
    InterfaceInfoPtr lookupInterface(const std::string& name);

    // 赋值兼容性判断。相比 Type::assignableTo 额外处理
    // 「类 -> 它实现的接口」这一情形（需查符号表，故不能放在 Type 里）。
    bool isAssignable(const TypePtr& from, const TypePtr& to);

    // v0.59：Java 风格原生↔包装装箱对（Int↔Integer 等）；"" = 非包装
    static std::string wrapperClassNameFor(TypeKind prim);
    static TypeKind primitiveKindForWrapper(const std::string& className);
    static const char* unboxMethodName(TypeKind prim);
    bool isPrimitiveToWrapper(const TypePtr& from, const TypePtr& to) const;
    bool isWrapperToPrimitive(const TypePtr& from, const TypePtr& to) const;
    bool typeArgPecsAssignable(const TypePtr& fromArg, const TypePtr& toArg);
    // 插入 valueOf / *Value() 调用
    Value emitBoxToWrapper(const Value& prim, const TypePtr& wrapTy);
    Value emitUnboxWrapper(const Value& wrap, const TypePtr& primTy);

    // 找出一组值的公共父类型名（基类优先，其次接口），
    // 用于多态数组的元素类型推断；不存在则返回空串。
    std::string findCommonSupertype(const std::vector<Value>& vals);

    // 计算对象字段地址，返回指针寄存器名
    std::string fieldPtr(const std::string& objIR, int slot);

    // GC v3：引用 / 装箱可空视为堆指针
/** 与 Type::isGcManaged 同义（docs/类型属性.md）；历史名保留。 */
    static bool isGcPointerType(const TypePtr& t);
    // 类实例字段位图（slot i 为 GC 指针则 bit i=1；vtable 槽不在 fields 中）
    static int64_t objectPtrBitmap(const ClassInfo* ci);
    // 堆 store + 可选写屏障（barrierBase 非空则对 **addr 槽** 调混合屏障）
    void emitHeapStore(const std::string& addr, const std::string& valIr,
                       const TypePtr& ty, const std::string& barrierBase);
    // 静态/全局 GC 指针：混合屏障再 store
    void emitGlobalGcStore(const std::string& gptr, const std::string& valIr,
                           const TypePtr& ty);
    // 变量写回：boxed 走堆屏障，否则写栈/全局槽
    void emitVarStore(const SymbolPtr& sym, const TypePtr& ty,
                      const std::string& valIr);
    std::string emitObjectNew(int64_t nfields, int64_t bitmap);
    // v0.54/v0.55.7：Hao 精确根（shadow；循环提升/spill 池；块尾清槽）
    void emitGcRootPush(const std::string& slotAddr);
    void emitGcRootUnwind();
    // Phase A / I2：唯一 safepoint 出口（禁裸 call @hao_gc_safepoint）
    // I2+ 纪律：gen* 禁止手写 `call void @hao_gc_safepoint`；
    //   允许出现处仅 `emitSafepoint` 实现体 + IREmitter declare。
    void emitSafepoint();
    // Phase B：通用指令封装 —— 实现在 IROps；此处转发兼容 gen*
    std::string emitLoad(const std::string& ty, const std::string& ptr) {
        return ops_.emitLoad(ty, ptr);
    }
    void emitStore(const std::string& ty, const std::string& val,
                   const std::string& ptr) {
        ops_.emitStore(ty, val, ptr);
    }
    std::string emitCall(const std::string& retTy, const std::string& callee,
                         const std::string& argsIr) {
        return ops_.emitCall(retTy, callee, argsIr);
    }
    void emitCallVoid(const std::string& callee, const std::string& argsIr) {
        ops_.emitCallVoid(callee, argsIr);
    }
    void emitCallTyped(const std::string& fnTy, const std::string& callee,
                       const std::string& argsIr) {
        ops_.emitCallTyped(fnTy, callee, argsIr);
    }
    void emitBr(const std::string& label) { ops_.emitBr(label); }
    void emitCondBr(const std::string& cond, const std::string& trueL,
                    const std::string& falseL) {
        ops_.emitCondBr(cond, trueL, falseL);
    }
    void emitRetVoid() { ops_.emitRetVoid(); }
    void emitRet(const std::string& ty, const std::string& val) {
        ops_.emitRet(ty, val);
    }
    std::string emitAlloca(const std::string& ty) {
        return ops_.emitAlloca(ty);
    }
    std::string emitAllocaNamed(const std::string& hint, const std::string& ty) {
        return ops_.emitAllocaNamed(hint, ty);
    }
    void emitAllocaAt(const std::string& addr, const std::string& ty) {
        ops_.emitAllocaAt(addr, ty);
    }
    // D3：-g 时发射薄 llvm.dbg.declare
    void emitDbgDeclareIf(const std::string& addr, const std::string& name,
                          int line, unsigned arg = 0) {
        if (!em_.debugEnabled() || addr.empty() || name.empty()) return;
        ops_.emitDbgDeclare(addr, name, line > 0 ? static_cast<unsigned>(line) : 1u,
                            arg);
    }
    // D5：-g 时发射薄 llvm.dbg.value
    void emitDbgValueIf(const std::string& llvmTy, const std::string& val,
                        const std::string& name, int line, unsigned arg = 0) {
        if (!em_.debugEnabled() || llvmTy.empty() || val.empty() || name.empty())
            return;
        ops_.emitDbgValue(llvmTy, val, name,
                          line > 0 ? static_cast<unsigned>(line) : 1u, arg);
    }
    std::string emitBinOp(const std::string& op, const std::string& ty,
                          const std::string& lhs, const std::string& rhs) {
        return ops_.emitBinOp(op, ty, lhs, rhs);
    }
    std::string emitICmp(const std::string& pred, const std::string& ty,
                         const std::string& lhs, const std::string& rhs) {
        return ops_.emitICmp(pred, ty, lhs, rhs);
    }
    std::string emitPhi(const std::string& ty, const std::string& incomingsIr) {
        return ops_.emitPhi(ty, incomingsIr);
    }
    std::string emitFCmp(const std::string& pred, const std::string& ty,
                         const std::string& lhs, const std::string& rhs) {
        return ops_.emitFCmp(pred, ty, lhs, rhs);
    }
    std::string emitSelect(const std::string& cond, const std::string& ty,
                           const std::string& tVal, const std::string& fVal) {
        return ops_.emitSelect(cond, ty, tVal, fVal);
    }
    std::string emitCast(const std::string& op, const std::string& fromTy,
                         const std::string& val, const std::string& toTy) {
        return ops_.emitCast(op, fromTy, val, toTy);
    }
    std::string emitGep(const std::string& pointeeTy, const std::string& ptr,
                        const std::string& idxTy, const std::string& idx) {
        return ops_.emitGep(pointeeTy, ptr, idxTy, idx);
    }
    std::string emitPtrToInt(const std::string& intTy, const std::string& ptr) {
        return ops_.emitPtrToInt(intTy, ptr);
    }
    std::string emitIntToPtr(const std::string& intTy, const std::string& val) {
        return ops_.emitIntToPtr(intTy, val);
    }
    std::string emitExtractValue(const std::string& aggTy,
                                 const std::string& agg, int index) {
        return ops_.emitExtractValue(aggTy, agg, index);
    }
    std::string emitFNeg(const std::string& ty, const std::string& val) {
        return ops_.emitFNeg(ty, val);
    }
    void emitUnreachable() { ops_.emitUnreachable(); }
    void beginFunctionGcRoots();
    // 分配函数级 unwind 槽（含 GC 返回/异常 ptr 根槽；push 须在 beginFunctionGcRoots 之后）
    void emitAllocUnwindSlots();
    void emitPushUnwindGcRoot();
    // 把 GC 指针（或 null）写入 unwindGcRootAddr_，供跨 finally/safepoint 保活
    void storeUnwindGcRootPtr(const std::string& ptrIr);
    void clearUnwindGcRoot();
    // 分配 ptr 槽、写入初值并 root_push；循环内走 spill 池（只 push 一次）
    std::string emitSpillGcRoot(const std::string& nameHint, const std::string& ptrIr);
    /* 块/分支作用域：只 store null，不 root_unwind（曾 unwind 致套件 AV）
     * G1 口径（v0.55.33）：本机 + loop spill 即为作用域临时根机；
     * 语句级 expr 清槽曾误杀 new/构造期根，禁止盲目开启。 */
    std::vector<std::vector<std::string>> blockGcSlots_;
    void beginBlockGcScope();
    void endBlockGcScope();
    void noteBlockGcSlot(const std::string& slotAddr);
    // 循环 spill 池：整段嵌套 while/for 共用一层；acquire 复用槽，禁止每轮 root_push
    struct LoopSpillPool {
        std::vector<std::string> slots;
        size_t next = 0;
        size_t highWater = 0; /* 用过的最大 next */
        std::vector<size_t> scopeStack;  /* 每层 while/for 的 next 基线 */
        std::vector<size_t> stickyStack; /* recycle 不低于栈顶（保护 for.seq 等） */
        /* enter 时 sticky.size()：leave 只卸本层 pin，不剥外层 */
        std::vector<size_t> stickyEnterStack;
        /* hoist pin 后 sticky.size()：cond 只卸本层条件 pin
         * （旧 size>1 在嵌套 while 生成期误清外层 junk 池槽 → AV） */
        std::vector<size_t> stickyFloorStack;
    };
    std::vector<LoopSpillPool> loopSpillPools_;
    int loopSpillDepth_ = 0;
    void enterLoopSpillScope();
    void leaveLoopSpillScope();
    void pinLoopSpillCheckpoint();
    void markLoopSpillStickyFloor(); /* hoist pin 之后调用 */
    void recycleLoopSpillSlots();
    void unpinLoopSpillCheckpoint();
    void clearLoopSpillSlots();
    std::string acquireLoopGcSlot(const std::string& nameHint);
    /* GC 操作数跨后续 genExpr/safepoint：spill 后 reload 到 v.ir */
    void rootGcOperand(Value& v);

    // 可见性校验：当前上下文能否访问 ownerClass 中具有 vis 的成员。
    // 规则：
    //   public    -> 任意位置可访问
    //   internal  -> 同包（importPath 相同；顶层函数也按当前单元包判断）
    //   private   -> 仅声明该成员的类内部可访问
    //   protected -> 声明类及其子类内部可访问
    template <class Vis>
    bool canAccessMember(Vis vis, const std::string& ownerClass) const {
        if (vis == Vis::Public) return true;
        if (vis == Vis::Internal) {
            auto it = classes_.find(ownerClass);
            if (it == classes_.end() || !it->second) return false;
            const ClassInfo* owner = it->second.get();
            // 泛型实例：包随模板
            if (owner->isGenericInstance()) {
                auto tit = classes_.find(owner->instanceOf);
                if (tit != classes_.end() && tit->second)
                    owner = tit->second.get();
            }
            return owner->importPath == currentImportPath_;
        }
        if (!currentClass_) return false;
        if (vis == Vis::Private)   return currentClass_->name == ownerClass;
        if (vis == Vis::Protected) return currentClass_->isSubclassOf(ownerClass);
        return true;
    }

    static const char* visName(FieldInfo::Vis v) {
        switch (v) {
            case FieldInfo::Vis::Private:   return "private";
            case FieldInfo::Vis::Protected: return "protected";
            case FieldInfo::Vis::Internal:  return "internal";
            case FieldInfo::Vis::Public:    break;
        }
        return "public";
    }
    static const char* visName(MethodInfo::Vis v) {
        switch (v) {
            case MethodInfo::Vis::Private:   return "private";
            case MethodInfo::Vis::Protected: return "protected";
            case MethodInfo::Vis::Internal:  return "internal";
            case MethodInfo::Vis::Public:    break;
        }
        return "public";
    }

    // 生成函数体公共流程（参数绑定 + 语句 + 收尾 return）
    void genFunctionBody(HaoLangParser::BlockContext* body,
                         const std::vector<std::string>& paramNames,
                         const std::vector<TypePtr>& paramTypes,
                         const TypePtr& returnType,
                         bool isMain,
                         bool hasThis,
                         const std::string& thisClassName,
                         antlr4::ParserRuleContext* declCtx,
                         const std::string& declName);

    // 解析类型标注
    TypePtr resolveType(HaoLangParser::TypeContext* t);

    // 报错
    void error(antlr4::ParserRuleContext* ctx, const std::string& msg);
    void error(antlr4::Token* tok, const std::string& msg);

    // 当前基本块是否已被终结（ret/br 之后不应再发射指令）
    bool blockTerminated_ = false;

    // ---------- 空安全 smart cast（v0.33 / 将就债 E）----------
    //  if (x != null) / while (x != null) 等分支内，局部变量按非空类型使用。
    //  帧栈：函数体一层 + 各 then/else/while 体一层；赋值使该名失效。
    std::vector<std::map<std::string, TypePtr>> smartCastStack_;
    void pushSmartCastFrame();
    void popSmartCastFrame();
    void addSmartCast(const std::string& name, TypePtr nonNull);
    void invalidateSmartCast(const std::string& name);
    TypePtr lookupSmartCast(const std::string& name) const;
    // 解析 x != null / x == null（可带括号）；非此形态返回空 name
    struct NullCheckFact { std::string name; bool isNotNull = false; };
    NullCheckFact analyzeNullCheck(antlr4::tree::ParseTree* expr);
    // 装箱可空 → 非空底层值（无 panic；调用方已保证非 null）
    Value unboxNullableKnown(const Value& base, const TypePtr& nonNull);

    // ---------- 循环上下文 ----------
    //  break 跳到 breakLabel，continue 跳到 continueLabel。
    //  用栈支持嵌套循环。
    struct LoopContext {
        std::string breakLabel;
        std::string continueLabel;
        int tryDepth = 0;   // 进入循环时的 tryStack_ 大小
        /* while：continue 前 clear 本层 spill；for：continue 走 step 的 recycle */
        bool clearSpillOnContinue = true;
    };
    std::vector<LoopContext> loops_;

    // while/for 体内 var 提升：alloca+root_push 一次，每轮只 store（防 shadow 假活）
    struct HoistedLocal {
        std::string addr;
        TypePtr type;
        bool boxed = false;
    };
    std::unordered_map<HaoLangParser::VarDeclContext*, HoistedLocal> loopHoisted_;
    void hoistVarDeclsInLoopBody(HaoLangParser::StatementContext* body);
    void clearHoistedGcSince(
        const std::unordered_map<HaoLangParser::VarDeclContext*, HoistedLocal>& saved);
    TypePtr peekVarDeclType(HaoLangParser::VarDeclContext* vd);
    bool inLoopSpillPool() const { return !loopSpillPools_.empty(); }

    // ---------- try / finally 清理链 ----------
    //  每个 try（无论是否有 finally）都有一个 cleanup 块，负责：
    //    1. 调用 hao_try_end 弹出运行时异常帧（必须在 finally 之前，
    //       这样 finally 中再 throw 会跳到外层 try 而非死循环回本帧）；
    //    2. 执行 finally 体（若有）；
    //    3. 按 unwind reason 分派：正常结束、return、break、continue、rethrow。
    //
    //  reason 与待返回值存放在函数级 alloca 中，使多层 finally 能串联：
    //  内层 finally 执行完若 reason 仍非 0，就跳到外层 cleanup，直到最外层
    //  才真正执行 ret / br 循环目标 / hao_rethrow。
    struct TryContext {
        std::string cleanupLabel;   // cleanup 块入口（pop + finally + 分派）
        std::string endLabel;       // try 正常结束后的汇聚点
    };
    std::vector<TryContext> tryStack_;

    // 函数级异常/清理状态（genFunctionBody 中分配）
    std::string unwindReasonAddr_;   // i32*：0=正常 1=return 2=break 3=continue 4=rethrow
    std::string unwindRetAddr_;      // i64*：return 值或异常对象（按位存放）
    std::string unwindStopAddr_;     // i32*：break/continue 只清理该深度以上的 try
    std::string unwindGcRootAddr_;   // ptr*：与 unwindRet 同步的 GC 指针根（shadow）
    int catchDepth_ = 0;             // >0 表示正在生成 catch 体（throw 走 IR 展开而非 longjmp）
    int tryCounter_ = 0;             // 每个 try 语句唯一编号，避免标签重名

    // 从 try/catch 体中"离开"：把原因写入函数级槽位，跳到最近的 cleanup；
    // 没有 try 包裹时直接执行真正的 return/break/continue。
    void emitUnwind(int reason, const Value& retVal = Value());
    // 把一个值按其类型转成 i64 存入 unwindRetAddr_（GC 指针同时写入 unwindGcRootAddr_）
    void storeUnwindRet(const Value& v);
    // 从 unwindRetAddr_ 读出并转回目标类型，发射 ret
    void emitUnwindRet();

    IREmitter em_;
    IROps ops_{em_};   // 须紧随 em_；通用指令收口（I3 !dbg 挂此层）
    SymbolTable syms_;
    DiagnosticEngine& diags_;

    // 当前函数的返回类型，用于校验 return
    TypePtr currentReturn_ = Type::makeUnit();
    bool sawReturn_ = false;

    // 是否正在生成 main：其 IR 返回类型固定为 i32（C 入口约定）
    bool inMain_ = false;

    // v0.54：函数入口 shadow 水位寄存器（空=本函数未启用精确根）
    std::string gcRootWm_;

    // v0.42：hao test —— 非 harness 的业务 main 跳过登记与 codegen
    bool testMode_ = false;

    // main(args: [String]) 时，由 genFunction 从 argc/argv 构造好的 args 数组
    // 寄存器名。非空（且正在生成 main 首个参数）时，参数绑定直接用它，
    // 不再引用不存在的 %args.arg 参数。
    std::string mainArgsIR_;

    // ---------- 类上下文 ----------
    // 正在生成方法体时，记录所属类，用于解析 this 与裸字段名
    ClassInfoPtr currentClass_;
    std::string thisAddr_;      // this 指针所在的栈地址

    // 是否正在生成构造函数体（super(args) 仅允许在构造器内调用）
    bool inConstructor_ = false;

    // ---------- 包 / import 上下文 ----------
    //  正在处理的编译单元的包前缀（main 为 ""，否则如 "calc$"、"util$strings$"）。
    //  所有非 main 包的顶层符号内部名都此前缀，避免跨包同名冲突。
    std::string currentPkgPrefix_;
    std::string currentImportPath_;     // 当前单元的 importPath，如 "calc"
    std::string currentUnitPath_;       // 当前单元源文件路径（报错用）

    std::vector<Import> currentImports_;

    // importPath -> 该包各源文件合并后的 import 列表（泛型实例化时恢复，
    // 否则 Map 等跨包调用 hashOf 会因 currentImports_ 仍是上一单元而失败）
    std::map<std::string, std::vector<Import>> importsByPath_;

    // 按包前缀恢复 currentImports_ / currentImportPath_
    void restoreImportsForPkgPrefix(const std::string& pkgPrefix);

    // importPath -> (短名 -> 内部全限定名)，收集阶段填充，供跨包解析
    std::map<std::string, std::map<std::string, std::string>> pkgExports_;

    // 函数重载表：内部全名（含包前缀）-> 同名函数列表（按收集顺序）。
    // 一个名字下第一个符号仍照常进符号表（供 lookup / 判断"是函数"），
    // 其余同名校重载只进此表；调用点按实参个数/类型选最佳匹配。
    std::map<std::string, std::vector<SymbolPtr>> overloads_;

    // Go 式 init()：各包顶层 func init()。收集时按 unit 顺序（入口包在前），
    // 生成 main 前反序调用（导入的包 init 先于依赖它的包，对齐 Go）。存 irName。
    std::vector<std::string> initCalls_;
    // 每包可有多个 init()，用递增后缀区分 IR 符号名（@init$0、@init$1…）
    int initCounter_ = 0;

    // 类静态 GC 指针字段全局名（@Class.field），main 前注册为根槽。
    std::vector<std::string> staticGcRootGlobals_;

    // 所有 extern 函数 @link("...") 声明的外部链接库（去重，追加到链接命令）。
    std::vector<std::string> linkLibs_;

    // 按实参类型/个数从候选集选最佳重载：
    //   0 分 = 精确类型匹配，1 分 = 可赋值（如 Int -> Double 提升）。
    //   a) 参数个数不匹配的候选淘汰；
    //   b) 取总分最小的候选；唯一则返回它；
    //   c) 无匹配或多候选并列最低分则报错并返回 nullptr。
    SymbolPtr selectOverload(const std::vector<SymbolPtr>& cands,
                             const std::vector<Value>& args,
                             antlr4::ParserRuleContext* ctx,
                             const std::string& displayName);

    // 类静态方法按签名重载：计分规则同 selectOverload。
    const MethodInfo* selectStaticOverload(
        const std::vector<const MethodInfo*>& cands,
        const std::vector<Value>& args,
        antlr4::ParserRuleContext* ctx,
        const std::string& displayName);

    // 静态方法 IR 名：首个重载无后缀，后续 @Class.m$Int$Long …
    static std::string staticMethodIRName(const std::string& classIRName,
                                          const std::string& methodName,
                                          const std::vector<TypePtr>& params,
                                          bool needsSuffix);

    // 为函数重载生成安全的 IR 符号名后缀（基础类型/类/泛型/数组/函数类型
    // 均可；'$' 与字母数字对 IR 符号名合法，避免 clang 的重复定义报错）。
        // Instance method IR name (same mangling as static overloads)
    static std::string instanceMethodIRName(const std::string& classIRName,
                                           const std::string& methodName,
                                           const std::vector<TypePtr>& params,
                                           bool needsSuffix) {
        return staticMethodIRName(classIRName, methodName, params, needsSuffix);
    }

static std::string overloadSuffix(const TypePtr& t);

    // 由 import 路径算内部前缀：main 为 ""，否则 '/' 或 '.' 换 '$' 后加 '$'
    // （demo/web、demo.web → demo$web$）
    static std::string pkgPrefixOf(const std::string& importPath) {
        if (importPath.empty()) return "";
        std::string p;
        for (char c : importPath)
            p += (c == '/' || c == '.' ? '$' : c);
        return p + "$";
    }

    // 包前缀还原为 importPath：collections$ → collections；util$strings$ → util/strings
    static std::string importPathFromPrefix(const std::string& prefix) {
        if (prefix.empty()) return "";
        std::string p = prefix;
        if (!p.empty() && p.back() == '$') p.pop_back();
        for (char& c : p) if (c == '$') c = '/';
        return p;
    }

    // 名字解析：依次查局部作用域、当前包顶层（带前缀）、通配导入包。
    // 若唯一解析到一个导入符号，返回它并设置 outImported=true；
    // 多个通配包同名则报错并返回 nullptr。
    SymbolPtr resolveTopLevelName(const std::string& name,
                                  antlr4::ParserRuleContext* ctx,
                                  bool* outImported = nullptr);

    // 解析 "pkg.member" 形式的限定名：pkg 是 import 别名/末段名。
    // 返回被导入包中该符号的内部符号；memberIsType 区分要找的是类型还是函数。
    SymbolPtr resolveQualifiedName(const std::string& pkgAlias,
                                   const std::string& member,
                                   antlr4::ParserRuleContext* ctx);

    // 判断内部符号 ownerClass 是否可由当前包访问（跨包 private 拒绝）
    bool canAccessTopLevel(const std::string& ownerImportPath, bool isPrivate) const;

    // 把限定类型名（如 "Circle" / "shapes.Circle"）解析为内部全限定名
    // （如 "shapes$Circle"），找不到返回空串。isIface 输出是否为接口。
    std::string resolveTypeQualifiedName(HaoLangParser::QualifiedNameContext* qn,
                                         bool* isIface = nullptr);

    // 已登记的类信息，按名字索引
    std::map<std::string, ClassInfoPtr> classes_;

    // 已登记的接口信息，按名字索引
    std::map<std::string, InterfaceInfoPtr> interfaces_;

    // 类型判定列表的 IR 名：类型名 -> @TypeName.typeids
    // 内容是该类型及其所有子类（或实现类）的虚表指针，NULL 结尾
    std::map<std::string, std::string> typeIdLists_;
};

} // namespace hao
