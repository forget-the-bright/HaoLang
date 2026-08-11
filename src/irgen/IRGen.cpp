// ============================================================
//  HaoLang IR 生成器实现
// ============================================================

#include "irgen/IRGen.h"
#include "util/StringUtil.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <set>

namespace hao {

// ============================================================
//  错误报告
// ============================================================

void IRGen::error(antlr4::ParserRuleContext* ctx, const std::string& msg) {
    auto* tok = ctx->getStart();
    diags_.error(currentUnitPath_, tok->getLine(),
                 tok->getCharPositionInLine(), msg);
}

void IRGen::error(antlr4::Token* tok, const std::string& msg) {
    diags_.error(currentUnitPath_, tok->getLine(),
                 tok->getCharPositionInLine(), msg);
}

SourceLoc IRGen::locFrom(antlr4::ParserRuleContext* ctx) const {
    SourceLoc loc;
    loc.file = currentUnitPath_;
    if (!ctx) return loc;
    auto* tok = ctx->getStart();
    if (!tok) return loc;
    loc.line = static_cast<unsigned>(tok->getLine());
    loc.col = static_cast<unsigned>(tok->getCharPositionInLine() + 1);
    return loc;
}

SourceLoc IRGen::locFrom(antlr4::Token* tok) const {
    SourceLoc loc;
    loc.file = currentUnitPath_;
    if (!tok) return loc;
    loc.line = static_cast<unsigned>(tok->getLine());
    loc.col = static_cast<unsigned>(tok->getCharPositionInLine() + 1);
    return loc;
}

void IRGen::emitRuntimeSrcLoc(const SourceLoc& loc) {
    if (!loc.valid()) {
        emitCallVoid("@hao_dbg_clear_src_loc", "");
        return;
    }
    std::string fileC = em_.internString(loc.file.empty() ? "?" : loc.file);
    emitCallVoid("@hao_dbg_set_src_loc",
                 "ptr " + fileC + ", i32 " + std::to_string((int)loc.line) +
                     ", i32 " + std::to_string((int)loc.col));
}

void IRGen::pinRuntimeCallSite(antlr4::ParserRuleContext* ctx) {
    if (!ctx) return;
    emitRuntimeSrcLoc(locFrom(ctx));
}

void IRGen::emitRuntimePushFrame(const SourceLoc& loc) {
    std::string fileC =
        em_.internString(loc.valid() && !loc.file.empty() ? loc.file : "?");
    int line = loc.valid() ? (int)loc.line : 0;
    int col = loc.valid() ? (int)loc.col : 0;
    emitCallVoid("@hao_dbg_push_frame",
                 "ptr " + fileC + ", i32 " + std::to_string(line) + ", i32 " +
                     std::to_string(col));
}

void IRGen::emitRuntimePopFrame() {
    emitCallVoid("@hao_dbg_pop_frame", "");
}

// ============================================================
//  类型解析
// ============================================================

std::string IRGen::resolveTypeQualifiedName(HaoLangParser::QualifiedNameContext* qn,
                                            bool* isIface) {
    auto ids = qn->IDENT();
    if (ids.empty()) return "";

    auto lookupInternal = [&](const std::string& internal) -> std::string {
        if (classes_.count(internal)) { if (isIface) *isIface = false; return internal; }
        if (interfaces_.count(internal)) { if (isIface) *isIface = true; return internal; }
        return "";
    };

    if (ids.size() == 1) {
        std::string shortName = ids[0]->getText();
        // 当前包
        std::string r = lookupInternal(currentPkgPrefix_ + shortName);
        if (!r.empty()) return r;
        // 通配导入包
        for (const auto& im : currentImports_) {
            if (!im.wildcard) continue;
            auto it = pkgExports_.find(im.importPath);
            if (it == pkgExports_.end()) continue;
            auto eit = it->second.find(shortName);
            if (eit != it->second.end()) return lookupInternal(eit->second);
        }
        return "";
    }

    // 限定名 pkg.Type：第一段是 import 别名
    std::string alias = ids[0]->getText();
    for (const auto& im : currentImports_) {
        if (im.alias != alias) continue;
        // 支持 a.b.C：取最后一段为类型名，前面拼回 importPath
        std::string member = ids.back()->getText();
        auto it = pkgExports_.find(im.importPath);
        if (it == pkgExports_.end()) break;
        auto eit = it->second.find(member);
        if (eit != it->second.end()) return lookupInternal(eit->second);
        break;
    }
    return "";
}

TypePtr IRGen::resolveType(HaoLangParser::TypeContext* t) {
    if (!t) return Type::makeUnknown();

    TypePtr base = Type::makeUnknown();
    auto* bt = t->baseType();

    if (auto* named = dynamic_cast<HaoLangParser::NamedTypeContext*>(bt)) {
        auto* qn = named->qualifiedName();
        std::string tn = qn->getText();

        // ---- 预置 Action/Func 泛型函数类型别名（v0.20.0，C# 风格）----
        // 全局可用（对标 Object 根父类），无需 import。
        //   Action          = ()->Unit
        //   Action<T1,...>  = (T1,...)->Unit
        //   Func<...T, R>   = (...T)->R   （最后一个类型实参是返回类型）
        if (qn->IDENT().size() == 1 && (tn == "Action" || tn == "Func")) {
            auto* ta = named->typeArgs();
            std::vector<TypePtr> args;
            if (ta)
                for (auto* at : ta->type()) args.push_back(resolveType(at));
            if (tn == "Action") {
                base = Type::makeFunc(std::move(args), Type::makeUnit());
            } else { // Func
                if (args.empty()) {
                    error(t, "Func 至少需要一个类型实参（最后一个为返回类型），如 Func<Int,String>");
                    base = Type::makeUnknown();
                } else {
                    TypePtr ret = args.back();
                    args.pop_back();
                    base = Type::makeFunc(std::move(args), ret);
                }
            }
            goto resolvedNamed;
        }

        // ---- delegate 命名函数类型别名（v0.19.0）----
        if (qn->IDENT().size() == 1) {
            auto dit = delegates_.find(currentPkgPrefix_ + tn);
            if (dit == delegates_.end()) dit = delegates_.find(tn);
            if (dit != delegates_.end()) {
                base = dit->second;
                goto resolvedNamed;
            }
        }

        // ---- 类型参数优先 ----
        if (currentTypeParams_.count(tn)) {
            base = Type::makeTypeParam(tn);
            base = substType(base, currentSubst_);
        } else if (qn->IDENT().size() == 1) {
            // 内建类型（主名 / i32 / int 等别名）
            if (TypePtr bt = typeFromName(tn)) {
                base = bt;
            } else {
                bool iface = false;
                std::string internal = resolveTypeQualifiedName(qn, &iface);
                if (!internal.empty()) {
                    base = iface ? (TypePtr)Type::makeInterface(internal)
                                 : (TypePtr)Type::makeClass(internal);
                } else {
                    base = Type::makeClass(tn);
                }
            }
        } else {
            // 多段限定名 pkg.Type
            bool iface = false;
            std::string internal = resolveTypeQualifiedName(qn, &iface);
            if (!internal.empty()) {
                base = iface ? (TypePtr)Type::makeInterface(internal)
                             : (TypePtr)Type::makeClass(internal);
            } else {
                base = Type::makeClass(tn);
            }
        }

        // ---- 泛型实参 Box<Int> ----
        if (auto* ta = named->typeArgs()) {
            std::vector<TypePtr> args;
            for (auto* at : ta->type()) args.push_back(resolveType(at));
            if (base->kind == TypeKind::Class) {
                if (!base->hasTypeParam())
                    instantiateClass(base->className, args, t);
                base = Type::makeClass(base->className, args);
            } else if (base->kind == TypeKind::Interface) {
                base->typeArgs = args;
            }
        }
    } else if (auto* arr = dynamic_cast<HaoLangParser::ArrayTypeContext*>(bt)) {
        base = Type::makeArray(resolveType(arr->type()));
    } else if (auto* ft = dynamic_cast<HaoLangParser::FuncTypeContext*>(bt)) {
        // 函数类型：(T1, T2) -> R
        std::vector<TypePtr> ps;
        if (auto* tl = ft->typeList())
            for (auto* tc : tl->type())
                ps.push_back(resolveType(tc));
        base = Type::makeFunc(std::move(ps), resolveType(ft->type()));
    }

    resolvedNamed:
    // 后缀 ? 表示可空
    if (t->QUESTION()) {
        // 文档约定：不支持 Func?/Action?（调用路径无空检查，易空指针）
        if (base->kind == TypeKind::Func) {
            error(t, "不支持可空函数类型（Func?/Action?），请用非空 Func 或可空包装对象");
            return Type::makeUnknown();
        }
        base = base->asNullable();
    }
    return base;
}

// ============================================================
//  顶层：编译单元（支持多文件 / 跨包）
// ============================================================

namespace {
// 从一个编译单元的语法树解析其 import 列表
std::vector<IRGen::Import> parseImports(HaoLangParser::CompilationUnitContext* unit) {
    std::vector<IRGen::Import> result;
    for (auto* imp : unit->importDecl()) {
        IRGen::Import im;
        im.importPath = imp->qualifiedName()->getText();   // 点分，如 "util.strings"
        for (char& c : im.importPath) if (c == '.') c = '/';
        im.wildcard = imp->STAR() != nullptr;
        if (imp->AS()) {
            im.alias = imp->IDENT()->getText();
        } else {
            // 默认可用名 = 末段（util.strings -> strings）
            auto ids = imp->qualifiedName()->IDENT();
            im.alias = ids.back()->getText();
        }
        result.push_back(std::move(im));
    }
    // 隐式预导入 fmt（v0.9.0 起 fmt 是 .hao 标准库包，不再需要 import 即可用）。
    // 若无显式 import fmt，注入一个 alias="fmt" 的条目，使 fmt.println 走跨包
    // 限定调用分支（特判 1b）解析到 stdlib/src/fmt/fmt.hao 的重载。
    bool hasFmt = false;
    for (const auto& im : result)
        if (im.importPath == "fmt") { hasFmt = true; break; }
    if (!hasFmt) {
        IRGen::Import im;
        im.importPath = "fmt";
        im.alias = "fmt";
        result.push_back(std::move(im));
    }
    // 隐式预导入 object.*（v0.15.0，Object 根父类）。
    // 用通配导入使裸名 `Object` 在所有包（含 stdlib 包）都能解析为 Object 类型，
    // 供类型标注（如 equals(o: Object)）与继承默认基类使用。
    // 注意：仅当「已存在 object 的通配导入」时才跳过；若用户只写了限定
    // `import object;`，仍应补一个通配导入使裸名 Object 可解析。
    bool hasObjectWildcard = false;
    for (const auto& im : result)
        if (im.importPath == "object" && im.wildcard) { hasObjectWildcard = true; break; }
    if (!hasObjectWildcard) {
        IRGen::Import im;
        im.importPath = "object";
        im.alias = "object";
        im.wildcard = true;
        result.push_back(std::move(im));
    }
    // 隐式预导入 lang.*（v0.22.0，基础类型包装 Integer/Boolean/String/Double）。
    // 使 Integer.valueOf / String.substring 等无需手写 import。
    // 裸名类型标注仍优先内建 Int/String/…；包装类型用 lang.String 或推断。
    bool hasLangWildcard = false;
    for (const auto& im : result)
        if (im.importPath == "lang" && im.wildcard) { hasLangWildcard = true; break; }
    if (!hasLangWildcard) {
        IRGen::Import im;
        im.importPath = "lang";
        im.alias = "lang";
        im.wildcard = true;
        result.push_back(std::move(im));
    }
    return result;
}
} // namespace

bool IRGen::declIsPrivate(const std::vector<HaoLangParser::ModifierContext*>& mods) {
    for (auto* m : mods) {
        auto* ctx = dynamic_cast<HaoLangParser::ModifierContext*>(m);
        if (ctx && ctx->PRIVATE()) return true;
    }
    return false;
}

// 顶层声明是否带 extern 修饰（声明外部 C 函数，无函数体）
bool IRGen::declIsExtern(const std::vector<HaoLangParser::ModifierContext*>& mods) {
    for (auto* m : mods) {
        auto* ctx = dynamic_cast<HaoLangParser::ModifierContext*>(m);
        if (ctx && ctx->EXTERN()) return true;
    }
    return false;
}

// 从形如 "hao_println_int" 的 STRING_LIT 词文取出内容（去掉首尾引号、
// 处理常见转义）。用于 extern func f(...) = "c_name"。
static std::string parseStringLitText(const std::string& tok) {
    if (tok.size() < 2) return "";
    std::string s = tok.substr(1, tok.size() - 2);
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char c = s[++i];
            switch (c) {
                case 'n': r += '\n'; break;
                case 't': r += '\t'; break;
                case 'r': r += '\r'; break;
                case '0': r += '\0'; break;
                case '\\': r += '\\'; break;
                case '"': r += '"'; break;
                default: r += c; break;
            }
        } else {
            r += s[i];
        }
    }
    return r;
}

