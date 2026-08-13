// ============================================================
//  Sema 类型解析实现（G2 首刀）
// ============================================================
#include "sema/TypeResolve.h"

namespace hao {

std::string resolveTypeQualifiedName(const TypeResolveEnv& env,
                                     HaoLangParser::QualifiedNameContext* qn,
                                     bool* isIface) {
    if (!qn || !env.currentPkgPrefix || !env.currentImports || !env.pkgExports ||
        !env.hasClass || !env.hasInterface)
        return "";
    auto ids = qn->IDENT();
    if (ids.empty()) return "";

    auto lookupInternal = [&](const std::string& internal) -> std::string {
        if (env.hasClass(internal)) {
            if (isIface) *isIface = false;
            return internal;
        }
        if (env.hasInterface(internal)) {
            if (isIface) *isIface = true;
            return internal;
        }
        return "";
    };

    if (ids.size() == 1) {
        std::string shortName = ids[0]->getText();
        std::string r = lookupInternal(*env.currentPkgPrefix + shortName);
        if (!r.empty()) return r;
        for (const auto& im : *env.currentImports) {
            if (!im.wildcard) continue;
            auto it = env.pkgExports->find(im.importPath);
            if (it == env.pkgExports->end()) continue;
            auto eit = it->second.find(shortName);
            if (eit != it->second.end()) return lookupInternal(eit->second);
        }
        return "";
    }

    std::string alias = ids[0]->getText();
    for (const auto& im : *env.currentImports) {
        if (im.alias != alias) continue;
        std::string member = ids.back()->getText();
        auto it = env.pkgExports->find(im.importPath);
        if (it == env.pkgExports->end()) break;
        auto eit = it->second.find(member);
        if (eit != it->second.end()) return lookupInternal(eit->second);
        break;
    }
    return "";
}

TypePtr resolveType(const TypeResolveEnv& env, HaoLangParser::TypeContext* t) {
    if (!t) return Type::makeUnknown();

    TypePtr base = Type::makeUnknown();
    auto* bt = t->baseType();

    if (auto* named = dynamic_cast<HaoLangParser::NamedTypeContext*>(bt)) {
        auto* qn = named->qualifiedName();
        std::string tn = qn->getText();

        // ---- 预置 Action/Func 泛型函数类型别名（v0.20.0，C# 风格）----
        if (qn->IDENT().size() == 1 && (tn == "Action" || tn == "Func")) {
            auto* ta = named->typeArgs();
            std::vector<TypePtr> args;
            if (ta) {
                for (auto* at : ta->typeArg()) {
                    if (at->QUESTION()) {
                        if (at->EXTENDS() && at->type())
                            args.push_back(Type::makeWildcardExtends(
                                resolveType(env, at->type())));
                        else if (at->SUPER() && at->type())
                            args.push_back(Type::makeWildcardSuper(
                                resolveType(env, at->type())));
                        else
                            args.push_back(Type::makeWildcard());
                    } else if (at->type())
                        args.push_back(resolveType(env, at->type()));
                    else
                        args.push_back(Type::makeUnknown());
                }
            }
            if (tn == "Action") {
                base = Type::makeFunc(std::move(args), Type::makeUnit());
            } else {  // Func
                if (args.empty()) {
                    if (env.error)
                        env.error(
                            t,
                            "Func 至少需要一个类型实参（最后一个为返回类型），"
                            "如 Func<Int,String>");
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
        if (qn->IDENT().size() == 1 && env.delegates && env.currentPkgPrefix) {
            auto dit = env.delegates->find(*env.currentPkgPrefix + tn);
            if (dit == env.delegates->end()) dit = env.delegates->find(tn);
            if (dit != env.delegates->end()) {
                base = dit->second;
                goto resolvedNamed;
            }
        }

        // ---- 类型参数优先 ----
        if (env.currentTypeParams && env.currentTypeParams->count(tn)) {
            base = Type::makeTypeParam(tn);
            if (env.currentSubst)
                base = substType(base, *env.currentSubst);
        } else if (qn->IDENT().size() == 1) {
            if (TypePtr builtin = typeFromName(tn)) {
                base = builtin;
            } else {
                bool iface = false;
                std::string internal = resolveTypeQualifiedName(env, qn, &iface);
                if (!internal.empty()) {
                    base = iface ? (TypePtr)Type::makeInterface(internal)
                                 : (TypePtr)Type::makeClass(internal);
                } else {
                    base = Type::makeClass(tn);
                }
            }
        } else {
            bool iface = false;
            std::string internal = resolveTypeQualifiedName(env, qn, &iface);
            if (!internal.empty()) {
                base = iface ? (TypePtr)Type::makeInterface(internal)
                             : (TypePtr)Type::makeClass(internal);
            } else {
                base = Type::makeClass(tn);
            }
        }

        // ---- 泛型实参 Box<Int> / Map<?,?> ----
        if (auto* ta = named->typeArgs()) {
            std::vector<TypePtr> args;
            for (auto* at : ta->typeArg()) {
                if (at->QUESTION()) {
                    if (at->EXTENDS() && at->type())
                        args.push_back(Type::makeWildcardExtends(
                            resolveType(env, at->type())));
                    else if (at->SUPER() && at->type())
                        args.push_back(Type::makeWildcardSuper(
                            resolveType(env, at->type())));
                    else
                        args.push_back(Type::makeWildcard());
                } else if (at->type()) {
                    args.push_back(resolveType(env, at->type()));
                } else {
                    args.push_back(Type::makeUnknown());
                }
            }
            bool hasWild = false;
            for (const auto& a : args)
                if (a->kind == TypeKind::Wildcard) {
                    hasWild = true;
                    break;
                }
            if (base->kind == TypeKind::Class) {
                if (!hasWild && !base->hasTypeParam() && env.instantiateClass)
                    env.instantiateClass(base->className, args, t);
                base = Type::makeClass(base->className, args);
            } else if (base->kind == TypeKind::Interface) {
                base = Type::makeInterface(base->className, args);
                if (!hasWild && !base->hasTypeParam() &&
                    env.ensureInterfaceInstances)
                    env.ensureInterfaceInstances(base);
            }
        } else if (base->kind == TypeKind::Interface) {
            // 裸接口名 Map：登记待用；typeids 走类型族
        }
    } else if (auto* arr = dynamic_cast<HaoLangParser::ArrayTypeContext*>(bt)) {
        base = Type::makeArray(resolveType(env, arr->type()));
    } else if (auto* ft = dynamic_cast<HaoLangParser::FuncTypeContext*>(bt)) {
        std::vector<TypePtr> ps;
        if (auto* tl = ft->typeList())
            for (auto* tc : tl->type())
                ps.push_back(resolveType(env, tc));
        base = Type::makeFunc(std::move(ps), resolveType(env, ft->type()));
    }

resolvedNamed:
    if (t->QUESTION()) {
        if (base->kind == TypeKind::Func) {
            if (env.error)
                env.error(t,
                          "不支持可空函数类型（Func?/Action?），请用非空 Func "
                          "或可空包装对象");
            return Type::makeUnknown();
        }
        base = base->asNullable();
    }
    return base;
}

}  // namespace hao
