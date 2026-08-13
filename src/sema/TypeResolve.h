// ============================================================
//  Sema 类型解析（G2 首刀）：从 IRGen 迁出 resolveType / 限定名查询
//  IRGen 仅组装只读环境并调用；禁止在 IRGen.cpp 再嵌实现体。
// ============================================================
#pragma once

#include "HaoLangParser.h"
#include "sema/Type.h"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace hao {

struct TypeResolveImport {
    std::string importPath;
    std::string alias;
    bool wildcard = false;
};

/** 类型解析只读查询 + 必要副作用回调（实例化 / 报错） */
struct TypeResolveEnv {
    const std::string* currentPkgPrefix = nullptr;
    const std::vector<TypeResolveImport>* currentImports = nullptr;
    const std::map<std::string, std::map<std::string, std::string>>* pkgExports =
        nullptr;
    const std::map<std::string, TypePtr>* delegates = nullptr;
    const std::set<std::string>* currentTypeParams = nullptr;
    const TypeSubst* currentSubst = nullptr;

    std::function<bool(const std::string& internal)> hasClass;
    std::function<bool(const std::string& internal)> hasInterface;
    std::function<void(const std::string& templateName,
                       const std::vector<TypePtr>& args,
                       antlr4::ParserRuleContext* useSite)>
        instantiateClass;
    std::function<void(const TypePtr& t)> ensureInterfaceInstances;
    std::function<void(antlr4::ParserRuleContext* ctx, const std::string& msg)>
        error;
};

std::string resolveTypeQualifiedName(const TypeResolveEnv& env,
                                     HaoLangParser::QualifiedNameContext* qn,
                                     bool* isIface);

TypePtr resolveType(const TypeResolveEnv& env, HaoLangParser::TypeContext* t);

}  // namespace hao