void IRGen::setCurrentUnit(const SourceUnit& u, const std::vector<Import>& imp) {
    currentPkgPrefix_   = pkgPrefixOf(u.importPath);
    currentImportPath_  = u.importPath;
    currentUnitPath_    = u.path;
    currentImports_     = imp;
    diags_.setDefaultFile(u.path);
    if (!u.path.empty() && em_.debugEnabled()) {
        // 仅首次设定：入口单元路径作为模块 DIFile（后续包切换不覆盖）
        // setDebugFile 内部若已有值则保留——见 IREmitter
        em_.setDebugFileIfEmpty(u.path);
    }
    // 确保每个包都有导出表条目（即便没有 public 成员），
    // 这样解析限定名时能区分"包未导入"与"成员不存在/私有"
    pkgExports_[u.importPath];
}

void IRGen::restoreImportsForPkgPrefix(const std::string& pkgPrefix) {
    std::string ip = importPathFromPrefix(pkgPrefix);
    currentImportPath_ = ip;
    auto it = importsByPath_.find(ip);
    if (it != importsByPath_.end())
        currentImports_ = it->second;
    else
        currentImports_.clear();
}

bool IRGen::canAccessTopLevel(const std::string& ownerImportPath,
                              bool isPrivate) const {
    if (!isPrivate) return true;
    // private 仅包内可见
    return ownerImportPath == currentImportPath_;
}

// 解析裸标识符：先查局部作用域（变量/参数/main 包函数），再查当前包顶层，
// 最后查通配导入包。多个通配包含同名时报歧义。
SymbolPtr IRGen::resolveTopLevelName(const std::string& name,
                                     antlr4::ParserRuleContext* ctx,
                                     bool* outImported) {
    // 1. 当前作用域（含 main 包的裸名全局函数，因为它们无前缀）
    if (auto s = syms_.lookup(name)) {
        if (outImported) *outImported = false;
        return s;
    }
    // 2. 当前包的非 main 前缀顶层
    if (!currentPkgPrefix_.empty()) {
        if (auto s = syms_.global()->lookup(currentPkgPrefix_ + name)) {
            if (outImported) *outImported = false;
            return s;
        }
    }
    // 3. 通配导入
    SymbolPtr found;
    std::string foundPkg;
    for (const auto& im : currentImports_) {
        if (!im.wildcard) continue;
        auto it = pkgExports_.find(im.importPath);
        if (it == pkgExports_.end()) continue;
        auto eit = it->second.find(name);
        if (eit == it->second.end()) continue;
        auto s = syms_.global()->lookup(eit->second);
        if (!s) continue;
        if (s->isPrivate) continue;   // 不导出 private
        if (found) {
            if (ctx) error(ctx, "标识符 '" + name + "' 有歧义：同时在包 '" +
                                foundPkg + "' 和 '" + im.importPath + "' 中");
            return nullptr;
        }
        found = s;
        foundPkg = im.importPath;
    }
    if (found && outImported) *outImported = true;
    return found;
}

// 解析 "pkg.member"：pkg 是 import 的别名/末段名
SymbolPtr IRGen::resolveQualifiedName(const std::string& pkgAlias,
                                      const std::string& member,
                                      antlr4::ParserRuleContext* ctx) {
    for (const auto& im : currentImports_) {
        if (im.alias != pkgAlias) continue;
        if (im.wildcard) continue;   // 通配导入不走 pkg.member；交给裸名解析

        auto it = pkgExports_.find(im.importPath);
        if (it == pkgExports_.end()) {
            if (ctx) error(ctx, "找不到导入的包 '" + pkgAlias + "'");
            return nullptr;
        }
        auto eit = it->second.find(member);
        if (eit == it->second.end()) {
            if (ctx) error(ctx, "包 '" + pkgAlias + "' 中没有 '" + member + "'");
            return nullptr;
        }
        auto s = syms_.global()->lookup(eit->second);
        if (s && s->isPrivate) {
            if (ctx) error(ctx, "'" + pkgAlias + "." + member + "' 是私有的");
            return nullptr;
        }
        return s;
    }
    return nullptr;   // 不是已知包别名（可能是对象成员访问，交给上层正常处理）
}

// ------------------------------------------------------------
//  函数重载解析（v0.9.0）
// ------------------------------------------------------------
//  按实参类型/个数从候选集选最佳重载。规则：
//    - 参数个数必须与候选形参个数一致；
//    - 每个实参要么精确类型匹配（0 分），要么可赋值（含 Int->Double 提升，1 分）；
//    - 取总分最小的候选；唯一则返回；
//    - 无匹配或并列最低分则报错并返回 nullptr。
SymbolPtr IRGen::selectOverload(const std::vector<SymbolPtr>& cands,
                                const std::vector<Value>& args,
                                antlr4::ParserRuleContext* ctx,
                                const std::string& displayName) {
    // ---- 1. 过滤出参数个数匹配的候选 ----
    std::vector<SymbolPtr> byArity;
    for (const auto& c : cands)
        if (c->paramTypes.size() == args.size()) byArity.push_back(c);
    if (byArity.empty()) {
        error(ctx, displayName + "：没有接受 " + std::to_string(args.size()) +
                   " 个参数的重载");
        return nullptr;
    }

    // ---- 2. 计算每个候选的匹配分，取最低分 ----
    // 窄化可累计 (Δrank+10)，初值须远大于任意可行总分
    int bestScore = 1000000;
    std::vector<SymbolPtr> best;
    for (const auto& c : byArity) {
        int score = 0;
        bool ok = true;
        for (size_t k = 0; k < args.size(); ++k) {
            if (args[k].type->equals(*c->paramTypes[k])) continue;   // 精确，0 分
            if (!isAssignable(args[k].type, c->paramTypes[k])) {
                ok = false;
                break;
            }
            // 转换代价：拓宽距离；窄化额外加分，避免重载歧义
            TypeKind from = args[k].type->kind;
            TypeKind to = c->paramTypes[k]->kind;
            int fr = Type::numericRank(from), tr = Type::numericRank(to);
            if (fr > 0 && tr > 0) {
                if (fr < tr) score += (tr - fr);
                else if (fr > tr) score += (fr - tr) + 10; // 窄化更贵
                else score += 1; // 同宽有符号/无符号互转，次于精确匹配
            } else {
                score += 1;
            }
        }
        if (!ok) continue;
        if (score < bestScore) {
            bestScore = score;
            best.assign(1, c);
        } else if (score == bestScore) {
            best.push_back(c);
        }
    }

    if (best.empty()) {
        error(ctx, displayName + "：没有与实参类型匹配的重载");
        return nullptr;
    }
    if (best.size() > 1) {
        error(ctx, displayName + "：实参同时匹配多个重载，存在歧义");
        return nullptr;
    }
    return best.front();
}

const MethodInfo* IRGen::selectStaticOverload(
    const std::vector<const MethodInfo*>& cands,
    const std::vector<Value>& args,
    antlr4::ParserRuleContext* ctx,
    const std::string& displayName) {
    std::vector<const MethodInfo*> byArity;
    for (const auto* c : cands)
        if (c && c->paramTypes.size() == args.size()) byArity.push_back(c);
    if (byArity.empty()) {
        error(ctx, displayName + "：没有接受 " + std::to_string(args.size()) +
                   " 个参数的重载");
        return nullptr;
    }
    // 窄化可累计 (Δrank+10)，初值须远大于任意可行总分
    int bestScore = 1000000;
    std::vector<const MethodInfo*> best;
    for (const auto* c : byArity) {
        int score = 0;
        bool ok = true;
        for (size_t k = 0; k < args.size(); ++k) {
            if (!c->paramTypes[k]) { ok = false; break; }
            if (args[k].type->equals(*c->paramTypes[k])) continue;
            if (!isAssignable(args[k].type, c->paramTypes[k])) {
                ok = false;
                break;
            }
            TypeKind from = args[k].type->kind;
            TypeKind to = c->paramTypes[k]->kind;
            int fr = Type::numericRank(from), tr = Type::numericRank(to);
            if (fr > 0 && tr > 0) {
                if (fr < tr) score += (tr - fr);
                else if (fr > tr) score += (fr - tr) + 10;
                else score += 1; // 同宽有符号/无符号互转，次于精确匹配
            } else {
                score += 1;
            }
        }
        if (!ok) continue;
        if (score < bestScore) {
            bestScore = score;
            best.assign(1, c);
        } else if (score == bestScore) {
            best.push_back(c);
        }
    }
    if (best.empty()) {
        error(ctx, displayName + "：没有与实参类型匹配的重载");
        return nullptr;
    }
    if (best.size() > 1) {
        error(ctx, displayName + "：实参同时匹配多个重载，存在歧义");
        return nullptr;
    }
    return best.front();
}

std::string IRGen::staticMethodIRName(const std::string& classIRName,
                                      const std::string& methodName,
                                      const std::vector<TypePtr>& params,
                                      bool needsSuffix) {
    std::string ir = "@" + classIRName + "." + methodName;
    if (!needsSuffix) return ir;
    ir += "$";
    for (size_t k = 0; k < params.size(); ++k) {
        if (k) ir += "$";
        ir += overloadSuffix(params[k]);
    }
    return ir;
}

// 为函数重载生成安全的 IR 符号名后缀。基础类型返回固定名；数组/函数/类型
// 参数递归展开（monoName 对数组返回空串、对函数类型含括号，不能直接用）。
std::string IRGen::overloadSuffix(const TypePtr& t) {
    switch (t->kind) {
        case TypeKind::SByte: case TypeKind::Byte:
        case TypeKind::Short: case TypeKind::UShort:
        case TypeKind::Int:   case TypeKind::UInt:
        case TypeKind::Long:  case TypeKind::ULong:
        case TypeKind::UIntPtr:
        case TypeKind::Float: case TypeKind::Double:
        case TypeKind::Bool:  case TypeKind::Char: case TypeKind::String:
        case TypeKind::Unit:
            return Type::kindDisplayName(t->kind);
        case TypeKind::Array:
            return "Arr" + (t->elem ? overloadSuffix(t->elem) : std::string("_"));
        case TypeKind::Func: {
            std::string s = "Fn";
            for (const auto& p : t->params) { s += "_"; s += overloadSuffix(p); }
            s += "_";
            s += overloadSuffix(t->elem ? t->elem : Type::makeUnit());
            return s;
        }
        case TypeKind::TypeParam:
            return "T_" + (t->className.empty() ? std::string("_") : t->className);
        default:
            return t->className.empty() ? std::string("_") : t->className;
    }
}

std::string IRGen::generate(const std::vector<SourceUnit>& units) {
    // I3：模块 DIFile 钉入口单元（后续 setCurrentUnit 不覆盖）
    if (!units.empty() && !units[0].path.empty())
        em_.setDebugFileIfEmpty(units[0].path);

    // 预先为每个单元解析 imports（生成阶段也要用）
    std::vector<std::vector<Import>> allImports;
    allImports.reserve(units.size());
    importsByPath_.clear();
    for (const auto& u : units) {
        auto imps = parseImports(u.tree);
        // 同包多文件：合并 import（后文件追加；通配/别名去重不严格，够用）
        auto& slot = importsByPath_[u.importPath];
        slot.insert(slot.end(), imps.begin(), imps.end());
        allImports.push_back(std::move(imps));
    }

    // 分阶段处理，使声明顺序、跨包引用都不影响可见性：
    //   阶段 A：登记所有包的类型名（类/接口/泛型模板），并填充 pkgExports_。
    //           此时不解析成员，因为成员类型可能引用其他包（尚未登记）。
    //   阶段 B：解析接口方法、类字段/方法、顶层函数签名（所有类型名已就位）。
    //   然后：继承解析、接口实现校验、虚表、代码生成。
    for (size_t i = 0; i < units.size(); ++i) {
        setCurrentUnit(units[i], allImports[i]);
        registerInterfaceNames(units[i]);
        registerClassNames(units[i]);
    }

    for (size_t i = 0; i < units.size(); ++i) {
        setCurrentUnit(units[i], allImports[i]);
        collectInterfaceMembers(units[i]);
        collectClassMembers(units[i]);
        collectFunctionSignatures(units[i]);
        collectGenericFunctions(units[i]);
        collectDelegates(units[i]);
    }

    // init() 依赖顺序：收集时是入口包在前、其依赖在后；反序即依赖在前。
    // 使 @main 启动时先跑导入包的 init()，再跑 main 自己的（对齐 Go）。
    std::reverse(initCalls_.begin(), initCalls_.end());

    if (diags_.hasErrors()) return {};

    resolveInheritance();
    // v0.18.0 起拆分为：先分配槽位（模板/非泛型接口 + 类虚方法）→ 实例化泛型接口
    // （复制模板槽位）→ 生成代码 → 全部泛型类实例就绪后填表 → 发射 vtable。
    assignVtableSlots();
    // 虚表就绪后为每类型合成 static Class（须在 markStaticInitFlags / 代码生成前）
    synthesizeClassStaticFields();
    instantiateAllGenericInterfaces();
    // 第 1 遍 typeids：为当前（非泛型）类生成，供 genUnitTopLevel 的 is/as
    // 编译期检查使用（原管线 emitTypeIdLists 在代码生成之前）。
    emitTypeIdLists();

    if (diags_.hasErrors()) return {};

    // 在生成任何函数体之前标记 hasStaticInit。
    // 否则「函数写在类前面」时访问静态字段会漏调 ensureInit
    // （@Class.sfn 仍为 null → 调用崩溃）。String/lambda 等非常量默认值同理。
    markStaticInitFlags();

    for (size_t i = 0; i < units.size(); ++i) {
        setCurrentUnit(units[i], allImports[i]);
        genUnitTopLevel(units[i]);
    }

    // 泛型实例/泛型函数的代码最后统一生成。泛型方法实例（v0.9.0）又可能
    // 触发新的泛型类/函数实例，故循环到三者全部稳定为止。
    // 泛型类实例在 genPendingInstantiations 中创建，其接口实现随之 instantiateInterface。
    for (;;) {
        genPendingInstantiations();
        genPendingFunctionInstances();
        if (pendingMethodInstances_.empty()) break;
        genPendingMethodInstances();
    }

    // 全部类（含泛型实例）就绪后填 vtable 并发射（必须在泛型实例循环之后，
    // 否则泛型实例实现的接口槽位/虚表缺失）。
    fillVtableEntries();
    emitTypeIdLists();   // 第 2 遍：泛型实例（List$Int 等）的 typeids
    emitVTables();
    emitClassMeta();     // v0.19.0：反射类型元数据（依赖虚表已生成）
    emitStaticGcRootRegistration(); // 静态 String/对象字段 → GC 根槽

    // 为所有 extern 函数输出 declare（外部 C 符号，如运行时/系统库）。
    // 即使未被引用也无害（clang 会忽略未引用的 declare）；若声明了不存在
    // 的 C 符号，会在链接期报错，这是期望行为。所有 extern 符号在阶段 B
    // 已收集到全局符号表，此时一次性输出。
    // 同 C 符号多名（如 ptrOfStr/ptrOf → hao_reflect_ptrtoint）只 declare 一次。
    std::set<std::string> emittedExternLinks;
    for (const auto& name : syms_.global()->declOrder()) {
        auto s = syms_.global()->lookupLocal(name);
        if (!s || s->kind != SymbolKind::Function || !s->isExtern) continue;
        // 与内建运行时声明（IREmitter::runtimeDecls）去重，避免重复 declare
        if (em_.isRuntimeDeclared(s->linkName)) continue;
        if (!emittedExternLinks.insert(s->linkName).second) continue;
        std::string d = "declare " + s->returnType->llvmType() + " @" + s->linkName + "(";
        for (size_t i = 0; i < s->paramTypes.size(); ++i) {
            if (i) d += ", ";
            d += s->paramTypes[i]->llvmType();
        }
        d += ")";
        em_.addGlobal(d);
    }

    // 必须有 main（main 包不加前缀，故仍查 "main"）
    auto mainSym = syms_.global()->lookup("main");
    if (!mainSym || mainSym->kind != SymbolKind::Function) {
        diags_.error(currentUnitPath_.empty() ? std::string("?") : currentUnitPath_,
                     1, 0, "未找到程序入口函数 main()");
    }

    if (diags_.hasErrors()) return {};
    return em_.finish();
}

void IRGen::genUnitTopLevel(const SourceUnit& u) {
    auto* unit = u.tree;
    for (auto* decl : unit->topLevelDecl()) {
        if (auto* fn = decl->funcDecl()) {
            // 泛型函数模板不直接生成代码，实例按需生成
            if (fn->typeParams()) continue;
            // extern 函数没有函数体，只在被引用时输出 declare
            if (declIsExtern(fn->modifier())) continue;
            genFunction(fn);
        } else if (auto* cls = decl->classDecl()) {
            // 泛型模板不直接生成代码，只在被实例化时按实参产出
            auto ci = lookupClass(currentPkgPrefix_ + cls->IDENT()->getText());
            if (ci && ci->isGenericTemplate()) continue;
            genClass(cls);
        } else if (auto* ad = decl->annotationDecl()) {
            // @interface：无方法体，但须发射合成 static Class + ensureInit
            auto ci = lookupClass(currentPkgPrefix_ + ad->IDENT()->getText());
            if (ci && ci->isAnnotation) {
                emitStaticFieldGlobals(ci);
                genStaticConstructor(ci);
            }
        } else if (decl->interfaceDecl()) {
            // 接口本身不产生代码，只贡献虚表布局
        } else if (auto* ed = decl->enumDecl()) {
            auto ci = lookupClass(currentPkgPrefix_ + ed->IDENT()->getText());
            if (ci) genEnum(ed, ci);
        } else if (auto* fd = decl->fieldDecl()) {
            error(fd, "当前版本尚不支持全局变量");
        }
    }
}

// ============================================================
//  类型可赋值性 / 公共父类型
// ------------------------------------------------------------
//  类层次相关的收集与代码生成在 IRGenClass.cpp；这里保留表达式生成
//  频繁用到的类型判断与多态数组公共类型推断。
// ============================================================

bool IRGen::isAssignable(const TypePtr& from, const TypePtr& to) {
    if (!from || !to) return false;

    // null 字面量可赋给任意可空类型
    if (from->isNull() && to->nullable) return true;

    // T -> T?：非空值赋给可空变量（引用类型直接通过，值类型需装箱，
    // 由 coerce 负责）
    if (!from->nullable && to->nullable && from->sameShape(*to)) return true;

    // 先用不依赖符号表的通用规则
    if (from->assignableTo(*to)) return true;

    if (from->nullable && !to->nullable) return false;

    // 子类 -> 父类：对象布局保证父类字段在前、虚表槽位一致，
    // 因此无需任何指针调整。泛型类型需先转为单态化实例名。
    if (from->kind == TypeKind::Class && to->kind == TypeKind::Class &&
        !from->isNull()) {
        auto ci = classOfType(from);
        auto tc = classOfType(to);
        return ci && tc && ci->isSubclassOf(tc->name);
    }

    // 类 -> 它（或其祖先）实现的接口：运行时都是 ptr，
    // 虚表指针已在对象内，无需转换。泛型接口按实例名匹配
    //（Iterable<Int> 记为 Iterable$Int，类 interfaceNames 存实例名）。
    if (from->kind == TypeKind::Class && to->kind == TypeKind::Interface) {
        auto ci = classOfType(from);
        std::string iname = to->typeArgs.empty()
            ? to->className
            : Type::makeInterface(to->className, to->typeArgs)->monoName();
        return ci && ci->implementsInterface(iname);
    }
    return false;
}

// ------------------------------------------------------------
//  找出一组值的公共父类型
// ------------------------------------------------------------
//  用于多态数组的元素类型推断：
//    [new Dog(..), new Cat(..)]     -> 公共基类 Animal
//    [new Circle(..), new Rect(..)] -> 公共接口 Shape
//  优先取公共基类（更具体），其次取公共接口；都没有则返回空串。
std::string IRGen::findCommonSupertype(const std::vector<Value>& vals) {
    if (vals.empty()) return {};

    // ---- 先尝试公共基类：沿第一个值的继承链上行 ----
    //  跳过 Object（通用根，所有类都继承它，无判别信息）：若只有 Object
    //  是公共基类，应继续尝试公共接口（[Circle, Rect, Square] 都实现
    //  Shape 时元素类型应是 Shape 而非 Object）。
    if (vals[0].type->kind == TypeKind::Class) {
        auto first = classOfType(vals[0].type);
        for (const ClassInfo* c = first ? first.get() : nullptr; c; c = c->base) {
            if (c->name == "object$Object") continue;
            TypePtr ct = Type::makeClass(c->name);
            bool ok = true;
            for (const auto& v : vals)
                if (!isAssignable(v.type, ct)) { ok = false; break; }
            if (ok) return c->name;
        }
    }

    // ---- 再尝试公共接口 ----
    std::vector<std::string> cands;
    if (vals[0].type->kind == TypeKind::Interface) {
        // 泛型接口按实例名（Iterable<Int> -> Iterable$Int）
        cands.push_back(vals[0].type->typeArgs.empty()
                        ? vals[0].type->className
                        : vals[0].type->monoName());
    } else if (vals[0].type->kind == TypeKind::Class) {
        auto ci = classOfType(vals[0].type);
        if (!ci) return {};
        // 沿继承链收集所有接口
        for (const ClassInfo* c = ci.get(); c; c = c->base)
            for (const auto& n : c->interfaceNames) cands.push_back(n);
    } else {
        return {};
    }

    for (const auto& iname : cands) {
        TypePtr it = Type::makeInterface(iname);
        bool ok = true;
        for (const auto& v : vals)
            if (!isAssignable(v.type, it)) { ok = false; break; }
        if (ok) return iname;
    }
    return {};
}

// ============================================================
//  函数与泛型函数（类/方法/构造函数的代码生成在 IRGenClass.cpp）
// ============================================================

namespace {
// harness 文件名：hao test 生成的合成入口（可在任意目录）
bool isHaoTestHarnessPath(const std::string& path) {
    static const char kSuf[] = "__hao_test_main.hao";
    constexpr size_t kLen = sizeof(kSuf) - 1;
    if (path.size() < kLen) return false;
    return path.compare(path.size() - kLen, kLen, kSuf) == 0;
}
} // namespace

void IRGen::setTestMode(bool v) { testMode_ = v; }

void IRGen::collectFunctionSignatures(const SourceUnit& u) {
    int unitInitCount = 0;   // 每包只允许一个 init()
    for (auto* decl : u.tree->topLevelDecl()) {
        auto* fn = decl->funcDecl();
        if (!fn) continue;
        // 泛型函数由 collectGenericFunctions 专门处理
        if (fn->typeParams()) continue;

        std::string shortName = fn->IDENT()->getText();
        // testMode：跳过业务 main（不登记），仅 harness 的 main 作为入口
        if (testMode_ && shortName == "main" && !isHaoTestHarnessPath(u.path))
            continue;

        bool isExtern = declIsExtern(fn->modifier());

        auto sym = std::make_shared<Symbol>();
        sym->kind = SymbolKind::Function;
        sym->name = currentPkgPrefix_ + shortName;
        sym->line = fn->getStart()->getLine();
        sym->column = fn->getStart()->getCharPositionInLine();
        sym->isPrivate = declIsPrivate(fn->modifier());
        sym->isExtern = isExtern;

        sym->returnType = fn->returnType()
            ? resolveType(fn->returnType()->type())
            : Type::makeUnit();

        if (auto* pl = fn->paramList()) {
            for (auto* p : pl->param()) {
                sym->paramNames.push_back(p->IDENT()->getText());
                sym->paramTypes.push_back(resolveType(p->type()));
            }
        }

        sym->type = Type::makeFunc(sym->paramTypes, sym->returnType);

        if (isExtern) {
            // extern func f(...) = "c_name";  —— 无函数体，映射到外部 C 符号
            if (fn->block()) {
                error(fn, "extern 函数 '" + shortName + "' 不能有函数体");
                continue;
            }
            // 链接名：显式 = "name" 优先，否则用 HaoLang 短名
            if (fn->STRING_LIT())
                sym->linkName = parseStringLitText(fn->STRING_LIT()->getText());
            else
                sym->linkName = shortName;
            // 调用直接 call @linkName，不带包前缀
            sym->irName = "@" + sym->linkName;

            // 可选 @link("ws2_32", ...)：为该 extern 声明外部链接库（追加到链接命令）。
            if (auto* ld = fn->linkDecl()) {
                if (ld->IDENT() && ld->IDENT()->getText() != "link")
                    error(ld, "未知的 extern 属性 '@" + ld->IDENT()->getText()
                           + "'（当前仅支持 @link）");
                for (auto* sl : ld->STRING_LIT())
                    sym->linkLibs.push_back(parseStringLitText(sl->getText()));
            }
            // 汇总到 IRGen 层（跨 extern 去重），供 Driver 拼链接命令。
            for (auto& lb : sym->linkLibs)
                if (std::find(linkLibs_.begin(), linkLibs_.end(), lb) == linkLibs_.end())
                    linkLibs_.push_back(lb);
        } else {
            if (!fn->block()) {
                error(fn, "函数 '" + shortName + "' 缺少实现体（只有 abstract/interface/extern 函数可以没有函数体）");
                continue;
            }
            // 函数重载：同名后续函数用参数签名后缀区分 IR 符号名，
            // 否则多个重载在 LLVM IR 里都叫 @f，clang 报重复定义。
            // （extern 重载靠显式 = "linkName" 区分，不加后缀。）
            std::string irName = "@" + sym->name;
            if (!overloads_[sym->name].empty()) {
                irName += "$";
                for (size_t k = 0; k < sym->paramTypes.size(); ++k) {
                    if (k) irName += "$";
                    irName += overloadSuffix(sym->paramTypes[k]);
                }
            }
            sym->irName = irName;
        }

        // Go 式 init()：包级 func init()，在 main 前按依赖顺序自动调用。
        // 每包只允许一个 init()（简化：Go 允许多个，但相同签名会让
        // genFunction 的重载匹配无法区分，这里限制为一个）。
        bool isInit = (shortName == "init" && !isExtern);
        if (isInit) {
            if (++unitInitCount > 1) {
                error(fn, "每个包只能有一个 init() 函数");
                continue;
            }
            sym->irName = "@" + sym->name;
            if (!sym->paramTypes.empty())
                error(fn, "init() 函数不能有参数");
            if (!sym->returnType->isUnit())
                error(fn, "init() 函数必须无返回值（省略返回类型）");
            initCalls_.push_back(sym->irName);
        }

        // 登记到重载表：同名函数按参数签名区分（函数重载 v0.9.0）。
        // 第一个同名函数照常进符号表（供 lookup / 名字解析判定"是函数"），
        // 其余重载只进 overloads_，调用点按实参个数/类型选最佳匹配。
        auto& provided = overloads_[sym->name];
        bool identicalSig = false;
        for (const auto& c : provided) {
            if (c->paramTypes.size() != sym->paramTypes.size()) continue;
            if (!c->returnType->equals(*sym->returnType)) continue;
            bool same = true;
            for (size_t k = 0; k < sym->paramTypes.size(); ++k)
                if (!c->paramTypes[k]->equals(*sym->paramTypes[k])) { same = false; break; }
            if (same) { identicalSig = true; break; }
        }
        // init() 天然同签名（无参），不判重复；其它函数同签名才报重定义。
        if (identicalSig && !isInit) {
            error(fn, "函数 '" + shortName + "' 重复定义（相同参数签名）");
            continue;
        }
        if (provided.empty())
            syms_.declareGlobal(sym);   // 首个代表符号进符号表
        provided.push_back(sym);

        // public 函数登记到包导出表（extern 也可以被跨包调用）
        if (!sym->isPrivate)
            pkgExports_[u.importPath][shortName] = sym->name;
    }
}

// ------------------------------------------------------------
//  收集 delegate 命名函数类型别名（v0.19.0）
// ------------------------------------------------------------
//  `delegate (Int)->String MyConverter;` 只登记类型别名（名 -> 函数类型），
//  不产生任何代码。resolveType 命中时返回该函数类型。
void IRGen::collectDelegates(const SourceUnit& u) {
    for (auto* decl : u.tree->topLevelDecl()) {
        auto* dd = decl->delegateDecl();
        if (!dd) continue;
        std::string name = currentPkgPrefix_ + dd->IDENT()->getText();
        TypePtr ft = resolveType(dd->type());
        if (ft->kind != TypeKind::Func) {
            error(dd, "delegate '" + dd->IDENT()->getText() +
                       "' 的类型必须是函数类型 (params)->ret");
            continue;
        }
        delegates_[name] = ft;
    }
}

// ------------------------------------------------------------
//  注解使用解析（v0.19.0）
// ------------------------------------------------------------
//  @Name(args) -> AnnotationUse{name, {键, 值字符串}*}. 参数编译期常量求值
//  （字符串/整数/浮点/布尔/标识符），供反射读取。
std::string IRGen::annotationArgValue(HaoLangParser::ExprContext* e) {
    if (!e) return "";
    // DFS 子树找第一个字面量/标识符 primary，提取其常量文本。
    std::function<std::string(antlr4::tree::ParseTree*)> find;
    find = [&](antlr4::tree::ParseTree* n) -> std::string {
        if (!n) return "";
        if (auto* lit = dynamic_cast<HaoLangParser::LiteralContext*>(n)) {
            if (auto* s = lit->STRING_LIT()) {
                // unescapeStringLiteral 已剥引号；勿再 substr，否则 "/ping"→"pin"
                return StringUtil::unescapeStringLiteral(s->getText());
            }
            if (lit->INT_LIT())    return lit->INT_LIT()->getText();
            if (lit->FLOAT_LIT())  return lit->FLOAT_LIT()->getText();
            if (lit->CHAR_LIT())   return lit->CHAR_LIT()->getText();
            if (lit->TRUE())       return "true";
            if (lit->FALSE())      return "false";
            if (lit->NULL_LIT())   return "null";
        }
        if (auto* id = dynamic_cast<HaoLangParser::IdentPrimaryContext*>(n))
            return id->IDENT()->getText();   // 标识符常量（enum 成员等）
        for (auto* c : n->children) {
            std::string r = find(c);
            if (!r.empty()) return r;
        }
        return "";
    };
    return find(e);
}

std::vector<AnnotationUse> IRGen::resolveAnnotationUses(
    const std::vector<HaoLangParser::AnnotationUseContext*>& uses) {
    std::vector<AnnotationUse> out;
    for (auto* au : uses) {
        if (!au) continue;
        AnnotationUse a;
        // v0.50：解析到真实注解 ClassInfo（供 meta 令牌）
        bool iface = false;
        std::string internal = resolveTypeQualifiedName(au->qualifiedName(), &iface);
        // import net; 后写 @GetMapping：类型解析只扫通配导入；注解额外扫
        // 各包导出中的 @interface（与历史「裸名注解」用法兼容）
        if (internal.empty() && au->qualifiedName()->IDENT().size() == 1) {
            std::string shortName = au->qualifiedName()->IDENT()[0]->getText();
            std::string hit;
            for (const auto& im : currentImports_) {
                auto pit = pkgExports_.find(im.importPath);
                if (pit == pkgExports_.end()) continue;
                auto eit = pit->second.find(shortName);
                if (eit == pit->second.end()) continue;
                auto cit = classes_.find(eit->second);
                if (cit == classes_.end() || !cit->second->isAnnotation) continue;
                if (!hit.empty() && hit != eit->second) {
                    error(au, "注解 '" + shortName + "' 有歧义");
                    hit.clear();
                    break;
                }
                hit = eit->second;
            }
            if (!hit.empty()) internal = hit;
        }
        if (internal.empty()) {
            internal = currentPkgPrefix_ + au->qualifiedName()->getText();
        }
        a.name = internal;
        a.className = classes_.count(internal) ? internal : "";
        if (auto* al = au->annotationArgs()) {
            for (auto* arg : al->annotationArg()) {
                if (arg->IDENT()) {
                    a.args.emplace_back(arg->IDENT()->getText(),
                                        annotationArgValue(arg->expr()));
                } else {
                    // 单值注解：键为空，值进 args[0]
                    a.args.emplace_back("", annotationArgValue(arg->expr()));
                }
            }
        }
        out.push_back(std::move(a));
    }
    return out;
}

// ------------------------------------------------------------
//  收集泛型函数模板
// ------------------------------------------------------------
void IRGen::collectGenericFunctions(const SourceUnit& u) {
    for (auto* decl : u.tree->topLevelDecl()) {
        auto* fn = decl->funcDecl();
        if (!fn || !fn->typeParams()) continue;

        std::string shortName = fn->IDENT()->getText();
        GenericFn tmpl;
        tmpl.name = currentPkgPrefix_ + shortName;
        tmpl.pkgPrefix = currentPkgPrefix_;
        tmpl.decl = fn;
        for (auto* id : fn->typeParams()->IDENT())
            tmpl.typeParams.push_back(id->getText());
        genericFns_[tmpl.name] = tmpl;

        auto sym = std::make_shared<Symbol>();
        sym->kind = SymbolKind::Function;
        sym->name = tmpl.name;
        sym->irName = "@" + tmpl.name;
        sym->line = fn->getStart()->getLine();
        sym->returnType = Type::makeUnknown();
        sym->isPrivate = declIsPrivate(fn->modifier());
        syms_.declareGlobal(sym);

        if (!sym->isPrivate)
            pkgExports_[u.importPath][shortName] = sym->name;
    }
}

// ------------------------------------------------------------
//  实例化泛型函数（按实参推断 T）
// ------------------------------------------------------------
SymbolPtr IRGen::instantiateFunction(const std::string& tplName,
                                      const std::vector<Value>& args,
                                      antlr4::ParserRuleContext* useSite) {
    auto it = genericFns_.find(tplName);
    if (it == genericFns_.end()) return nullptr;
    const GenericFn& tmpl = it->second;
    auto* fn = tmpl.decl;

    // 先建立类型参数上下文，使 resolveType 能把 T 识别为 TypeParam，
    // 否则它会被当成未定义的类名，推断永远失败。
    auto savedParams0 = currentTypeParams_;
    auto savedPrefix0 = currentPkgPrefix_;
    currentTypeParams_.clear();
    // 解析模板签名时用模板自己的包前缀，使同包类型的裸名能正确解析
    currentPkgPrefix_ = tmpl.pkgPrefix;
    for (const auto& tp : tmpl.typeParams) currentTypeParams_.insert(tp);

    // 收集形参类型（含类型参数 T）
    std::vector<TypePtr> paramTypes;
    if (auto* pl = fn->paramList())
        for (auto* p : pl->param())
            paramTypes.push_back(resolveType(p->type()));

    if (args.size() != paramTypes.size()) {
        currentTypeParams_ = savedParams0;
        currentPkgPrefix_ = savedPrefix0;
        return nullptr;
    }

    // 推断替换表（递归合一，支持 T、[T]、(T)->R 等嵌套类型参数）
    TypeSubst subst;
    for (size_t i = 0; i < paramTypes.size(); ++i)
        unifyWithArg(paramTypes[i], args[i].type, subst, useSite);

    for (const auto& tp : tmpl.typeParams)
        if (!subst.count(tp)) {
            error(useSite, "无法推断泛型函数 '" + tplName + "' 的类型参数 " + tp);
            currentTypeParams_ = savedParams0;
            currentPkgPrefix_ = savedPrefix0;
            return nullptr;
        }
    if (diags_.hasErrors()) {
        currentTypeParams_ = savedParams0;
        currentPkgPrefix_ = savedPrefix0;
        return nullptr;
    }

    // 实例名 firstOf$Int
    std::string instName = tplName;
    for (const auto& tp : tmpl.typeParams)
        instName += "$" + subst[tp]->monoName();

    // 已实例化过该组合则直接返回缓存的符号
    if (generatedGenericFns_.count(instName)) {
        currentTypeParams_ = savedParams0;
        currentPkgPrefix_ = savedPrefix0;
        return syms_.global()->lookup(instName);
    }
    generatedGenericFns_.insert(instName);

    // 在替换表生效下重新解析签名
    auto savedSubst  = currentSubst_;
    currentSubst_ = subst;
    // currentTypeParams_ 已是类型参数集合（推断阶段设置的），保持即可

    auto sym = std::make_shared<Symbol>();
    sym->kind = SymbolKind::Function;
    sym->name = instName;
    sym->irName = "@" + instName;
    sym->line = fn->getStart()->getLine();
    sym->column = fn->getStart()->getCharPositionInLine();
    sym->returnType = fn->returnType()
        ? resolveType(fn->returnType()->type())
        : Type::makeUnit();
    if (auto* pl = fn->paramList())
        for (auto* p : pl->param()) {
            sym->paramNames.push_back(p->IDENT()->getText());
            sym->paramTypes.push_back(resolveType(p->type()));
        }
    sym->type = Type::makeFunc(sym->paramTypes, sym->returnType);
    syms_.declareGlobal(sym);
    PendingFnInstance pi;
    pi.decl = fn;
    pi.subst = subst;
    pi.instName = instName;
    pi.pkgPrefix = tmpl.pkgPrefix;
    pendingFnInstances_.push_back(std::move(pi));

    currentSubst_      = savedSubst;
    currentTypeParams_ = savedParams0;
    currentPkgPrefix_  = savedPrefix0;
    return sym;
}

// 递归合一：把模板形参类型 param 与实参类型 arg 对齐，把发现的
// 类型参数绑定写入 subst。递归处理数组与函数类型，使
// map<T,R>([T], (T)->R) 能从实参 [Int] 和 (Int)->String 同时推断 T、R。
void IRGen::unifyWithArg(const TypePtr& param, const TypePtr& arg,
                          TypeSubst& subst, antlr4::ParserRuleContext* ctx) {
    if (!param || !arg) return;

    auto bind = [&](const std::string& tpName, const TypePtr& actual) {
        if (!actual || actual->isUnknown()) return;
        auto it = subst.find(tpName);
        if (it != subst.end() && !it->second->sameShape(*actual)) {
            error(ctx, "无法统一类型参数 " + tpName + "：同时推断为 " +
                         it->second->toString() + " 和 " + actual->toString());
            return;
        }
        subst[tpName] = actual;
    };

    if (param->kind == TypeKind::TypeParam) {
        bind(param->className, arg);
        return;
    }
    if (param->kind == TypeKind::Array && arg->kind == TypeKind::Array) {
        unifyWithArg(param->elem, arg->elem, subst, ctx);
        return;
    }
    if (param->kind == TypeKind::Func && arg->kind == TypeKind::Func) {
        size_t n = std::min(param->params.size(), arg->params.size());
        for (size_t i = 0; i < n; ++i)
            unifyWithArg(param->params[i], arg->params[i], subst, ctx);
        unifyWithArg(param->elem, arg->elem, subst, ctx);
        return;
    }
    // 其它情况（Int->Double 提升、具体类型一致）不贡献类型参数绑定；
    // 赋值兼容性在调用点用 isAssignable 校验。
}

Value IRGen::callGenericFunction(const std::string& tplName,
                                 HaoLangParser::ArgListContext* al,
                                 antlr4::ParserRuleContext* ctx) {
    auto it = genericFns_.find(tplName);
    if (it == genericFns_.end()) return Value();
    const GenericFn& tmpl = it->second;

    auto savedParams0 = currentTypeParams_;
    auto savedPrefix0 = currentPkgPrefix_;
    auto savedSubst0  = currentSubst_;
    currentTypeParams_.clear();
    currentPkgPrefix_ = tmpl.pkgPrefix;
    for (const auto& tp : tmpl.typeParams) currentTypeParams_.insert(tp);

    // 模板形参类型（含 TypeParam）
    std::vector<TypePtr> tplParams;
    auto* fn = tmpl.decl;
    if (auto* pl = fn->paramList())
        for (auto* p : pl->param())
            tplParams.push_back(resolveType(p->type()));

    size_t nargs = al ? al->arg().size() : 0;

    // 判断某个实参表达式是否为 lambda（需要期望类型才能推断参数/返回）。
    // 对裸 lambda 实参（map(arr, { it*2 })）沿优先级链下降到 primary。
    auto isLambdaArg = [&](size_t k) -> HaoLangParser::LambdaContext* {
        antlr4::tree::ParseTree* node = al->arg(k)->expr();
        // 沿单孩子链下降到 postfixExpr（裸 lambda 没有运算符/后缀，每
        // 一层只有一个孩子即下一优先级的子表达式）
        for (;;) {
            if (auto* pe = dynamic_cast<HaoLangParser::PostfixExprContext*>(node)) {
                if (pe->postfixOp().empty()) {
                    if (auto* lp = dynamic_cast<HaoLangParser::LambdaPrimaryContext*>(
                            pe->primary()))
                        return lp->lambda();
                }
                return nullptr;
            }
            if (node->children.size() != 1) return nullptr;
            node = node->children[0];
        }
    };

    // ---- 第一遍：求值非 lambda 实参，建立初步替换表（如 [T] 得到 T）----
    std::vector<Value> args(nargs);
    TypeSubst subst;
    for (size_t k = 0; k < nargs; ++k) {
        if (isLambdaArg(k)) continue;
        Value av = genExpr(al->arg(k)->expr());
        if (!av.valid()) { currentTypeParams_=savedParams0; currentPkgPrefix_=savedPrefix0; currentSubst_=savedSubst0; return Value(); }
        rootGcOperand(av);
        args[k] = av;
        if (k < tplParams.size())
            unifyWithArg(tplParams[k], av.type, subst, ctx);
    }

    // ---- 第二遍：求值 lambda 实参 ----
    //  用已知替换把形参函数类型具体化（T 已有值，R 仍是 TypeParam），
    //  作为期望类型压栈，lambda 据此确定参数类型、并从函数体推断返回类型，
    //  随后再合一，从而得到 R。
    for (size_t k = 0; k < nargs; ++k) {
        if (!isLambdaArg(k)) continue;
        TypePtr expected;
        if (k < tplParams.size()) {
            expected = substType(tplParams[k], subst);
            // 仍未解析的类型参数（如 map 的 R）不能作为 lambda 的期望类型：
            // 它要求 lambda 返回 TypeParam，会与实际返回的具体类型冲突。
            // 把这些位置换成 Unknown，让 lambda 自己从函数体推断，随后合一得到它们。
            if (expected->kind == TypeKind::Func) {
                auto conv = std::make_shared<Type>(*expected);
                for (auto& p : conv->params)
                    if (p->hasTypeParam()) p = Type::makeUnknown();
                // 仍未解析的返回类型（如 map 的 R）置空，让 lambda 从函数体推断，
                // 再由 unifyWithArg 把实际返回类型绑定到 R
                if (conv->elem && conv->elem->hasTypeParam())
                    conv->elem = nullptr;
                expected = conv;
            }
            expectedTypes_.push_back(expected);
        }
        Value av = genExpr(al->arg(k)->expr());
        if (k < tplParams.size()) expectedTypes_.pop_back();
        if (!av.valid()) { currentTypeParams_=savedParams0; currentPkgPrefix_=savedPrefix0; currentSubst_=savedSubst0; return Value(); }
        rootGcOperand(av);
        args[k] = av;
        if (k < tplParams.size())
            unifyWithArg(tplParams[k], av.type, subst, ctx);
    }

    currentTypeParams_ = savedParams0;
    currentPkgPrefix_  = savedPrefix0;
    currentSubst_      = savedSubst0;

    if (diags_.hasErrors()) return Value();

    // 所有类型实参齐备，实例化并直接调用
    auto sym = instantiateFunction(tplName, args, ctx);
    if (!sym || diags_.hasErrors()) return Value();

    if (args.size() != sym->paramTypes.size()) {
        error(ctx, "函数需要 " + std::to_string(sym->paramTypes.size()) +
                     " 个参数，实际 " + std::to_string(args.size()) + " 个");
        return Value();
    }
    std::string argStr;
    for (size_t k = 0; k < args.size(); ++k) {
        if (!isAssignable(args[k].type, sym->paramTypes[k])) {
            error(ctx, "第 " + std::to_string(k+1) + " 个参数类型不匹配：期望 " +
                         sym->paramTypes[k]->toString() + "，实际 " +
                         args[k].type->toString());
            return Value();
        }
        args[k] = coerce(args[k], sym->paramTypes[k], 0, 0);
        if (k) argStr += ", ";
        argStr += formatCallArg(sym->paramTypes[k], al->arg(k)->expr(), args[k]);
    }
    if (sym->returnType->isUnit()) {
        emitCallVoid(sym->irName, argStr);
        return Value("", Type::makeUnit());
    }
    std::string reg = emitCall(sym->returnType->llvmType(), sym->irName, argStr);
    return Value(reg, sym->returnType);
}

void IRGen::genPendingFunctionInstances() {
    while (!pendingFnInstances_.empty()) {
        auto batch = pendingFnInstances_;
        pendingFnInstances_.clear();
        for (auto& pi : batch) {
            auto savedSubst  = currentSubst_;
            auto savedParams = currentTypeParams_;
            auto savedPrefix = currentPkgPrefix_;
            currentSubst_ = pi.subst;
            currentPkgPrefix_ = pi.pkgPrefix;
            currentTypeParams_.clear();
            for (auto& [k, v] : pi.subst) currentTypeParams_.insert(k);
            auto sym = syms_.global()->lookup(pi.instName);
            if (sym) genFunction(pi.decl, pi.instName, sym);
            currentSubst_      = savedSubst;
            currentTypeParams_ = savedParams;
            currentPkgPrefix_  = savedPrefix;
        }
    }
}

// ============================================================
//  函数体
// ============================================================

// ============================================================
//  函数体公共流程
// ------------------------------------------------------------
//  普通函数、方法、构造函数三者的主体流程一致：
//    绑定参数到栈 -> 生成语句 -> 补全 return
//  方法与构造函数额外有隐式 this 参数。
// ============================================================

void IRGen::genFunctionBody(HaoLangParser::BlockContext* body,
                            const std::vector<std::string>& paramNames,
                            const std::vector<TypePtr>& paramTypes,
                            const TypePtr& returnType,
                            bool isMain,
                            bool hasThis,
                            const std::string& thisClassName,
                            antlr4::ParserRuleContext* declCtx,
                            const std::string& declName) {
    SymbolTable::Guard guard(syms_);

    // ---- try / finally 清理用的函数级槽位（含 GC 返回/异常根）----
    emitAllocUnwindSlots();
    tryStack_.clear();
    catchDepth_ = 0;
    tryCounter_ = 0;

    // ---- 在绑定形参前分析 lambda，确定哪些名字被捕获 ----
    //  被任意 lambda 捕获的可变变量（参数或 var 局部变量）需要堆装箱，
    //  使外层与所有闭包共享同一个 cell。分析只需 AST 和「本函数形参名」，
    //  不依赖符号表，故可在参数符号建立前进行。
    lambdas_.clear();
    capturedVarNames_.clear();
    analyzeLambdas(body);

    /* v0.54：精确根水位（本函数所有 push 在出口 unwind） */
    beginFunctionGcRoots();
    emitPushUnwindGcRoot();

    // ---- 隐式 this ----
    unsigned dbgArgBase = 0;
    int dbgLine = (declCtx && declCtx->getStart())
                      ? static_cast<int>(declCtx->getStart()->getLine())
                      : 1;
    if (hasThis) {
        thisAddr_ = "%this.addr";
        emitAllocaAt(thisAddr_, "ptr");
        emitStore("ptr", "%this.arg", thisAddr_);
        emitGcRootPush(thisAddr_);

        auto ts = std::make_shared<Symbol>();
        ts->kind = SymbolKind::Variable;
        ts->name = "this";
        ts->type = Type::makeClass(thisClassName);
        ts->isMutable = false;
        ts->irAddr = thisAddr_;
        syms_.declare(ts);
        dbgArgBase = 1;
        emitDbgDeclareIf(thisAddr_, "this", dbgLine, 1);
        /* D15：this 初值薄 dbg.value */
        emitDbgValueIf("ptr", "%this.arg", "this", dbgLine, 1);
    }

    // ---- 参数：拷贝到栈上 ----
    //  被 lambda 捕获的参数装箱到堆 cell（holder 存 cell 指针），
    //  保证闭包内对参数的修改对外层/其它闭包可见。
    for (size_t i = 0; i < paramTypes.size(); ++i) {
        const auto& pname = paramNames[i];
        const auto& ptype = paramTypes[i];
        std::string addr = "%" + pname + ".addr";
        // main(args: [String]) 的首个参数：源是 genFunction 构造的 args 数组，
        // 不是 %args.arg（签名里没有该参数），且不参与可变装箱。
        bool isArgsParam = (isMain && i == 0 && !mainArgsIR_.empty());
        bool boxed = capturedVarNames_.count(pname) > 0 && !isArgsParam;
        std::string argSrc = isArgsParam ? mainArgsIR_
                                         : ("%" + pname + ".arg");

        auto ps = std::make_shared<Symbol>();
        ps->kind = SymbolKind::Variable;
        ps->name = pname;
        ps->type = ptype;
        ps->isMutable = true;

        // 数组形参：按引用（调用方传入「数组指针槽」地址）。
        // 例外：main 的 args、lambda 捕获装箱、以及 [T]?（调用方传数据指针/null，
        // 与 formatCallArg 仅对非空数组走 by-ref 对齐；可空走 by-ref 会砸堆）。
        bool arrayByRef = (ptype->kind == TypeKind::Array && !ptype->nullable &&
                           !isArgsParam && !boxed);
        if (arrayByRef) {
            // %name.arg 本身即调用方槽位 ptr；直接作 irAddr，+= 写回调用方
            ps->irAddr = argSrc;
            ps->byRefParam = true;
            emitGcRootPush(argSrc);
        } else if (boxed) {
            /* GC 形参：先把 .arg 挂进 shadow，再 object_new（分配可 safepoint） */
            emitAllocaAt(addr, "ptr");
            if (isGcPointerType(ptype)) {
                emitStore("ptr", argSrc, addr);
                emitGcRootPush(addr);
                std::string held = emitLoad("ptr", addr);
                std::string cell = emitObjectNew(1, 1);
                emitHeapStore(cell, held, ptype, cell);
                emitStore("ptr", cell, addr);
            } else {
                std::string cell = emitObjectNew(1, 0);
                emitHeapStore(cell, argSrc, ptype, cell);
                emitStore("ptr", cell, addr);
                emitGcRootPush(addr);
            }
            ps->boxed = true;
            ps->irAddr = addr;
        } else {
            emitAllocaAt(addr, ptype->llvmType());
            emitStore(ptype->llvmType(), argSrc, addr);
            ps->irAddr = addr;
            if (isGcPointerType(ptype))
                emitGcRootPush(addr);
        }
        syms_.declare(ps);
        unsigned argNo = dbgArgBase + static_cast<unsigned>(i) + 1;
        emitDbgDeclareIf(ps->irAddr, pname, dbgLine, argNo);
        /* D15：形参初值薄 dbg.value（array-by-ref 仅 declare） */
        if (!arrayByRef)
            emitDbgValueIf(ptype->llvmType(), argSrc, pname, dbgLine, argNo);
    }

    // ---- 函数体：符号与参数同层；块 GC 作用域使顶层局部在落回出口前可清槽 ----
    /* v0.53.3：入口 safepoint，加密协作 STW（循环外长直线路径也能 park） */
    emitSafepoint();
    pushSmartCastFrame();
    beginBlockGcScope();
    genBlock(body, /*newScope=*/false);
    endBlockGcScope();
    popSmartCastFrame();

    // ---- 补全返回 ----
    if (!blockTerminated_) {
        emitGcRootUnwind();
        emitRuntimePopFrame();
        if (isMain) {
            emitRet("i32", "0");
        } else if (returnType->isUnit()) {
            emitRetVoid();
        } else {
            error(declCtx, "'" + declName + "' 声明返回 " + returnType->toString() +
                           "，但存在没有 return 的执行路径");
            // 仍生成合法 IR，避免 clang 报错掩盖真实错误
            emitRet(returnType->llvmType(), zeroValueFor(returnType));
        }
    }
    gcRootWm_.clear();
    loopHoisted_.clear();
    /* A15：函数尾探测 leave noop（depth 已 0 → 仅 TRACE，无 IR） */
    if (loopSpillDepth_ <= 0)
        leaveLoopSpillScope();
    loopSpillPools_.clear();
    loopSpillDepth_ = 0;
    blockGcSlots_.clear();
}

void IRGen::genFunction(HaoLangParser::FuncDeclContext* fn) {
    // 普通顶层函数：用加了包前缀的内部名查已收集的签名。
    // 符号表里只存同名的第一个代表符号，重载函数需按 AST 参数签名
    // 从 overloads_ 里选出对应的符号（否则会拿错重载的参数来生成函数体）。
    std::string shortName = fn->IDENT()->getText();
    // testMode：业务 main 已在 collect 时跳过；此处再拦一层，避免误用 harness 的符号生成体
    if (testMode_ && shortName == "main" && !isHaoTestHarnessPath(currentUnitPath_))
        return;
    std::string name = currentPkgPrefix_ + shortName;
    auto& oc = overloads_[name];
    if (oc.empty()) return;
    SymbolPtr sym;
    if (oc.size() == 1) {
        sym = oc[0];
    } else {
        // 解析本 decl 的签名，与候选重载逐一比对
        std::vector<TypePtr> want;
        if (auto* pl = fn->paramList())
            for (auto* p : pl->param())
                want.push_back(resolveType(p->type()));
        TypePtr ret = fn->returnType() ? resolveType(fn->returnType()->type())
                                       : Type::makeUnit();
        for (const auto& c : oc) {
            if (c->paramTypes.size() != want.size()) continue;
            if (!c->returnType->equals(*ret)) continue;
            bool same = true;
            for (size_t k = 0; k < want.size(); ++k)
                if (!c->paramTypes[k]->equals(*want[k])) { same = false; break; }
            if (same) { sym = c; break; }
        }
        if (!sym) {
            error(fn, "找不到函数 '" + name + "' 的重载定义（签名不匹配）");
            return;
        }
    }
    // 用 sym->irName（重载时带参数签名后缀）作为 define 名，避免各重载
    // 复用同一个 @area 导致 clang 重复定义。
    genFunction(fn, sym->irName.substr(1), sym);
}

// 供泛型实例化调用：用显式提供的符号（可能是 firstOf$Int）生成代码
void IRGen::genFunction(HaoLangParser::FuncDeclContext* fn,
                          const std::string& instName,
                          const SymbolPtr& sym) {
    std::string name = instName;

    // 顶层函数必须有实现体（无体形式仅用于抽象方法与接口方法）
    if (!fn->block()) {
        error(fn, "函数 '" + name + "' 缺少实现体");
        return;
    }

    em_.resetFunctionState();
    currentReturn_ = sym->returnType;
    sawReturn_ = false;
    blockTerminated_ = false;
    currentClass_ = nullptr;
    thisAddr_.clear();
    inConstructor_ = false;

    // main 的返回类型在 IR 中固定为 i32（C 入口约定）
    bool isMain = (instName == "main");
    inMain_ = isMain;
    if (isMain && !(sym->returnType->isUnit() || sym->returnType->kind == TypeKind::Int)) {
        error(fn, "main 函数返回类型必须是 Unit 或 Int（C 入口约定），不能是 " +
                      sym->returnType->toString());
    }
    std::string retIR = isMain ? "i32" : sym->returnType->llvmType();

    // ---- main 命令行参数 args[] ----
    //  用户可声明 `func main(args: [String])`。此时 C 入口签名用标准
    //  `int main(int argc, char** argv)`，从 argc/argv 构造 [String] 数组。
    bool hasArgs = false;
    if (isMain && sym->paramTypes.size() == 1) {
        const auto& p0 = sym->paramTypes[0];
        if (p0->kind == TypeKind::Array && p0->elem &&
            p0->elem->kind == TypeKind::String)
            hasArgs = true;
    }

    // ---- 函数签名 ----
    std::string sig = "define " + retIR + " @" + name + "(";
    if (hasArgs) {
        sig += "i32 %argc, i8** %argv";
    } else {
        for (size_t i = 0; i < sym->paramTypes.size(); ++i) {
            if (i) sig += ", ";
            sig += sym->paramTypes[i]->llvmType() + " %" + sym->paramNames[i] + ".arg";
        }
    }
    sig += ") {";

    em_.emitBlank();
    em_.emitRaw(sig);
    em_.emitLabel("entry");
    beginDebugFunction(fn, name);

    // Go 式 init()：main 启动时先按依赖顺序调用各包 init()（依赖在前）
    if (isMain) {
        // 静态 GC 指针字段挂根槽（函数体在全部类就绪后 emit）
        emitCallVoid("@hao.registerStaticRoots", "");
        for (const auto& in : initCalls_)
            emitCallVoid(in, "");
    }

    // main 带 args：把 argc/argv 构造成 [String] 数组，供参数绑定使用。
    mainArgsIR_.clear();
    if (hasArgs) {
        mainArgsIR_ = emitCall("ptr", "@hao_make_args", "i32 %argc, i8** %argv");
    }

    genFunctionBody(fn->block(), sym->paramNames, sym->paramTypes,
                    sym->returnType, isMain, /*hasThis=*/false, "",
                    fn, "函数 '" + name + "'");

    em_.flushEntryAllocas();
    em_.emitRaw("}");
}

// ============================================================
//  类
// ------------------------------------------------------------
//  对象是一块堆内存，字段按声明顺序占据 8 字节槽位。
//  方法降级为普通函数，隐式 this 作为首个参数。
//  构造函数由 new 表达式在分配内存后调用。
// ============================================================


// ============================================================
//  语句
// ============================================================

void IRGen::genExprStmt(HaoLangParser::ExprStmtContext* st) {
    genExpr(st->expr());
}

} // namespace hao
