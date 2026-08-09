// ============================================================
//  HaoLang IR 生成 —— 类 / 接口 / 继承 / 虚表 / 泛型类单态化
// ------------------------------------------------------------
//  从 IRGen.cpp 拆分而来，逻辑保持不变。涵盖：
//    - 类与接口的签名收集（字段槽位、方法、实现关系）
//    - 继承链解析、接口实现校验、虚表与 typeid 列表生成
//    - 泛型类单态化（Box<T> -> Box$Int）与待实例化队列
//    - 类/方法/构造函数的代码生成
//  顶层编排（generate）、函数体、类型可赋值性判断仍在 IRGen.cpp。
// ============================================================

#include "irgen/IRGen.h"

#include "util/StringUtil.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <set>

namespace hao {

namespace {
// 有虚表的类，字段槽位整体后移一位（槽位 0 留给虚表指针）。
// 须在代码生成前调用（genConstructor/genMethod 用偏移后的槽位写字段）。
void vtableShiftFields(ClassInfo& ci) {
    if (!ci.hasVTable) return;
    for (auto& f : ci.fields) f.slot += 1;
}
} // namespace

// declIsPrivate 实现在 IRGen.cpp（静态成员）

// 阶段 A：只登记接口名（内部名加包前缀），不解析方法。
// 这样类/字段在阶段 B 引用任意包的接口类型时都能找到。
void IRGen::registerInterfaceNames(const SourceUnit& u) {
    for (auto* decl : u.tree->topLevelDecl()) {
        auto* itf = decl->interfaceDecl();
        if (!itf) continue;

        std::string shortName = itf->IDENT()->getText();
        std::string iname = currentPkgPrefix_ + shortName;
        if (interfaces_.count(iname) || classes_.count(iname)) {
            error(itf, "接口 '" + shortName + "' 与已有类型重名");
            continue;
        }

        auto ii = std::make_shared<InterfaceInfo>();
        ii->name = iname;
        ii->declNode = itf;
        // 泛型接口：登记 typeParams（阶段 A，AST 节点可用），本条目即模板
        if (itf->typeParams())
            for (auto* tp : itf->typeParams()->IDENT())
                ii->typeParams.push_back(tp->getText());
        interfaces_[iname] = ii;

        // 在全局符号表登记接口名，使其可用作类型标注
        auto sym = std::make_shared<Symbol>();
        sym->kind = SymbolKind::Class;
        sym->name = iname;
        sym->type = Type::makeInterface(iname);
        sym->line = itf->getStart()->getLine();
        syms_.declareGlobal(sym);

        // 登记到包导出表（private 不导出）
        if (!declIsPrivate(itf->modifier()))
            pkgExports_[u.importPath][shortName] = iname;
    }
}

// 阶段 B：解析接口方法签名
void IRGen::collectInterfaceMembers(const SourceUnit& u) {
    for (auto* decl : u.tree->topLevelDecl()) {
        auto* itf = decl->interfaceDecl();
        if (!itf) continue;

        std::string shortName = itf->IDENT()->getText();
        std::string iname = currentPkgPrefix_ + shortName;
        auto ii = lookupInterface(iname);
        if (!ii) continue;   // 阶段 A 已报错

        if (itf->typeList())
            error(itf, "当前版本尚不支持接口继承接口");

        // 泛型接口：解析方法签名时须让 T 解析为 TypeParam（而非同名类）。
        // 若声明了 typeParams 但未在阶段 A 记录（理论不可能），此处兜底补齐。
        if (itf->typeParams() && ii->typeParams.empty())
            for (auto* tp : itf->typeParams()->IDENT())
                ii->typeParams.push_back(tp->getText());

        std::set<std::string> savedParams = currentTypeParams_;
        std::set<std::string> ifaceParams;
        for (const auto& p : ii->typeParams) ifaceParams.insert(p);
        currentTypeParams_ = savedParams;
        for (const auto& p : ifaceParams) currentTypeParams_.insert(p);

        for (auto* mem : itf->interfaceMember()) {
            if (mem->propertyDecl()) {
                error(mem, "当前版本尚不支持接口中的属性");
                continue;
            }
            if (!mem->IDENT()) continue;

            MethodInfo mi;
            mi.name = mem->IDENT()->getText();
            mi.returnType = mem->returnType()
                ? resolveType(mem->returnType()->type())
                : Type::makeUnit();
            mi.isAbstract = (mem->block() == nullptr);
            if (!mi.isAbstract)
                error(mem, "当前版本尚不支持接口方法的默认实现");

            if (auto* pl = mem->paramList()) {
                for (auto* p : pl->param()) {
                    mi.paramNames.push_back(p->IDENT()->getText());
                    mi.paramTypes.push_back(resolveType(p->type()));
                }
            }

            if (ii->findMethod(mi.name)) {
                error(mem, "方法 '" + mi.name + "' 在接口 '" + shortName + "' 中重复声明");
                continue;
            }
            mi.vtableSlot = static_cast<int>(ii->methods.size());
            ii->methods.push_back(mi);
        }

        currentTypeParams_ = savedParams;
    }
}

InterfaceInfoPtr IRGen::lookupInterface(const std::string& name) {
    auto it = interfaces_.find(name);
    return it == interfaces_.end() ? nullptr : it->second;
}

// ============================================================
//  泛型接口单态化
// ------------------------------------------------------------
//  为 (泛型接口模板, 实参组合) 生成一个具体接口实例（Iterable$Int）。
//  模板方法按 substType 把 T 替换为实参；虚表槽位**复制模板的**（不新分配），
//  因为同一泛型接口各实例方法名相同，槽位必然一致——由此绕开
//  「实例何时创建、槽位何时分配」的时序问题。
//  幂等：同名实例已存在直接返回。
// ============================================================
InterfaceInfoPtr IRGen::instantiateInterface(const std::string& templateName,
                                             const std::vector<TypePtr>& args,
                                             antlr4::ParserRuleContext* useSite) {
    auto tmplIt = interfaces_.find(templateName);
    if (tmplIt == interfaces_.end()) {
        if (useSite) error(useSite, "未定义的接口 '" + templateName + "'");
        return nullptr;
    }
    InterfaceInfoPtr tmpl = tmplIt->second;

    if (!tmpl->isGenericTemplate()) {
        if (useSite)
            error(useSite, "接口 '" + templateName + "' 不是泛型接口，不能提供类型参数");
        return nullptr;
    }
    if (args.size() != tmpl->typeParams.size()) {
        if (useSite)
            error(useSite, "泛型接口 '" + templateName + "' 需要 " +
                           std::to_string(tmpl->typeParams.size()) +
                           " 个类型参数，实际提供 " + std::to_string(args.size()) + " 个");
        return nullptr;
    }
    for (const auto& a : args) {
        if (a->hasTypeParam()) {
            if (useSite)
                error(useSite, "无法用未确定的类型参数 '" + a->toString() +
                               "' 实例化 '" + templateName + "'");
            return nullptr;
        }
    }

    // ---- 已实例化则直接复用 ----
    std::string instName = Type::makeInterface(templateName, args)->monoName();
    auto exist = interfaces_.find(instName);
    if (exist != interfaces_.end()) return exist->second;

    // ---- 建立替换表并复制模板方法（槽位继承模板）----
    TypeSubst subst;
    for (size_t i = 0; i < tmpl->typeParams.size(); ++i)
        subst[tmpl->typeParams[i]] = args[i];

    auto inst = std::make_shared<InterfaceInfo>();
    inst->name       = instName;
    inst->instanceOf = templateName;
    inst->typeArgs   = args;
    inst->declNode   = tmpl->declNode;

    // 先登记再递归实例化方法签名中引用的泛型接口（防自引用无限递归）
    interfaces_[instName] = inst;

    for (const auto& m : tmpl->methods) {
        MethodInfo im = m;
        im.paramTypes.clear();
        for (const auto& pt : m.paramTypes) im.paramTypes.push_back(substType(pt, subst));
        im.returnType = m.returnType ? substType(m.returnType, subst) : nullptr;

        // 连带实例化签名中引用的泛型接口（如 iterator(): Iterator<T>）。
        // 须在 push 之前调用（std::move(im) 后 im.returnType 已为空）。
        for (const auto& pt : im.paramTypes) ensureInterfaceInstances(pt);
        ensureInterfaceInstances(im.returnType);

        // 槽位继承模板（assignVtableSlots 已给模板分配全局槽位）
        inst->methods.push_back(std::move(im));
    }

    return inst;
}

// 递归实例化类型中引用的泛型接口（幂等；instantiateInterface 内部缓存避免重复）
void IRGen::ensureInterfaceInstances(const TypePtr& t) {
    if (!t) return;
    if (t->kind == TypeKind::Interface && !t->typeArgs.empty()) {
        std::string iname = t->monoName();
        if (!interfaces_.count(iname))
            instantiateInterface(t->className, t->typeArgs, nullptr);
        return;   // 接口实例内部不再直接展开（instantiateInterface 已递归其方法）
    }
    if (t->elem) ensureInterfaceInstances(t->elem);
    for (const auto& a : t->typeArgs) ensureInterfaceInstances(a);
    for (const auto& p : t->params)   ensureInterfaceInstances(p);
}

// 遍历登记到的泛型接口待实例化项，逐个实例化。
// 必须在 assignVtableSlots 之后（模板槽位已分配）调用。
void IRGen::instantiateAllGenericInterfaces() {
    std::vector<TypePtr> items;
    for (auto& [k, t] : pendingGenericInterfaces_) items.push_back(t);
    pendingGenericInterfaces_.clear();
    for (auto& t : items)
        instantiateInterface(t->className, t->typeArgs, nullptr);
}
// ============================================================
//  泛型单态化
// ------------------------------------------------------------
//  为 (模板, 实参组合) 生成一个具体类。例如 Box<Int> 生成名为
//  Box$Int 的普通类，其成员类型中的 T 全部换成 Int。
//
//  实例化结果缓存在 classes_ 中，同一组合只做一次。
//  生成的实例进入 pendingInstances_ 队列，代码在遍历完全部
//  顶层声明后统一产出（因为实例化可能发生在任意位置，
//  且自身可能触发新的实例化，如 Box<Box<Int>>）。
// ============================================================
ClassInfoPtr IRGen::instantiateClass(const std::string& templateName,
                                     const std::vector<TypePtr>& args,
                                     antlr4::ParserRuleContext* useSite) {
    auto tmplIt = classes_.find(templateName);
    if (tmplIt == classes_.end()) {
        if (useSite) error(useSite, "未定义的泛型类 '" + templateName + "'");
        return nullptr;
    }
    ClassInfoPtr tmpl = tmplIt->second;

    if (!tmpl->isGenericTemplate()) {
        if (useSite)
            error(useSite, "类 '" + templateName + "' 不是泛型类，不能提供类型参数");
        return nullptr;
    }
    if (args.size() != tmpl->typeParams.size()) {
        if (useSite)
            error(useSite, "泛型类 '" + templateName + "' 需要 " +
                           std::to_string(tmpl->typeParams.size()) +
                           " 个类型参数，实际提供 " + std::to_string(args.size()) + " 个");
        return nullptr;
    }
    // 实参本身不能再含未替换的类型参数
    for (const auto& a : args) {
        if (a->hasTypeParam()) {
            if (useSite)
                error(useSite, "无法用未确定的类型参数 '" + a->toString() +
                               "' 实例化 '" + templateName + "'");
            return nullptr;
        }
    }

    // ---- 已实例化则直接复用 ----
    TypePtr probe = Type::makeClass(templateName, args);
    std::string instName = probe->monoName();
    auto exist = classes_.find(instName);
    if (exist != classes_.end()) return exist->second;

    // ---- 建立替换表 ----
    TypeSubst subst;
    for (size_t i = 0; i < tmpl->typeParams.size(); ++i)
        subst[tmpl->typeParams[i]] = args[i];

    auto* cls = static_cast<HaoLangParser::ClassDeclContext*>(tmpl->declNode);
    if (!cls) return nullptr;

    auto inst = std::make_shared<ClassInfo>();
    inst->name       = instName;
    inst->importPath = tmpl->importPath;
    inst->instanceOf = templateName;
    inst->typeArgs   = args;
    inst->line       = tmpl->line;
    inst->column     = tmpl->column;
    inst->declNode   = cls;

    // 先登记再解析成员：模板若自引用（如 func next(): Box<T>），
    // 递归实例化时能命中缓存而不至于无限递归
    classes_[instName] = inst;

    // ---- 在替换表生效的上下文中解析成员 ----
    //  同时把包前缀切到模板所属包：泛型类成员内可能引用同包类型的裸名
    //  （如 List<T> 的 filter 返回 List<T>），必须用模板前缀而非调用点前缀。
    auto savedSubst  = currentSubst_;
    auto savedParams = currentTypeParams_;
    auto savedPrefix = currentPkgPrefix_;
    currentSubst_ = subst;
    currentTypeParams_.clear();
    for (const auto& p : tmpl->typeParams) currentTypeParams_.insert(p);
    {
        size_t dp = templateName.find_last_of('$');
        currentPkgPrefix_ = (dp == std::string::npos) ? ""
                            : templateName.substr(0, dp + 1);
    }

    for (auto* mod : cls->modifier())
        if (mod->ABSTRACT()) inst->isAbstract = true;

    // 基类与接口，支持 `: list` 与 `extends/extends implements/implements` 三种写法。
    // 泛型接口（Iterable<T>）在实例化时 T 已被替换为实参，故直接实例化实例并记实例名。
    auto addBase = [&](const TypePtr& rt) {
        if (!rt) return;
        std::string tn = rt->className;
        if (rt->kind == TypeKind::Interface) {
            if (!rt->typeArgs.empty())
                instantiateInterface(tn, rt->typeArgs, cls);
            inst->interfaceNames.push_back(
                rt->typeArgs.empty() ? tn
                                     : Type::makeInterface(tn, rt->typeArgs)->monoName());
        } else if (!tn.empty() && inst->baseName.empty()) {
            inst->baseName = tn;
        }
    };
    if (auto* cb = cls->classBase()) {
        if (auto* eb = dynamic_cast<HaoLangParser::ExtendsBaseContext*>(cb)) {
            addBase(resolveType(eb->type()));
            if (auto* il = eb->typeList())
                for (auto* t : il->type()) addBase(resolveType(t));
        } else if (auto* ib = dynamic_cast<HaoLangParser::ImplementsBaseContext*>(cb)) {
            if (auto* il = ib->typeList())
                for (auto* t : il->type()) addBase(resolveType(t));
        } else if (auto* cob = dynamic_cast<HaoLangParser::ColonBaseContext*>(cb)) {
            if (auto* tl = cob->typeList())
                for (auto* t : tl->type()) addBase(resolveType(t));
        }
    }
    // 泛型实例默认继承 Object（v0.15.0）
    if (inst->baseName.empty()) inst->baseName = "object$Object";

    int slot = 0;
    for (auto* mem : cls->classMember()) {
        // ---- 字段 ----
        if (auto* fd = mem->fieldDecl()) {
            FieldInfo fi;
            fi.name       = fd->IDENT()->getText();
            fi.isMutable  = (fd->VAR() != nullptr);
            fi.ownerClass = instName;
            fi.line       = fd->getStart()->getLine();
            fi.column     = fd->getStart()->getCharPositionInLine();
            for (auto* mod : fd->modifier()) {
                if (mod->STATIC())         fi.isStatic = true;
                else if (mod->PRIVATE())   fi.visibility = FieldInfo::Vis::Private;
                else if (mod->PROTECTED()) fi.visibility = FieldInfo::Vis::Protected;
                else if (mod->INTERNAL())  fi.visibility = FieldInfo::Vis::Internal;
            }

            if (fd->type()) {
                fi.type = resolveType(fd->type());   // 已施加替换
            } else if (fd->expr()) {
                fi.type = inferExprType(fd->expr());
                if (!fi.type || fi.type->isUnknown()) fi.type = Type::makeInt();
            } else {
                fi.type = Type::makeInt();
            }
            if (fi.isStatic) {
                fi.defaultExpr = fd->expr();
                inst->staticFields.push_back(fi);
                continue;
            }
            fi.slot = slot++;
            fi.defaultExpr = fd->expr();
            inst->fields.push_back(fi);
            continue;
        }

        // ---- 方法 ----
        if (auto* fn = mem->funcDecl()) {
            // 泛型方法：登记模板到 genericMethods_（key = 类模板名.方法名），
            // 实例类不收集它；调用时按「类实例名.方法名$R」单态化。
            // 泛型类模板的成员在这里解析（collectClassMembers 跳过模板），
            // 故模板登记放在此处，覆盖幂等。
            if (fn->typeParams()) {
                GenericMethod gm;
                gm.className = templateName;
                gm.methodName = fn->IDENT()->getText();
                gm.pkgPrefix = currentPkgPrefix_;
                gm.decl = fn;
                for (auto* id : fn->typeParams()->IDENT())
                    gm.typeParams.push_back(id->getText());
                genericMethods_[templateName + "." + gm.methodName] = gm;
                continue;
            }
            MethodInfo mi;
            mi.name       = fn->IDENT()->getText();
            mi.ownerClass = instName;
            mi.line       = fn->getStart()->getLine();
            mi.column     = fn->getStart()->getCharPositionInLine();
            mi.returnType = fn->returnType()
                ? resolveType(fn->returnType()->type())
                : Type::makeUnit();

            for (auto* mod : fn->modifier()) {
                if (mod->STATIC())         mi.isStatic = true;
                else if (mod->OVERRIDE())  mi.isOverride = true;
                else if (mod->ABSTRACT())  mi.isAbstract = true;
                else if (mod->PRIVATE())   mi.visibility = MethodInfo::Vis::Private;
                else if (mod->PROTECTED()) mi.visibility = MethodInfo::Vis::Protected;
                else if (mod->INTERNAL())  mi.visibility = MethodInfo::Vis::Internal;
            }
            if (auto* pl = fn->paramList()) {
                for (auto* p : pl->param()) {
                    mi.paramNames.push_back(p->IDENT()->getText());
                    mi.paramTypes.push_back(resolveType(p->type()));
                }
            }
            if (mi.isStatic) {
                bool needsSuffix = !inst->findStaticMethods(mi.name).empty();
                mi.irName = staticMethodIRName(instName, mi.name, mi.paramTypes,
                                               needsSuffix);
                inst->staticMethods.push_back(mi);
                continue;
            }
            // 实例名已含类型参数，故方法符号名天然唯一
            mi.irName = "@" + instName + "." + mi.name;
            inst->methods.push_back(mi);
            continue;
        }

        // ---- 构造函数 ----
        if (auto* ctor = mem->constructorDecl()) {
            inst->ctorIRName = "@" + instName + ".ctor";
            if (auto* pl = ctor->paramList()) {
                for (auto* p : pl->param()) {
                    inst->ctorParamNames.push_back(p->IDENT()->getText());
                    inst->ctorParamTypes.push_back(resolveType(p->type()));
                }
            }
            continue;
        }

        // 静态构造器（泛型类静态构造器少见，但保持一致地登记）
        if (auto* sc = mem->staticCtorDecl()) {
            if (sc->IDENT()->getText() != instName) {
                error(sc, "静态构造器名必须与类名一致，应为 '" + instName + "'");
                continue;
            }
            if (sc->paramList()) {
                error(sc, "静态构造器不能有参数");
                continue;
            }
            if (inst->staticCtorNode) {
                error(sc, "类 '" + instName + "' 只能有一个静态构造器");
                continue;
            }
            inst->staticCtorNode = sc;
            continue;
        }

        if (mem->propertyDecl())
            error(mem, "当前版本尚不支持自动属性（get/set）");
    }

    currentSubst_      = savedSubst;
    currentTypeParams_ = savedParams;
    currentPkgPrefix_  = savedPrefix;

    // 泛型实例与普类一致：一律虚表（Object.getClass / Class 令牌 / is/as）
    inst->hasVTable = true;
    inst->vtableIRName = "@" + instName + ".vtable";
    // 有虚表则字段槽位整体后移一位（槽位 0 留给虚表指针）。
    // 须在 genPendingInstantiations 生成 ctor/method 之前完成。
    vtableShiftFields(*inst);
    ensureClassStaticField(inst);

    pendingInstances_.push_back(inst);
    return inst;
}

// ============================================================
//  生成所有泛型实例的代码
// ------------------------------------------------------------
//  实例化可能发生在任意位置，且实例化过程本身会触发新的实例化
//  （如 Box<Box<Int>>），因此用队列迭代直到不再产生新实例。
void IRGen::genPendingInstantiations() {
    // 每轮取出当前队列全部内容处理；处理过程中新入队的实例
    // 会在下一轮被处理。
    std::set<std::string> generated;
    while (!pendingInstances_.empty()) {
        auto batch = pendingInstances_;
        pendingInstances_.clear();

        for (auto& inst : batch) {
            if (generated.count(inst->name)) continue;
            generated.insert(inst->name);

            auto* cls = static_cast<HaoLangParser::ClassDeclContext*>(inst->declNode);
            if (!cls) continue;

            // 重建替换表：模板类型参数 -> 本实例的实参
            auto tmpl = lookupClass(inst->instanceOf);
            if (!tmpl) continue;
            TypeSubst subst;
            for (size_t i = 0; i < tmpl->typeParams.size() &&
                               i < inst->typeArgs.size(); ++i)
                subst[tmpl->typeParams[i]] = inst->typeArgs[i];

            auto savedSubst  = currentSubst_;
            auto savedParams = currentTypeParams_;
            auto savedPrefix = currentPkgPrefix_;
            auto savedImports = currentImports_;
            auto savedImportPath = currentImportPath_;
            currentSubst_ = subst;
            // 模板名形如 "calc$Box"（非 main 包），包前缀是最后一个 '$'
            // 之前的部分（含 '$'）；main 包模板名无 '$'，前缀为空。
            {
                size_t dp = tmpl->name.find_last_of('$');
                currentPkgPrefix_ = (dp == std::string::npos) ? ""
                                    : tmpl->name.substr(0, dp + 1);
            }
            restoreImportsForPkgPrefix(currentPkgPrefix_);
            currentTypeParams_.clear();
            for (const auto& p : tmpl->typeParams) currentTypeParams_.insert(p);

            em_.emitBlank();
            em_.emitRaw("; ======== 泛型实例 " + inst->name +
                        "  (来自 " + inst->instanceOf + ") ========");
            em_.emitRaw("; 字段布局:");
            if (inst->hasVTable) em_.emitRaw(";   [0] <vtable>");
            for (const auto& f : inst->fields)
                em_.emitRaw(";   [" + std::to_string(f.slot) + "] " +
                            f.name + " : " + f.type->toString());

            emitStaticFieldGlobals(inst);
            genStaticConstructor(inst);

            for (auto* mem : cls->classMember()) {
                if (auto* ctor = mem->constructorDecl()) genConstructor(ctor, inst);
                else if (auto* fn = mem->funcDecl()) {
                    // 泛型方法由 genPendingMethodInstances 单独生成
                    if (fn->typeParams()) continue;
                    const MethodInfo* mi = inst->findMethod(fn->IDENT()->getText());
                    if (mi && mi->isAbstract) continue;
                    genMethod(fn, inst);
                }
            }

            currentSubst_      = savedSubst;
            currentTypeParams_ = savedParams;
            currentPkgPrefix_  = savedPrefix;
            currentImports_    = savedImports;
            currentImportPath_ = savedImportPath;
        }
    }
}

// ------------------------------------------------------------
//  解析继承链
// ------------------------------------------------------------
//  三件事：
//    1. 把 baseName 解析为 base 指针，并检测继承环
//    2. 按拓扑顺序扁平化字段：父类字段在前，子类新增在后，
//       使父类字段在子类中的槽位保持不变
//    3. 扁平化方法：继承父类方法（虚表项仍指向父类实现），
void IRGen::resolveInheritance() {
    // ---- 1. 连接 base 指针 ----
    //  泛型模板没有解析过的成员，不参与继承处理；
    //  其实例作为普通类正常参与。
    for (auto& [cname, ci] : classes_) {
        if (ci->isGenericTemplate()) continue;
        if (ci->baseName.empty()) continue;
        auto it = classes_.find(ci->baseName);
        if (it == classes_.end()) {
            if (interfaces_.count(ci->baseName)) {
                // 已在收集阶段归入 interfaceNames，不该到这里
                ci->baseName.clear();
            } else {
                diags_.error(ci->line, ci->column,
                             "未定义的基类 '" + ci->baseName + "'");
                ci->baseName.clear();
            }
            continue;
        }
        ci->base = it->second.get();
    }

    // ---- 2. 检测继承环 ----
    for (auto& [cname, ci] : classes_) {
        if (ci->isGenericTemplate()) continue;
        // 沿链上行，步数超过类总数即说明有环
        size_t steps = 0;
        for (ClassInfo* c = ci->base; c; c = c->base) {
            if (c == ci.get() || ++steps > classes_.size()) {
                diags_.error(ci->line, ci->column,
                             "类 '" + cname + "' 的继承关系存在循环");
                ci->base = nullptr;
                ci->baseName.clear();
                break;
            }
        }
    }

    // ---- 3. 按继承深度排序，保证处理子类时父类已扁平化 ----
    std::vector<ClassInfo*> order;
    order.reserve(classes_.size());
    for (auto& [cname, ci] : classes_)
        if (!ci->isGenericTemplate()) order.push_back(ci.get());

    auto depthOf = [](const ClassInfo* c) {
        size_t d = 0;
        for (const ClassInfo* p = c->base; p; p = p->base) ++d;
        return d;
    };
    std::sort(order.begin(), order.end(),
              [&](const ClassInfo* a, const ClassInfo* b) {
                  return depthOf(a) < depthOf(b);
              });

    for (ClassInfo* ci : order) {
        if (!ci->base) continue;
        ClassInfo* base = ci->base;

        // ---- 字段：父类在前，子类新增顺延 ----
        std::vector<FieldInfo> merged = base->fields;   // 已含祖父类字段
        for (auto& f : ci->fields) {
            // 子类不允许与父类字段同名（Java 允许遮蔽，但易出错，这里禁止）
            bool dup = false;
            for (const auto& bf : merged) {
                if (bf.name == f.name) {
                    diags_.error(f.line, f.column,
                                 "字段 '" + f.name + "' 与基类 '" +
                                 bf.ownerClass + "' 中的字段重名");
                    dup = true;
                    break;
                }
            }
            if (dup) continue;
            f.slot = static_cast<int>(merged.size());   // 追加在父类字段之后
            merged.push_back(f);
        }
        ci->fields = std::move(merged);

        // ---- 方法：继承 + 覆写 ----
        std::vector<MethodInfo> mergedM = base->methods;
        for (auto& m : ci->methods) {
            auto it = std::find_if(mergedM.begin(), mergedM.end(),
                                   [&](const MethodInfo& x) { return x.name == m.name && x.signatureMatches(m); });
            if (it == mergedM.end()) {
                // 本类新增的方法。
                // 标了 override 但基类没有同名方法时，它可能是在实现接口
                // （接口实现同样允许写 override），故只在既非基类方法、
                // 也非任何已声明接口的方法时才报错。
                if (m.isOverride) {
                    bool fromInterface = false;
                    for (const ClassInfo* c = ci; c && !fromInterface; c = c->base)
                        for (const auto& iname : c->interfaceNames) {
                            auto ii = lookupInterface(iname);
                            if (ii && ii->findMethod(m.name)) { fromInterface = true; break; }
                        }
                    if (!fromInterface)
                        diags_.error(m.line, m.column,
                                     "方法 '" + m.name + "' 标记了 override，"
                                     "但基类与所实现的接口中都没有同名方法");
                }
                mergedM.push_back(m);
                continue;
            }

            // 覆写：签名必须一致
            if (!m.signatureMatches(*it)) {
                diags_.error(m.line, m.column,
                             "方法 '" + m.name + "' 的签名 " + m.signatureString() +
                             " 与基类 '" + it->ownerClass + "' 中的 " +
                             it->signatureString() + " 不一致");
                continue;
            }
            // 覆写非抽象方法必须显式写 override（对标 C#，比 Java 更严格，
            // 可避免因拼写巧合造成的意外覆写）
            if (!m.isOverride && !it->isAbstract) {
                diags_.error(m.line, m.column,
                             "方法 '" + m.name + "' 覆写了基类 '" + it->ownerClass +
                             "' 的方法，必须显式标记 override");
            }
            // 保留父类分配的虚表槽位，替换实现
            m.vtableSlot = it->vtableSlot;
            *it = m;
        }
        ci->methods = std::move(mergedM);

        // 非抽象类必须实现所有继承来的抽象方法
        if (!ci->isAbstract) {
            for (const auto& m : ci->methods) {
                if (m.isAbstract)
                    diags_.error(ci->line, ci->column,
                                 "类 '" + ci->name + "' 必须实现继承的抽象方法 " +
                                 m.name + m.signatureString() +
                                 "（或声明为 abstract）");
            }
        }
    }
}

// ------------------------------------------------------------
//  校验接口实现并分配虚表
// ------------------------------------------------------------
//  槽位采用「全局编号」：按接口的声明顺序、接口内方法的声明顺序，
//  为每个 (接口, 方法) 组合分配一个唯一编号。所有类的虚表都按这个
//  统一编号布局。
//
//  这样做的关键好处：接口方法调用点只知道接口类型，不知道具体类，
//  但因为编号全局唯一，直接用该编号索引对象的虚表即可正确分派。
//  若改为每个接口内部从 0 编号，实现多接口的类就会发生槽位冲突
void IRGen::assignVtableSlots() {
    // ---- 第一步：为所有接口方法分配全局槽位 ----
    // interfaces_ 是 std::map，遍历顺序按名字排序，因此编号稳定可复现。
    // 泛型接口实例（isGenericInstance）共享模板槽位，不在此新分配；
    // 泛型接口模板（isGenericTemplate）在此分配，实例复制其槽位。
    int nextSlot = 0;
    for (auto& [iname, ii] : interfaces_) {
        if (ii->isGenericInstance()) continue;   // 实例继承模板槽位
        for (auto& m : ii->methods) m.vtableSlot = nextSlot++;
    }

    // ---- 第二步：为参与继承的虚方法分配槽位 ----
    //  凡是「在继承体系中声明、可能被覆写」的方法都需要槽位，
    //  否则父类型引用无法分派到子类实现。
    //
    //  关键：同一方法在整条继承链上必须共用同一槽位。因此 key 取
    //  「最初声明该方法的类」——不能用 ownerClass，因为覆写时它已被
    //  改写为子类名，会导致每个子类各分到一个新槽位，父类型调用时
    //  取到的槽位为 null。
    std::set<std::string> hasSubclass;
    for (auto& [cname, ci] : classes_)
        if (!ci->isGenericTemplate() && !ci->baseName.empty())
            hasSubclass.insert(ci->baseName);

    // 找出方法 mname 在类 c 的继承链上最早的声明者
    auto methodSlotKey = [&](const ClassInfo* c, const MethodInfo& m) {
        // irName is unique per overload (@Class.m / @Class.m$Int); use as slot key root
        // Walk bases for same signature to share slot on override
        std::string root = c->name;
        for (const ClassInfo* p = c; p; p = p->base)
            if (p->findMethod(m.name, m.paramTypes)) root = p->name;
        std::string leaf = m.name;
        auto pos = m.irName.rfind('.');
        if (pos != std::string::npos) leaf = m.irName.substr(pos + 1);
        return root + "." + leaf;
    };

    std::map<std::string, int> methodSlots;
    for (auto& [cname, ci] : classes_) {
        if (ci->isGenericTemplate()) continue;
        bool inHierarchy = hasSubclass.count(cname) > 0 || !ci->baseName.empty();
        if (!inHierarchy) continue;

        for (auto& m : ci->methods) {
            if (m.vtableSlot >= 0) continue;         // 接口方法已分配
            std::string key = methodSlotKey(ci.get(), m);
            if (!methodSlots.count(key)) methodSlots[key] = nextSlot++;
        }
    }
    // 回填：整条继承链上的同名方法使用同一槽位
    for (auto& [cname, ci] : classes_) {
        if (ci->isGenericTemplate()) continue;
        bool inHierarchy = hasSubclass.count(cname) > 0 || !ci->baseName.empty();
        if (!inHierarchy) continue;
        for (auto& m : ci->methods) {
            if (m.vtableSlot >= 0) continue;
            auto it = methodSlots.find(methodSlotKey(ci.get(), m));
            if (it != methodSlots.end()) m.vtableSlot = it->second;
        }
    }

    // 每个类至少需要 1 个虚表槽位（即使没有虚方法），因为虚表地址同时作为
    // 运行时类型标识（is/as/catch 匹配，以及未来精确式 GC 的类型头）。
    // 零长度全局常量可能被 LLVM 合并，导致不同类的虚表地址相同而误判。
    vtableTotalSlots_ = std::max(static_cast<size_t>(nextSlot), size_t(1));

    // 非泛型类统一获得虚表/类型标识，并据此偏移字段槽位。
    // （泛型实例的 hasVTable 在 instantiateClass 判定并偏移，见其 addBase 之后。）
    // 以上均须在代码生成（genUnitTopLevel/genPendingInstantiations）之前完成，
    // 否则 ctor/method 会按未偏移槽位写字段，覆盖虚表指针。
    for (auto& [cname, ci] : classes_) {
        if (ci->isGenericTemplate()) continue;
        ci->hasVTable = true;
        ci->vtableIRName = "@" + cname + ".vtable";
        vtableShiftFields(*ci);
    }
}

// 逐类校验接口实现并填充 vtableEntries。必须在全部泛型类实例生成之后调用
//（fillVtableEntries 遍历 classes_，含泛型实例，需其接口实例已实例化）。
void IRGen::fillVtableEntries() {
    const size_t totalSlots = vtableTotalSlots_;

    // ---- 逐类校验接口实现，并填充虚表 ----
    //  hasVTable / vtableIRName / 字段槽位偏移已在 assignVtableSlots 完成
    //  （须在代码生成前），此处只填充 vtableEntries。
    for (auto& [cname, ci] : classes_) {
        if (ci->isGenericTemplate()) continue;

        // 未被本类实现的槽位填 null；正确性由实现检查保证
        ci->vtableEntries.assign(totalSlots, "null");

        // 接口方法校验（沿继承链：父类实现的接口子类同样满足）
        for (const ClassInfo* c = ci.get(); c; c = c->base) {
            for (const auto& iname : c->interfaceNames) {
                auto ii = lookupInterface(iname);
                if (!ii) {
                    if (c == ci.get())
                        diags_.error(ci->line, ci->column,
                                     "类 '" + cname + "' 声明实现的 '" + iname +
                                     "' 不是接口");
                    continue;
                }
                for (const auto& im : ii->methods) {
                    MethodInfo* cm = ci->findMethodMut(im.name);
                    if (!cm) {
                        if (!ci->isAbstract)
                            diags_.error(ci->line, ci->column,
                                         "类 '" + cname + "' 未实现接口 '" + iname +
                                         "' 的方法 " + im.name + im.signatureString());
                        continue;
                    }
                    if (!cm->signatureMatches(im)) {
                        diags_.error(ci->line, ci->column,
                                     "类 '" + cname + "' 的方法 '" + im.name +
                                     "' 签名 " + cm->signatureString() +
                                     " 与接口 '" + iname + "' 声明的 " +
                                     im.signatureString() + " 不一致");
                        continue;
                    }
                    // 接口方法的全局槽位优先（可能与继承槽位不同，
                    // 此时两个槽位都填同一实现，保证两种调用路径都对）
                    ci->vtableEntries[static_cast<size_t>(im.vtableSlot)] = cm->irName;
                }
            }
        }

        // 类自身虚方法填表
        for (const auto& m : ci->methods) {
            if (m.vtableSlot < 0 || m.isAbstract) continue;
            size_t s = static_cast<size_t>(m.vtableSlot);
            if (s < ci->vtableEntries.size() && ci->vtableEntries[s] == "null")
                ci->vtableEntries[s] = m.irName;
        }
    }
}

// ------------------------------------------------------------
//  生成虚表全局常量
void IRGen::emitVTables() {
    for (auto& [cname, ci] : classes_) {
        if (ci->isGenericTemplate() || !ci->hasVTable) continue;

        // 追加一个指向该类的反射 meta 的哨兵槽。作用：
        //  1) 每个类的虚表内容因此唯一（含不同 meta 地址），避免链接器把
        //     内容相同的 constant 合并导致虚表地址不唯一（否则 is/as 与
        //     反射 getClass 都会误判类）；
        //  2) 运行时可经虚表反查类元数据。
        // 哨兵在数组末尾，虚表索引（slot < 原长度）不会读到它。
        std::string def = ci->vtableIRName +
            " = private unnamed_addr constant [" +
            std::to_string(ci->vtableEntries.size() + 1) + " x ptr] [";
        for (size_t i = 0; i < ci->vtableEntries.size(); ++i) {
            def += "ptr " + ci->vtableEntries[i] + ", ";
        }
        def += "ptr @" + cname + ".meta]";   // 哨兵：反射 meta 指针
        em_.addGlobal(def);
    }
}

// ------------------------------------------------------------
//  反射类型元数据（v0.19.0）：为每个类生成 HaoClassMeta 描述
// ------------------------------------------------------------
//  结构与 stdlib/runtime_reflect.c 中全 8 字节字段的 C 结构体一一对应，
//  LLVM IR 的 struct 类型与 C 布局一致（同一目标 ABI），零对齐差异。
//  每个类生成：字段/方法/注解/接口数组 + 单条 meta；全部注册进
//  @hao_all_metas（NULL 结尾），运行时从对象虚表指针反查。
void IRGen::emitClassMeta() {
    // 结构体类型定义（一次）
    static bool typesDeclared = false;
    if (!typesDeclared) {
        typesDeclared = true;
        em_.addGlobal("%r.HaoFieldMeta = type { ptr, ptr, i64, i64, i64, ptr }");
        // MethodMeta：+ annos ptr + annoCount（v0.29 方法级注解）
        em_.addGlobal("%r.HaoMethodMeta = type { ptr, ptr, ptr, i64, ptr, i64, ptr, ptr, i64 }");
        // AnnoMeta：name + value + annoClassMeta（v0.50 Class 令牌）
        em_.addGlobal("%r.HaoAnnoMeta  = type { ptr, ptr, ptr }");
        // ClassMeta：+ factory + ctorParamCount + ctorParamTypes + isAnnotation（v0.50）
        // + classMirror（v0.50.1 Class 单例；meta 须为 global 方可写入）
        em_.addGlobal("%r.HaoClassMeta = type { ptr, ptr, ptr, i64, ptr, i64, ptr, "
                      "i64, ptr, i64, ptr, i64, i64, ptr, i64, ptr, i64, ptr }");
    }

    auto visStrM = [](MethodInfo::Vis v, bool isStatic) {
        if (isStatic) return std::string("static");
        switch (v) {
            case MethodInfo::Vis::Private:   return std::string("private");
            case MethodInfo::Vis::Protected: return std::string("protected");
            case MethodInfo::Vis::Internal:  return std::string("internal");
            default:                         return std::string("public");
        }
    };
    auto visStrF = [](FieldInfo::Vis v, bool isStatic) {
        if (isStatic) return std::string("static");
        switch (v) {
            case FieldInfo::Vis::Private:   return std::string("private");
            case FieldInfo::Vis::Protected: return std::string("protected");
            case FieldInfo::Vis::Internal:  return std::string("internal");
            default:                        return std::string("public");
        }
    };

    std::vector<std::string> metaRefs;   // 收集 @<cls>_meta 供注册表
    for (auto& [cname, ci] : classes_) {
        if (ci->isGenericTemplate() || !ci->hasVTable) continue;

        // ---- 字段数组 ----
        std::string fieldsDef;
        std::vector<std::string> fieldStrs;
        for (size_t i = 0; i < ci->fields.size(); ++i) {
            const FieldInfo& f = ci->fields[i];
            if (f.isStatic && f.name.rfind("_", 0) == 0) continue; // 合成静态字段跳过
            std::string owner = f.ownerClass.empty() ? cname : f.ownerClass;
            std::string vis = visStrF(f.visibility, f.isStatic);
            fieldStrs.push_back("{ ptr " + em_.internString(f.name) + ", ptr " +
                                em_.internString(f.type->toString()) + ", i64 " +
                                std::to_string(f.slot) + ", i64 " +
                                (f.isStatic ? "1" : "0") + ", i64 " +
                                (f.isMutable ? "1" : "0") + ", ptr " +
                                em_.internString(vis) + " }");
        }
        if (fieldStrs.empty()) {
            fieldsDef = "@" + cname + ".meta_fields = private unnamed_addr constant "
                        "[0 x %r.HaoFieldMeta] zeroinitializer";
        } else {
            fieldsDef = "@" + cname + ".meta_fields = private unnamed_addr constant ["
                        + std::to_string(fieldStrs.size()) + " x %r.HaoFieldMeta] [";
            for (size_t i = 0; i < fieldStrs.size(); ++i) {
                if (i) fieldsDef += ", ";
                fieldsDef += "%r.HaoFieldMeta " + fieldStrs[i];
            }
            fieldsDef += "]";
        }
        em_.addGlobal(fieldsDef);

        // 注解参数编码：单值 → 裸字符串；多值 → key=val;key=val
        auto encodeAnnoValue = [](const AnnotationUse& a) -> std::string {
            if (a.args.empty()) return "";
            if (a.args.size() == 1 && a.args[0].first.empty())
                return a.args[0].second;
            std::string s;
            for (size_t i = 0; i < a.args.size(); ++i) {
                if (i) s += ";";
                if (a.args[i].first.empty()) s += a.args[i].second;
                else s += a.args[i].first + "=" + a.args[i].second;
            }
            return s;
        };
        auto emitAnnoArray = [&](const std::vector<AnnotationUse>& annos,
                                 const std::string& arrName) -> size_t {
            std::vector<std::string> elems;
            for (const auto& a : annos) {
                std::string metaPtr = "null";
                if (!a.className.empty() && classes_.count(a.className)) {
                    auto aci = classes_[a.className];
                    if (aci && aci->hasVTable)
                        metaPtr = "@" + a.className + ".meta";
                }
                elems.push_back("{ ptr " + em_.internString(a.name) + ", ptr " +
                                em_.internString(encodeAnnoValue(a)) +
                                ", ptr " + metaPtr + " }");
            }
            std::string def;
            if (elems.empty()) {
                def = arrName + " = private unnamed_addr constant "
                      "[0 x %r.HaoAnnoMeta] zeroinitializer";
            } else {
                def = arrName + " = private unnamed_addr constant [" +
                      std::to_string(elems.size()) + " x %r.HaoAnnoMeta] [";
                for (size_t i = 0; i < elems.size(); ++i) {
                    if (i) def += ", ";
                    def += "%r.HaoAnnoMeta " + elems[i];
                }
                def += "]";
            }
            em_.addGlobal(def);
            return elems.size();
        };

        // ---- 方法数组（含实例方法 + 静态方法，v0.20.0 起静态方法也入元数据供 invoke）----
        std::string methodsDef;
        std::vector<std::string> methodStrs;
        size_t mi = 0;   // 跨全部方法统一计数，用于 meta_m 数组名唯一
        auto emitOneMethod = [&](const MethodInfo& m) {
            if (m.ownerClass != cname) return;   // 只列本类声明的方法
            // 参数名数组
            std::vector<std::string> pids;
            for (const auto& pt : m.paramTypes) pids.push_back(em_.internString(pt->toString()));
            std::string paramArr;
            if (pids.empty()) {
                paramArr = "@" + cname + ".meta_m" + std::to_string(mi) +
                           " = private unnamed_addr constant [0 x ptr] zeroinitializer";
            } else {
                paramArr = "@" + cname + ".meta_m" + std::to_string(mi) +
                           " = private unnamed_addr constant [" +
                           std::to_string(pids.size()) + " x ptr] [ptr " + pids[0];
                for (size_t k = 1; k < pids.size(); ++k) paramArr += ", ptr " + pids[k];
                paramArr += "]";
            }
            em_.addGlobal(paramArr);
            std::string annoArr = "@" + cname + ".meta_m" + std::to_string(mi) + "_annos";
            size_t annoN = emitAnnoArray(m.annotations, annoArr);
            std::string vis = visStrM(m.visibility, m.isStatic);
            std::string invk = emitInvokeThunk(ci, m);   // 反射 invoke thunk（v0.20.0）
            methodStrs.push_back("{ ptr " + em_.internString(m.name) + ", ptr " +
                                 em_.internString(m.returnType->toString()) + ", ptr " +
                                 "@" + cname + ".meta_m" + std::to_string(mi) + ", i64 " +
                                 std::to_string(m.paramTypes.size()) + ", ptr " +
                                 em_.internString(vis) + ", i64 " +
                                 (m.isStatic ? "1" : "0") + ", ptr " + invk +
                                 ", ptr " + annoArr + ", i64 " +
                                 std::to_string(annoN) + " }");
            ++mi;
        };
        for (const auto& m : ci->methods) emitOneMethod(m);
        for (const auto& m : ci->staticMethods) emitOneMethod(m);
        if (methodStrs.empty()) {
            methodsDef = "@" + cname + ".meta_methods = private unnamed_addr constant "
                         "[0 x %r.HaoMethodMeta] zeroinitializer";
        } else {
            methodsDef = "@" + cname + ".meta_methods = private unnamed_addr constant ["
                         + std::to_string(methodStrs.size()) + " x %r.HaoMethodMeta] [";
            for (size_t i = 0; i < methodStrs.size(); ++i) {
                if (i) methodsDef += ", ";
                methodsDef += "%r.HaoMethodMeta " + methodStrs[i];
            }
            methodsDef += "]";
        }
        em_.addGlobal(methodsDef);

        // ---- 类级注解数组 ----
        size_t classAnnoN = emitAnnoArray(ci->annotations, "@" + cname + ".meta_annos");

        // ---- 接口数组 ----
        std::string ifacesDef;
        std::vector<std::string> ifaceStrs;
        for (const auto& in : ci->interfaceNames) ifaceStrs.push_back(em_.internString(in));
        if (ifaceStrs.empty()) {
            ifacesDef = "@" + cname + ".meta_ifaces = private unnamed_addr constant "
                        "[0 x ptr] zeroinitializer";
        } else {
            ifacesDef = "@" + cname + ".meta_ifaces = private unnamed_addr constant ["
                        + std::to_string(ifaceStrs.size()) + " x ptr] [ptr " + ifaceStrs[0];
            for (size_t k = 1; k < ifaceStrs.size(); ++k) ifacesDef += ", ptr " + ifaceStrs[k];
            ifacesDef += "]";
        }
        em_.addGlobal(ifacesDef);

        // ---- 构造工厂（包扫描 / reflect.newInstance[Args]）----
        std::string factoryRef = emitNewFactory(ci);

        // ---- 构造器参数类型名数组 ----
        std::vector<std::string> ctorPids;
        for (const auto& pt : ci->ctorParamTypes)
            ctorPids.push_back(em_.internString(pt->toString()));
        std::string ctorParamsDef;
        if (ctorPids.empty()) {
            ctorParamsDef = "@" + cname + ".meta_ctor_params = private unnamed_addr constant "
                            "[0 x ptr] zeroinitializer";
        } else {
            ctorParamsDef = "@" + cname + ".meta_ctor_params = private unnamed_addr constant ["
                            + std::to_string(ctorPids.size()) + " x ptr] [ptr " + ctorPids[0];
            for (size_t k = 1; k < ctorPids.size(); ++k)
                ctorParamsDef += ", ptr " + ctorPids[k];
            ctorParamsDef += "]";
        }
        em_.addGlobal(ctorParamsDef);

        // ---- 类 meta ----
        std::string metaName = "@" + cname + ".meta";
        std::string superRef = ci->baseName.empty() ? "null"
                                                    : em_.internString(ci->baseName);
        // global（非 constant）：末字段 classMirror 运行时写入 Class 单例
        em_.addGlobal(metaName + " = internal global %r.HaoClassMeta { "
            "ptr " + em_.internString(cname) + ", ptr " + superRef + ", ptr @" +
            cname + ".meta_ifaces, i64 " + std::to_string(ifaceStrs.size()) +
            ", ptr @" + cname + ".meta_fields, i64 " + std::to_string(fieldStrs.size()) +
            ", ptr @" + cname + ".meta_methods, i64 " + std::to_string(methodStrs.size()) +
            ", ptr @" + cname + ".meta_annos, i64 " + std::to_string(classAnnoN) +
            ", ptr " + ci->vtableIRName + ", i64 " + (ci->isAbstract ? "1" : "0") +
            ", i64 " + (ci->isEnum ? "1" : "0") + ", ptr " + factoryRef +
            ", i64 " + std::to_string(ci->ctorParamTypes.size()) +
            ", ptr @" + cname + ".meta_ctor_params" +
            ", i64 " + (ci->isAnnotation ? "1" : "0") +
            ", ptr null }");
        metaRefs.push_back(metaName);
    }

    // ---- 全局注册表（NULL 结尾）----
    metaRefs.push_back("null");
    std::string reg = "@hao_all_metas = global [" + std::to_string(metaRefs.size()) +
                      " x ptr] [";
    for (size_t i = 0; i < metaRefs.size(); ++i) {
        if (i) reg += ", ";
        reg += "ptr " + metaRefs[i];
    }
    reg += "]";
    em_.addGlobal(reg);
}

// ------------------------------------------------------------
//  构造工厂 @Class$new(ptr %argslots)（v0.31；v0.33 字段默认对齐语言 new）
//  槽约定与 invoke thunk 相同；0 参时可传 null。供 reflect / HttpApp.scan。
//  字段默认值走 genExpr（与语言 new 同路径），不再跳过非 String 的 ptr。
// ------------------------------------------------------------
std::string IRGen::emitNewFactory(const ClassInfoPtr& ci) {
    if (!ci || ci->isAbstract || ci->isEnum || ci->isGenericTemplate())
        return "null";
    if (!ci->hasVTable)
        return "null";

    std::string name = "@" + ci->name + "$new";

    // 按声明类所属包解析字段默认里的类型名（与语言 new 在调用点不同；
    // 工厂是独立函数，须恢复定义包的 import）。
    auto savedPrefix = currentPkgPrefix_;
    auto savedImports = currentImports_;
    auto savedImportPath = currentImportPath_;
    auto savedClass = currentClass_;
    auto savedReturn = currentReturn_;
    bool savedSawReturn = sawReturn_;
    bool savedBlockTerm = blockTerminated_;
    auto savedLoops = loops_;
    int savedTryCounter = tryCounter_;
    auto savedTryStack = tryStack_;
    int savedCatchDepth = catchDepth_;
    auto savedLambdas = lambdas_;
    auto savedCapNames = capturedVarNames_;
    std::string savedThisAddr = thisAddr_;
    bool savedInCtor = inConstructor_;
    std::string savedGcWm = gcRootWm_;
    auto savedHoist = loopHoisted_;
    auto savedSpillPools = loopSpillPools_;
    int savedSpillDepth = loopSpillDepth_;
    std::string savedUnwindReason = unwindReasonAddr_;
    std::string savedUnwindRet = unwindRetAddr_;
    std::string savedUnwindStop = unwindStopAddr_;
    std::string savedUnwindGc = unwindGcRootAddr_;

    {
        std::string baseForPkg = ci->isGenericInstance() ? ci->instanceOf : ci->name;
        size_t dp = baseForPkg.find_last_of('$');
        currentPkgPrefix_ = (dp == std::string::npos) ? ""
                                                      : baseForPkg.substr(0, dp + 1);
        restoreImportsForPkgPrefix(currentPkgPrefix_);
    }

    em_.pushFunctionState();
    currentClass_ = ci;
    currentReturn_ = Type::makeClass(ci->name);
    sawReturn_ = false;
    blockTerminated_ = false;
    inConstructor_ = false;
    thisAddr_.clear();
    loops_.clear();
    loopHoisted_.clear();
    loopSpillPools_.clear();
    loopSpillDepth_ = 0;
    tryCounter_ = 0;
    tryStack_.clear();
    catchDepth_ = 0;
    lambdas_.clear();
    capturedVarNames_.clear();
    gcRootWm_.clear();

    em_.emitBlank();
    em_.emitRaw("; 反射构造工厂：" + ci->name + "（"
                + std::to_string(ci->ctorParamTypes.size()) + " 参）");
    em_.emitRaw("define ptr " + name + "(ptr %argslots) {");
    em_.emitLabel("entry");

    // 字段默认 / 嵌套 new 可能走带异常路径的 genExpr，预留与函数体一致的槽
    emitAllocUnwindSlots();
    beginFunctionGcRoots();
    emitPushUnwindGcRoot();
    /* v0.53.5：反射工厂入口 safepoint（根已就绪） */
    em_.emit("call void @hao_gc_safepoint()");

    // 与普通 new 一致：先跑静态初始化（v0.30 $new0 曾漏，会导致静态字段未就绪）
    if (ci->hasStaticInit)
        em_.emit("call void @" + ci->name + ".ensureInit()");

    std::string objRaw = emitObjectNew(ci->slotCount(), objectPtrBitmap(ci.get()));
    std::string objSlot = emitSpillGcRoot("new.obj", objRaw);
    std::string obj = em_.nextTemp();
    em_.emit(obj + " = load ptr, ptr " + objSlot);
    std::string vtp = em_.nextTemp();
    em_.emit(vtp + " = getelementptr ptr, ptr " + obj + ", i64 0");
    em_.emit("store ptr " + ci->vtableIRName + ", ptr " + vtp);

    // 字段默认值：与语言 new 同路径（genExpr + coerce），覆盖 Int?/对象/lambda 等
    for (const auto& f : ci->fields) {
        if (!f.defaultExpr) continue;
        obj = em_.nextTemp();
        em_.emit(obj + " = load ptr, ptr " + objSlot);
        auto* dexpr = static_cast<HaoLangParser::ExprContext*>(f.defaultExpr);
        expectedTypes_.push_back(f.type);
        ExpectedTypeGuard eg{this};
        analyzeLambdas(dexpr);
        Value dv = genExpr(dexpr);
        if (!dv.valid()) {
            // 已报错；仍生成可链接的工厂（默认值留零）
            continue;
        }
        if (!isAssignable(dv.type, f.type)) {
            error(dexpr, "字段 '" + f.name + "' 的初始值类型 " +
                         dv.type->toString() + " 与声明类型 " +
                         f.type->toString() + " 不匹配");
            continue;
        }
        dv = coerce(dv, f.type, 0, 0);
        std::string fp = fieldPtr(obj, f.slot);
        emitHeapStore(fp, dv.ir, f.type, obj);
    }

    obj = em_.nextTemp();
    em_.emit(obj + " = load ptr, ptr " + objSlot);

    // 从 [Long] 槽按构造器真实 LLVM 类型解包（与 emitInvokeThunk 同约定）
    std::string argStr = "ptr " + obj;
    for (size_t i = 0; i < ci->ctorParamTypes.size(); ++i) {
        std::string lt = ci->ctorParamTypes[i]->llvmType();
        std::string cs = em_.nextTemp();
        em_.emit(cs + " = getelementptr ptr, ptr %argslots, i64 " +
                 std::to_string(i));
        std::string ca = em_.nextTemp();
        em_.emit(ca + " = load i64, ptr " + cs);
        std::string pv;
        if (lt == "double") {
            std::string cp = em_.nextTemp();
            em_.emit(cp + " = bitcast i64 " + ca + " to double");
            pv = cp;
        } else if (lt == "float") {
            std::string ct = em_.nextTemp();
            em_.emit(ct + " = trunc i64 " + ca + " to i32");
            std::string cp = em_.nextTemp();
            em_.emit(cp + " = bitcast i32 " + ct + " to float");
            pv = cp;
        } else if (lt == "i32") {
            std::string cp = em_.nextTemp();
            em_.emit(cp + " = trunc i64 " + ca + " to i32");
            pv = cp;
        } else if (lt == "i16") {
            std::string cp = em_.nextTemp();
            em_.emit(cp + " = trunc i64 " + ca + " to i16");
            pv = cp;
        } else if (lt == "i8") {
            std::string cp = em_.nextTemp();
            em_.emit(cp + " = trunc i64 " + ca + " to i8");
            pv = cp;
        } else if (lt == "ptr") {
            std::string cp = em_.nextTemp();
            em_.emit(cp + " = inttoptr i64 " + ca + " to ptr");
            pv = cp;
        } else { // i64
            pv = ca;
        }
        argStr += ", " + lt + " " + pv;
    }

    if (!ci->ctorIRName.empty())
        em_.emit("call void " + ci->ctorIRName + "(" + argStr + ")");
    if (!blockTerminated_) {
        obj = em_.nextTemp();
        em_.emit(obj + " = load ptr, ptr " + objSlot);
        emitGcRootUnwind();
        em_.emit("ret ptr " + obj);
    }
    em_.flushEntryAllocas();
    em_.emitRaw("}");

    std::string def = em_.popFunctionState();
    em_.addFunctionDef(def);

    currentPkgPrefix_ = savedPrefix;
    currentImports_ = savedImports;
    currentImportPath_ = savedImportPath;
    currentClass_ = savedClass;
    currentReturn_ = savedReturn;
    sawReturn_ = savedSawReturn;
    blockTerminated_ = savedBlockTerm;
    loops_ = savedLoops;
    loopHoisted_ = savedHoist;
    loopSpillPools_ = savedSpillPools;
    loopSpillDepth_ = savedSpillDepth;
    tryCounter_ = savedTryCounter;
    tryStack_ = savedTryStack;
    catchDepth_ = savedCatchDepth;
    lambdas_ = savedLambdas;
    capturedVarNames_ = savedCapNames;
    thisAddr_ = savedThisAddr;
    inConstructor_ = savedInCtor;
    gcRootWm_ = savedGcWm;
    unwindReasonAddr_ = savedUnwindReason;
    unwindRetAddr_ = savedUnwindRet;
    unwindStopAddr_ = savedUnwindStop;
    unwindGcRootAddr_ = savedUnwindGc;
    return name;
}

// ------------------------------------------------------------
//  反射 invoke thunk（v0.20.0）
// ------------------------------------------------------------
//  为反射方法生成统一调用器，签名固定为
//     i64 @...$invk(i64 %obj, ptr %argslots)
//   - %obj：接收者（实例方法）或 0（静态方法，忽略）
//   - %argslots：指向 [Int] 参数数组的元素区（每个实参 8 字节原始槽）
//  thunk 把每个槽按方法形参的真实 LLVM 类型转换后调用方法（虚方法经
//  this 虚表分派保持多态，static/非虚直接静态调用），再把返回值统一转成
//  i64。所有值都是 8 字节槽，编组零拷贝。
std::string IRGen::emitInvokeThunk(const ClassInfoPtr& ci, const MethodInfo& mi) {
    std::string name = mi.irName + "$invk";
    std::string def;
    def += "\n; 反射 invoke thunk：" + mi.name + "\n";
    def += "define i64 " + name + "(i64 %obj, ptr %argslots) {\nentry:\n";
    def += "  %thisp = inttoptr i64 %obj to ptr\n";

    std::string argStr = mi.isStatic ? "" : "ptr %thisp";
    std::string sigTypes = mi.isStatic ? "" : "ptr";
    for (size_t i = 0; i < mi.paramTypes.size(); ++i) {
        std::string lt = mi.paramTypes[i]->llvmType();
        def += "  %s" + std::to_string(i) + " = getelementptr ptr, ptr %argslots, i64 " +
               std::to_string(i) + "\n";
        def += "  %a" + std::to_string(i) + " = load i64, ptr %s" + std::to_string(i) + "\n";
        std::string pv;
        if (lt == "double") {
            def += "  %p" + std::to_string(i) + " = bitcast i64 %a" + std::to_string(i) + " to double\n";
            pv = "%p" + std::to_string(i);
        } else if (lt == "float") {
            def += "  %t" + std::to_string(i) + " = trunc i64 %a" + std::to_string(i) + " to i32\n";
            def += "  %p" + std::to_string(i) + " = bitcast i32 %t" + std::to_string(i) + " to float\n";
            pv = "%p" + std::to_string(i);
        } else if (lt == "i32") {
            def += "  %p" + std::to_string(i) + " = trunc i64 %a" + std::to_string(i) + " to i32\n";
            pv = "%p" + std::to_string(i);
        } else if (lt == "i16") {
            def += "  %p" + std::to_string(i) + " = trunc i64 %a" + std::to_string(i) + " to i16\n";
            pv = "%p" + std::to_string(i);
        } else if (lt == "i8") {
            def += "  %p" + std::to_string(i) + " = trunc i64 %a" + std::to_string(i) + " to i8\n";
            pv = "%p" + std::to_string(i);
        } else if (lt == "ptr") {
            def += "  %p" + std::to_string(i) + " = inttoptr i64 %a" + std::to_string(i) + " to ptr\n";
            pv = "%p" + std::to_string(i);
        } else { // i64
            pv = "%a" + std::to_string(i);
        }
        if (!argStr.empty()) { argStr += ", "; sigTypes += ", "; }
        argStr += lt + " " + pv;
        sigTypes += lt;
    }

    std::string fnTy = mi.returnType->llvmType() + " (" + sigTypes + ")";
    if (mi.vtableSlot >= 0 && ci->hasVTable) {
        // 虚方法：经 this 虚表分派，保持多态
        def += "  %vtp = getelementptr ptr, ptr %thisp, i64 0\n";
        def += "  %vt = load ptr, ptr %vtp\n";
        def += "  %mp = getelementptr ptr, ptr %vt, i64 " + std::to_string(mi.vtableSlot) + "\n";
        def += "  %fp = load ptr, ptr %mp\n";
        if (mi.returnType->isUnit())
            def += "  call " + fnTy + " %fp(" + argStr + ")\n";
        else
            def += "  %r = call " + fnTy + " %fp(" + argStr + ")\n";
    } else {
        if (mi.returnType->isUnit())
            def += "  call " + fnTy + " " + mi.irName + "(" + argStr + ")\n";
        else
            def += "  %r = call " + fnTy + " " + mi.irName + "(" + argStr + ")\n";
    }

    if (mi.returnType->isUnit()) {
        def += "  ret i64 0\n";
    } else {
        // 以 llvmType 定指令宽度；有/无符号用 TypeKind 选 sext/zext。
        // 注意 Int?/Short? 等 kind 仍是数值，但 llvmType 已是 ptr（装箱）。
        auto rk = mi.returnType->kind;
        std::string rt = mi.returnType->llvmType();
        if (rt == "double") {
            def += "  %rr = bitcast double %r to i64\n";
            def += "  ret i64 %rr\n";
        } else if (rt == "float") {
            def += "  %bits = bitcast float %r to i32\n";
            def += "  %rr = zext i32 %bits to i64\n";
            def += "  ret i64 %rr\n";
        } else if (rt == "i32") {
            if (mi.returnType->isUnsigned())
                def += "  %rr = zext i32 %r to i64\n";
            else
                def += "  %rr = sext i32 %r to i64\n";
            def += "  ret i64 %rr\n";
        } else if (rt == "i16") {
            if (mi.returnType->isUnsigned())
                def += "  %rr = zext i16 %r to i64\n";
            else
                def += "  %rr = sext i16 %r to i64\n";
            def += "  ret i64 %rr\n";
        } else if (rt == "i8") {
            if (rk == TypeKind::SByte)
                def += "  %rr = sext i8 %r to i64\n";
            else
                def += "  %rr = zext i8 %r to i64\n";
            def += "  ret i64 %rr\n";
        } else if (rt == "ptr") {
            def += "  %rr = ptrtoint ptr %r to i64\n";
            def += "  ret i64 %rr\n";
        } else {
            def += "  ret i64 %r\n";
        }
    }
    def += "}\n";
    em_.addFunctionDef(def);
    return name;
}

// ------------------------------------------------------------
//  为 is / as 生成类型判定列表
// ------------------------------------------------------------
//  对象槽位 0 存虚表指针，而每个类的虚表是唯一的全局常量，
//  因此虚表地址即可作为类型标识。
//
//  `obj is T` 要对 T 的所有子类都成立，所以为每个类型生成一个
//  「自身 + 全部子类（或实现类）的虚表」列表，运行时线性查找。
//  类层次通常很浅，查找成本可忽略；换来的是无需在对象里额外
void IRGen::emitTypeIdLists() {
    // ---- 类：自身 + 所有子类 ----
    for (auto& [cname, ci] : classes_) {
        std::vector<std::string> vts;
        for (auto& [oname, oc] : classes_) {
            if (!oc->hasVTable) continue;          // 无虚表的类无法参与判定
            if (oc->isAbstract) continue;          // 抽象类不会被实例化
            if (oc->isSubclassOf(cname)) vts.push_back(oc->vtableIRName);
        }
        // typeIdLists_ 已含该名（第 2 遍：泛型实例生成后补发）则跳过，避免重复定义
        if (vts.empty() || typeIdLists_.count(cname)) continue;

        std::string name = "@" + cname + ".typeids";
        typeIdLists_[cname] = name;

        std::string def = name + " = private unnamed_addr constant [" +
                          std::to_string(vts.size() + 1) + " x ptr] [";
        for (const auto& v : vts) def += "ptr " + v + ", ";
        def += "ptr null]";                        // NULL 结尾
        em_.addGlobal(def);
    }

    // ---- 接口：所有实现该接口的类 ----
    for (auto& [iname, ii] : interfaces_) {
        std::vector<std::string> vts;
        for (auto& [oname, oc] : classes_) {
            if (!oc->hasVTable || oc->isAbstract) continue;
            if (oc->implementsInterface(iname)) vts.push_back(oc->vtableIRName);
        }
        if (vts.empty() || typeIdLists_.count(iname)) continue;

        std::string name = "@" + iname + ".typeids";
        typeIdLists_[iname] = name;

        std::string def = name + " = private unnamed_addr constant [" +
                          std::to_string(vts.size() + 1) + " x ptr] [";
        for (const auto& v : vts) def += "ptr " + v + ", ";
        def += "ptr null]";
        em_.addGlobal(def);
    }
}

// ------------------------------------------------------------
//  阶段 A：只登记类名（含泛型模板），不解析成员。
//  所有包的类型名都登记后，阶段 B 解析成员时才能跨包引用类型。
void IRGen::registerClassNames(const SourceUnit& u) {
    for (auto* decl : u.tree->topLevelDecl()) {
        auto* cls = decl->classDecl();
        if (!cls) continue;

        std::string shortName = cls->IDENT()->getText();
        std::string cname = currentPkgPrefix_ + shortName;
        if (classes_.count(cname)) {
            error(cls, "类 '" + shortName + "' 重复定义");
            continue;
        }

        auto ci = std::make_shared<ClassInfo>();
        ci->name = cname;
        ci->importPath = u.importPath;
        ci->line = cls->getStart()->getLine();
        ci->column = cls->getStart()->getCharPositionInLine();

        if (auto* tp = cls->typeParams()) {
            for (auto* id : tp->IDENT()) ci->typeParams.push_back(id->getText());
            ci->declNode = cls;
            for (const auto& p : ci->typeParams) {
                if (classes_.count(p) || interfaces_.count(p))
                    error(cls, "类型参数 '" + p + "' 与已有类型重名");
            }
        }

        classes_[cname] = ci;

        auto tsym = std::make_shared<Symbol>();
        tsym->kind = SymbolKind::Class;
        tsym->name = cname;
        tsym->type = Type::makeClass(cname);
        tsym->classInfo = ci;
        tsym->line = ci->line;
        syms_.declareGlobal(tsym);

        if (!declIsPrivate(cls->modifier()))
            pkgExports_[u.importPath][shortName] = cname;
    }

    // ---- enum 也登记为类 ----
    for (auto* decl : u.tree->topLevelDecl()) {
        auto* ed = decl->enumDecl();
        if (!ed) continue;

        std::string shortName = ed->IDENT()->getText();
        std::string cname = currentPkgPrefix_ + shortName;
        if (classes_.count(cname)) {
            error(ed, "枚举 '" + shortName + "' 与已有类型重名");
            continue;
        }

        auto ci = std::make_shared<ClassInfo>();
        ci->name = cname;
        ci->importPath = u.importPath;
        ci->isEnum = true;
        ci->line = ed->getStart()->getLine();
        ci->column = ed->getStart()->getCharPositionInLine();

        classes_[cname] = ci;

        auto tsym = std::make_shared<Symbol>();
        tsym->kind = SymbolKind::Class;
        tsym->name = cname;
        tsym->type = Type::makeClass(cname);
        tsym->classInfo = ci;
        tsym->line = ci->line;
        syms_.declareGlobal(tsym);

        if (!declIsPrivate(ed->modifier()))
            pkgExports_[u.importPath][shortName] = cname;
    }

    // ---- annotation 也登记为类（v0.19.0）----
    for (auto* decl : u.tree->topLevelDecl()) {
        auto* ad = decl->annotationDecl();
        if (!ad) continue;

        std::string shortName = ad->IDENT()->getText();
        std::string cname = currentPkgPrefix_ + shortName;
        if (classes_.count(cname)) {
            error(ad, "注解 '" + shortName + "' 与已有类型重名");
            continue;
        }

        auto ci = std::make_shared<ClassInfo>();
        ci->name = cname;
        ci->importPath = u.importPath;
        ci->isAnnotation = true;
        ci->line = ad->getStart()->getLine();
        ci->column = ad->getStart()->getCharPositionInLine();

        classes_[cname] = ci;

        auto tsym = std::make_shared<Symbol>();
        tsym->kind = SymbolKind::Class;
        tsym->name = cname;
        tsym->type = Type::makeClass(cname);
        tsym->classInfo = ci;
        tsym->line = ci->line;
        syms_.declareGlobal(tsym);

        pkgExports_[u.importPath][shortName] = cname;
    }
}

// 阶段 B：解析类的字段、方法、构造函数、继承关系。
void IRGen::collectClassMembers(const SourceUnit& u) {
    for (auto* decl : u.tree->topLevelDecl()) {
        auto* cls = decl->classDecl();
        if (!cls) continue;

        std::string shortName = cls->IDENT()->getText();
        std::string cname = currentPkgPrefix_ + shortName;
        auto ci = lookupClass(cname);
        if (!ci) continue;

        // 泛型类模板的成员在实例化时解析
        if (ci->isGenericTemplate()) continue;

        // ---- 类级注解（v0.19.0）----
        ci->annotations = resolveAnnotationUses(cls->annotationUse());

        for (auto* mod : cls->modifier())
            if (mod->ABSTRACT()) ci->isAbstract = true;

        // ---- 继承与接口实现 ----
        //  支持两种写法：
        //    - `class Dog : Animal, Shape`（冒号混列，按类型自动分流：类->继承、接口->实现）
        //    - `class Dog extends Animal implements Shape`（显式区分）
        //  用 resolveType 解析，使跨包限定名（other.Shape）也能解析到内部名。
        auto addBase = [&](const TypePtr& rt) {
            if (!rt) return;
            std::string tn = rt->className;
            if (rt->kind == TypeKind::Interface) {
                // 泛型接口：先记入待实例化表（此时模板槽位未分配，不立即实例化），
                // 槽位分配后由 instantiateAllGenericInterfaces 统一实例化，再记实例名。
                if (!rt->typeArgs.empty()) {
                    std::string iname = Type::makeInterface(tn, rt->typeArgs)->monoName();
                    pendingGenericInterfaces_[iname] = rt;
                    ci->interfaceNames.push_back(iname);
                } else {
                    ci->interfaceNames.push_back(tn);
                }
            } else if (!tn.empty()) {
                if (!ci->baseName.empty()) {
                    error(cls, "类 '" + shortName + "' 不能继承多个类；多继承请改用接口");
                    return;
                }
                if (tn == cname) {
                    error(cls, "类 '" + shortName + "' 不能继承自身");
                    return;
                }
                ci->baseName = tn;
            }
        };

        if (auto* cb = cls->classBase()) {
            if (auto* eb = dynamic_cast<HaoLangParser::ExtendsBaseContext*>(cb)) {
                // extends <类>：基类必须是类（非接口）
                TypePtr rt = resolveType(eb->type());
                if (rt && rt->kind == TypeKind::Interface)
                    error(cls, "extends 应指定基类，接口请用 implements");
                else
                    addBase(rt);
                if (auto* il = eb->typeList())
                    for (auto* t : il->type()) {
                        TypePtr i = resolveType(t);
                        if (i && i->kind != TypeKind::Interface)
                            error(cls, "implements 只能列出接口，类 '" +
                                       i->className + "' 请用 extends");
                        else
                            addBase(i);
                    }
            } else if (auto* ib = dynamic_cast<HaoLangParser::ImplementsBaseContext*>(cb)) {
                if (auto* il = ib->typeList())
                    for (auto* t : il->type()) {
                        TypePtr i = resolveType(t);
                        if (i && i->kind != TypeKind::Interface)
                            error(cls, "implements 只能列出接口，类 '" +
                                       i->className + "' 请用 extends");
                        else
                            addBase(i);
                    }
            } else if (auto* cob = dynamic_cast<HaoLangParser::ColonBaseContext*>(cb)) {
                if (auto* tl = cob->typeList())
                    for (auto* t : tl->type())
                        addBase(resolveType(t));
            }
        }

        // ---- 默认继承 Object 根父类（v0.15.0）----
        //  未显式指定基类的类默认继承 Object（Java/C# 模型）。
        //  Object 自身（object 包里的 Object）不能继承自己。
        if (ci->baseName.empty() && cname != "object$Object")
            ci->baseName = "object$Object";

        int slot = 0;
        for (auto* mem : cls->classMember()) {
            // ---- 字段 ----
            if (auto* fd = mem->fieldDecl()) {
                FieldInfo fi;
                fi.name = fd->IDENT()->getText();
                fi.isMutable = (fd->VAR() != nullptr);
                fi.ownerClass = cname;
                fi.line = fd->getStart()->getLine();
                fi.column = fd->getStart()->getCharPositionInLine();
                fi.annotations = resolveAnnotationUses(fd->annotationUse());
                for (auto* mod : fd->modifier()) {
                    if (mod->STATIC())         fi.isStatic = true;
                    else if (mod->PRIVATE())   fi.visibility = FieldInfo::Vis::Private;
                    else if (mod->PROTECTED()) fi.visibility = FieldInfo::Vis::Protected;
                    else if (mod->INTERNAL())  fi.visibility = FieldInfo::Vis::Internal;
                }

                if (fd->type()) {
                    fi.type = resolveType(fd->type());
                } else if (fd->expr()) {
                    fi.type = inferExprType(fd->expr());
                    if (!fi.type || fi.type->isUnknown()) {
                        error(fd, "无法推断字段 '" + fi.name + "' 的类型，请显式标注");
                        fi.type = Type::makeInt();
                    }
                } else {
                    error(fd, "字段 '" + fi.name + "' 必须有类型标注或初始值");
                    fi.type = Type::makeInt();
                }

                // 静态字段：类级全局变量，不占对象槽位，也不参与继承
                if (fi.isStatic) {
                    if (ci->findStaticField(fi.name)) {
                        error(fd, "静态字段 '" + fi.name + "' 在类 '" + shortName + "' 中重复定义");
                        continue;
                    }
                    fi.defaultExpr = fd->expr();
                    ci->staticFields.push_back(fi);
                    continue;
                }

                if (ci->findField(fi.name)) {
                    error(fd, "字段 '" + fi.name + "' 在类 '" + shortName + "' 中重复定义");
                    continue;
                }
                fi.slot = slot++;
                fi.defaultExpr = fd->expr();
                ci->fields.push_back(fi);
                continue;
            }

            // ---- 方法 ----
            if (auto* fn = mem->funcDecl()) {
                // 泛型方法（v0.9.0）：方法级类型参数在调用时从 lambda 实参推断，
                // 单态化为「类实例名.方法名$R」。此处只登记模板，不加入普通
                // 方法表（实例化时单独处理）。
                if (fn->typeParams()) {
                    GenericMethod gm;
                    gm.className = cname;
                    gm.methodName = fn->IDENT()->getText();
                    gm.pkgPrefix = currentPkgPrefix_;
                    gm.decl = fn;
                    for (auto* id : fn->typeParams()->IDENT())
                        gm.typeParams.push_back(id->getText());
                    // 同名普通方法与泛型方法互斥；重复泛型方法模板覆盖（后者胜）
                    genericMethods_[cname + "." + gm.methodName] = gm;
                    continue;
                }
                MethodInfo mi;
                mi.name = fn->IDENT()->getText();
                mi.ownerClass = cname;
                mi.line = fn->getStart()->getLine();
                mi.column = fn->getStart()->getCharPositionInLine();
                mi.returnType = fn->returnType()
                    ? resolveType(fn->returnType()->type())
                    : Type::makeUnit();
                mi.annotations = resolveAnnotationUses(fn->annotationUse());

                for (auto* mod : fn->modifier()) {
                    if (mod->STATIC())         mi.isStatic = true;
                    else if (mod->OVERRIDE())  mi.isOverride = true;
                    else if (mod->ABSTRACT())  mi.isAbstract = true;
                    else if (mod->PRIVATE())   mi.visibility = MethodInfo::Vis::Private;
                    else if (mod->PROTECTED()) mi.visibility = MethodInfo::Vis::Protected;
                    else if (mod->INTERNAL())  mi.visibility = MethodInfo::Vis::Internal;
                }

                if (mi.isAbstract) {
                    if (!ci->isAbstract)
                        error(fn, "抽象方法 '" + mi.name + "' 只能声明在 abstract 类中");
                    if (fn->block())
                        error(fn, "抽象方法 '" + mi.name + "' 不能有实现体");
                } else if (!fn->block()) {
                    error(fn, "方法 '" + mi.name + "' 缺少实现体（若为抽象方法请加 abstract 修饰）");
                }

                if (auto* pl = fn->paramList()) {
                    for (auto* p : pl->param()) {
                        mi.paramNames.push_back(p->IDENT()->getText());
                        mi.paramTypes.push_back(resolveType(p->type()));
                    }
                }

                // 静态方法：按参数签名重载（IR 名带 overloadSuffix）
                if (mi.isStatic) {
                    if (ci->findStaticMethod(mi.name, mi.paramTypes)) {
                        error(fn, "静态方法 '" + mi.name + "' 在类 '" + shortName +
                                   "' 中重复定义（参数签名相同）");
                        continue;
                    }
                    bool needsSuffix = !ci->findStaticMethods(mi.name).empty();
                    mi.irName = staticMethodIRName(cname, mi.name, mi.paramTypes,
                                                   needsSuffix);
                    ci->staticMethods.push_back(mi);
                    continue;
                }
                                // Instance methods: overload by param signature (IR suffix like static)
                if (ci->findMethod(mi.name, mi.paramTypes)) {
                    error(fn, "方法 '" + mi.name + "' 在类 '" + shortName +
                               "' 中重复定义（参数签名相同）");
                    continue;
                }
                bool needsSuffix = !ci->findMethods(mi.name).empty();
                mi.irName = instanceMethodIRName(cname, mi.name, mi.paramTypes, needsSuffix);
                ci->methods.push_back(mi);
                continue;
            }

            // ---- 构造函数 ----
            if (auto* ctor = mem->constructorDecl()) {
                if (!ci->ctorIRName.empty()) {
                    error(ctor, "类 '" + shortName + "' 只能有一个构造函数");
                    continue;
                }
                ci->ctorIRName = "@" + cname + ".ctor";
                if (auto* pl = ctor->paramList()) {
                    for (auto* p : pl->param()) {
                        ci->ctorParamNames.push_back(p->IDENT()->getText());
                        ci->ctorParamTypes.push_back(resolveType(p->type()));
                    }
                }
                continue;
            }

            // ---- 静态构造器（C# 风格 static ClassName()）----
            if (auto* sc = mem->staticCtorDecl()) {
                if (sc->IDENT()->getText() != shortName) {
                    error(sc, "静态构造器名必须与类名一致，应为 '" + shortName + "'");
                    continue;
                }
                if (sc->paramList()) {
                    error(sc, "静态构造器不能有参数");
                    continue;
                }
                if (ci->staticCtorNode) {
                    error(sc, "类 '" + shortName + "' 只能有一个静态构造器");
                    continue;
                }
                ci->staticCtorNode = sc;
                continue;
            }

            if (auto* pd = mem->propertyDecl()) {
                error(pd, "当前版本尚不支持自动属性（get/set）");
                continue;
            }
        }
    }

    // ---- 注解：收集字段（v0.19.0）----
    for (auto* decl : u.tree->topLevelDecl()) {
        auto* ad = decl->annotationDecl();
        if (!ad) continue;

        std::string shortName = ad->IDENT()->getText();
        std::string cname = currentPkgPrefix_ + shortName;
        auto ci = lookupClass(cname);
        if (!ci || !ci->isAnnotation) continue;

        ci->baseName = "object$Object";
        int slot = 0;
        for (auto* am : ad->annotationMember()) {
            FieldInfo fi;
            fi.name = am->IDENT()->getText();
            fi.isMutable = (am->VAR() != nullptr);
            fi.ownerClass = cname;
            fi.line = am->getStart()->getLine();
            fi.column = am->getStart()->getCharPositionInLine();
            if (am->type())
                fi.type = resolveType(am->type());
            else if (am->expr())
                fi.type = inferExprType(am->expr());
            else
                fi.type = Type::makeString();
            fi.slot = slot++;
            fi.defaultExpr = am->expr();
            ci->fields.push_back(fi);
        }
    }

    // ---- 枚举：合成类成员（常量集合 + 名称/序数）----
    for (auto* decl : u.tree->topLevelDecl()) {
        auto* ed = decl->enumDecl();
        if (!ed) continue;

        std::string shortName = ed->IDENT()->getText();
        std::string cname = currentPkgPrefix_ + shortName;
        auto ci = lookupClass(cname);
        if (!ci || !ci->isEnum) continue;

        ci->baseName = "object$Object";   // 所有类默认继承 Object

        // 字段：_name / _ordinal（私有，槽位由 resolveInterfaceImpls 后移）
        FieldInfo fn;
        fn.name = "_name"; fn.type = Type::makeString();
        fn.ownerClass = cname; fn.visibility = FieldInfo::Vis::Private;
        fn.line = ci->line; fn.column = ci->column; fn.slot = 0;
        ci->fields.push_back(fn);
        FieldInfo fo;
        fo.name = "_ordinal"; fo.type = Type::makeInt();
        fo.ownerClass = cname; fo.visibility = FieldInfo::Vis::Private;
        fo.line = ci->line; fo.column = ci->column; fo.slot = 1;
        ci->fields.push_back(fo);

        // 私有构造函数 Color(_name: String, _ordinal: Int)
        ci->ctorIRName = "@" + cname + ".ctor";
        ci->ctorParamNames = { "_name", "_ordinal" };
        ci->ctorParamTypes = { Type::makeString(), Type::makeInt() };

        // 方法：name() / ordinal() / override toString()
        MethodInfo mn;
        mn.name = "name"; mn.irName = "@" + cname + ".name";
        mn.ownerClass = cname; mn.returnType = Type::makeString();
        mn.line = ci->line; mn.column = ci->column;
        ci->methods.push_back(mn);
        MethodInfo mo;
        mo.name = "ordinal"; mo.irName = "@" + cname + ".ordinal";
        mo.ownerClass = cname; mo.returnType = Type::makeInt();
        mo.line = ci->line; mo.column = ci->column;
        ci->methods.push_back(mo);
        MethodInfo mt;
        mt.name = "toString"; mt.irName = "@" + cname + ".toString";
        mt.ownerClass = cname; mt.returnType = Type::makeString();
        mt.isOverride = true;
        mt.line = ci->line; mt.column = ci->column;
        ci->methods.push_back(mt);

        // 静态字段：每个常量一个（类型为本枚举，只读）
        for (auto* ec : ed->enumConstant()) {
            FieldInfo sf;
            sf.name = ec->IDENT()->getText();
            sf.type = Type::makeClass(cname);
            sf.isStatic = true; sf.isMutable = false;
            sf.ownerClass = cname;
            sf.line = ec->getStart()->getLine();
            sf.column = ec->getStart()->getCharPositionInLine();
            ci->staticFields.push_back(sf);
        }

        // 静态构造器：创建各常量实例（genStaticConstructor 按 isEnum 分支）
        ci->staticCtorNode = ed;
    }
}

ClassInfoPtr IRGen::lookupClass(const std::string& name) {
    auto it = classes_.find(name);
    return it == classes_.end() ? nullptr : it->second;
}

// 由类型取 ClassInfo：泛型类型需先转成单态化实例名
ClassInfoPtr IRGen::classOfType(const TypePtr& t) {
    if (!t || t->kind != TypeKind::Class) return nullptr;
    if (!t->typeArgs.empty()) return lookupClass(t->monoName());
    return lookupClass(t->className);
}
void IRGen::genClass(HaoLangParser::ClassDeclContext* cls) {
    std::string cname = currentPkgPrefix_ + cls->IDENT()->getText();
    auto ci = lookupClass(cname);
    if (!ci) return;   // 收集阶段已报错

    em_.emitBlank();
    em_.emitRaw("; ============ 类 " + cname +
                (ci->baseName.empty() ? "" : " : " + ci->baseName) + " ============");
    em_.emitRaw("; 字段布局:");
    if (ci->hasVTable) em_.emitRaw(";   [0] <vtable>");
    for (const auto& f : ci->fields)
        em_.emitRaw(";   [" + std::to_string(f.slot) + "] " +
                    f.name + " : " + f.type->toString() +
                    (f.ownerClass == cname ? "" : "  (继承自 " + f.ownerClass + ")"));

    emitStaticFieldGlobals(ci);
    genStaticConstructor(ci);

    for (auto* mem : cls->classMember()) {
        if (auto* ctor = mem->constructorDecl()) genConstructor(ctor, ci);
        else if (auto* fn = mem->funcDecl()) {
            // 抽象方法无实现体，不生成代码
            const MethodInfo* mi = ci->findMethod(fn->IDENT()->getText());
            if (mi && mi->isAbstract) continue;
            genMethod(fn, ci);
        }
        // 字段与属性无需生成代码
    }
}

// 生成枚举类代码。成员在 collectClassMembers 中已合成（见 collectEnumMembers）；
// 这里手工发射构造器、name/ordinal/toString 方法，并委托 genStaticConstructor
// 创建各常量实例（isEnum 分支）。
void IRGen::genEnum(HaoLangParser::EnumDeclContext* ed, const ClassInfoPtr& ci) {
    (void)ed;
    em_.emitBlank();
    em_.emitRaw("; ============ 枚举 " + ci->name + " ============");

    emitStaticFieldGlobals(ci);

    const FieldInfo* nameF = ci->findField("_name");
    const FieldInfo* ordF  = ci->findField("_ordinal");
    int nameSlot = nameF ? nameF->slot : 1;
    int ordSlot  = ordF  ? ordF->slot  : 2;

    // ---- 构造器 Color(_name, _ordinal) ----
    em_.resetFunctionState();
    currentReturn_ = Type::makeUnit();
    sawReturn_ = false; blockTerminated_ = false; inMain_ = false;
    currentClass_ = ci; inConstructor_ = true; thisAddr_.clear();
    em_.emitRaw("define void " + ci->ctorIRName +
                "(ptr %this.arg, ptr %_name.arg, i32 %_ordinal.arg) {");
    em_.emitLabel("entry");
    {
        std::string n1 = em_.nextTemp();
        em_.emit(n1 + " = getelementptr ptr, ptr %this.arg, i64 " + std::to_string(nameSlot));
        emitHeapStore(n1, "%_name.arg", Type::makeString(), "%this.arg");
        std::string n2 = em_.nextTemp();
        em_.emit(n2 + " = getelementptr ptr, ptr %this.arg, i64 " + std::to_string(ordSlot));
        em_.emit("store i32 %_ordinal.arg, ptr " + n2);
        em_.emit("ret void");
    }
    em_.emitRaw("}");
    currentClass_ = nullptr; thisAddr_.clear();

    // ---- name / ordinal / toString 方法 ----
    //  用 lambda 内联生成：读指定字段槽位并返回。
    auto emitMethod = [&](const std::string& mname, const TypePtr& ret, int slot) {
        em_.resetFunctionState();
        currentReturn_ = ret;
        sawReturn_ = false; blockTerminated_ = false; inMain_ = false;
        currentClass_ = ci; inConstructor_ = false; thisAddr_.clear();
        em_.emitRaw("define " + ret->llvmType() + " @" + ci->name + "." +
                    mname + "(ptr %this.arg) {");
        em_.emitLabel("entry");
        std::string fp = em_.nextTemp();
        em_.emit(fp + " = getelementptr ptr, ptr %this.arg, i64 " + std::to_string(slot));
        std::string v = em_.nextTemp();
        em_.emit(v + " = load " + ret->llvmType() + ", ptr " + fp);
        em_.emit("ret " + ret->llvmType() + " " + v);
        em_.emitRaw("}");
        currentClass_ = nullptr; thisAddr_.clear();
    };
    emitMethod("name", Type::makeString(), nameSlot);
    emitMethod("ordinal", Type::makeInt(), ordSlot);
    emitMethod("toString", Type::makeString(), nameSlot);
    em_.emitBlank();

    genStaticConstructor(ci);
}

void IRGen::genMethod(HaoLangParser::FuncDeclContext* fn, const ClassInfoPtr& ci) {
    std::string mname = fn->IDENT()->getText();
    // 解析本声明的参数类型，供静态重载精确匹配
    std::vector<TypePtr> declParams;
    if (auto* pl = fn->paramList()) {
        for (auto* p : pl->param())
            declParams.push_back(resolveType(p->type()));
    }
    // 必须取本类自己声明的版本：findMethod 会返回扁平化后的项，
    // 若该方法继承自父类，其 irName 指向父类实现，用它生成会重复定义符号。
    const MethodInfo* mi = ci->findOwnMethod(mname, declParams);
    if (!mi) mi = ci->findMethod(mname, declParams);
    if (!mi) mi = ci->findStaticMethod(mname, declParams);
    if (!mi) mi = ci->findStaticMethod(mname);
    if (!mi) return;
    if (!fn->block()) return;   // 抽象方法无实现体，不生成代码

    em_.resetFunctionState();
    currentReturn_ = mi->returnType;
    sawReturn_ = false;
    blockTerminated_ = false;
    inMain_ = false;
    currentClass_ = ci;
    inConstructor_ = false;

    bool hasThis = !mi->isStatic;

    // 签名：实例方法首参为隐式 this；静态方法无 this
    std::string sig = "define " + mi->returnType->llvmType() + " " +
                      mi->irName + "(";
    if (hasThis) sig += "ptr %this.arg";
    for (size_t i = 0; i < mi->paramTypes.size(); ++i) {
        if (hasThis || i > 0) sig += ", ";
        sig += mi->paramTypes[i]->llvmType() + " %" + mi->paramNames[i] + ".arg";
    }
    sig += ") {";

    em_.emitBlank();
    em_.emitRaw(sig);
    em_.emitLabel("entry");

    genFunctionBody(fn->block(), mi->paramNames, mi->paramTypes,
                    mi->returnType, /*isMain=*/false, hasThis, ci->name,
                    fn, "方法 '" + ci->name + "." + mname + "'");

    em_.flushEntryAllocas();
    em_.emitRaw("}");
    currentClass_ = nullptr;
    thisAddr_.clear();
}

void IRGen::genConstructor(HaoLangParser::ConstructorDeclContext* ctor,
                           const ClassInfoPtr& ci) {
    em_.resetFunctionState();
    currentReturn_ = Type::makeUnit();
    sawReturn_ = false;
    blockTerminated_ = false;
    inMain_ = false;
    currentClass_ = ci;
    inConstructor_ = true;

    std::string sig = "define void " + ci->ctorIRName + "(ptr %this.arg";
    for (size_t i = 0; i < ci->ctorParamTypes.size(); ++i)
        sig += ", " + ci->ctorParamTypes[i]->llvmType() +
               " %" + ci->ctorParamNames[i] + ".arg";
    sig += ") {";

    em_.emitBlank();
    em_.emitRaw(sig);
    em_.emitLabel("entry");

    genFunctionBody(ctor->block(), ci->ctorParamNames, ci->ctorParamTypes,
                    Type::makeUnit(), /*isMain=*/false, /*hasThis=*/true, ci->name,
                    ctor, "构造函数 '" + ci->name + "'");

    em_.flushEntryAllocas();
    em_.emitRaw("}");
    currentClass_ = nullptr;
    thisAddr_.clear();
}

// ============================================================
//  泛型方法单态化（v0.9.0）
// ------------------------------------------------------------
//  List<T>.map<R>(f:(T)->R): List<R> 这类方法级类型参数：
//    - 类级 T 在实例类（List$Int）收集时已替换；
//    - 方法级 R 在调用时从 lambda 实参推断，单态化为具体方法
//      List$Int.map$String，静态调用（零开销）。
//  与顶层泛型函数（callGenericFunction / instantiateFunction）同路线，
//  但多了隐式 this 与类级 T 替换。
// ============================================================

Value IRGen::callGenericMethod(const Value& recv, const ClassInfoPtr& ci,
                               const GenericMethod& gm,
                               HaoLangParser::CallOpContext* call,
                               antlr4::ParserRuleContext* ctx) {
    // ---- 1. 建立上下文：类级替换 + 类级/方法级类型参数集合 ----
    TypeSubst classSubst;
    std::string tplName = ci->instanceOf.empty() ? ci->name : ci->instanceOf;
    auto tmpl = lookupClass(tplName);
    if (tmpl) {
        for (size_t i = 0; i < tmpl->typeParams.size() && i < ci->typeArgs.size(); ++i)
            classSubst[tmpl->typeParams[i]] = ci->typeArgs[i];
    }
    auto savedParams0 = currentTypeParams_;
    auto savedPrefix0 = currentPkgPrefix_;
    auto savedSubst0  = currentSubst_;
    currentTypeParams_.clear();
    // 解析方法签名时用声明类的包前缀，使同包类型裸名正确解析
    currentPkgPrefix_ = gm.pkgPrefix;
    if (tmpl) for (const auto& tp : tmpl->typeParams) currentTypeParams_.insert(tp);
    for (const auto& tp : gm.typeParams) currentTypeParams_.insert(tp);
    currentSubst_ = classSubst;   // 类级替换生效，方法级 R 保留为 TypeParam

    // ---- 2. 解析方法形参类型（T 已替换，R 仍是 TypeParam）----
    std::vector<TypePtr> tplParams;
    auto* fn = gm.decl;
    if (auto* pl = fn->paramList())
        for (auto* p : pl->param())
            tplParams.push_back(resolveType(p->type()));

    size_t nargs = call && call->argList() ? call->argList()->arg().size() : 0;

    // 判断某实参是否为 lambda（需期望类型才能推断参数/返回类型）
    auto isLambdaArg = [&](size_t k) -> HaoLangParser::LambdaContext* {
        antlr4::tree::ParseTree* node = call->argList()->arg(k)->expr();
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

    // ---- 3. 两遍求值实参，推断方法级类型参数（同 callGenericFunction）----
    std::vector<Value> args(nargs);
    TypeSubst methodSubst;
    for (size_t k = 0; k < nargs; ++k) {
        if (isLambdaArg(k)) continue;
        Value av = genExpr(call->argList()->arg(k)->expr());
        if (!av.valid()) { currentTypeParams_=savedParams0; currentPkgPrefix_=savedPrefix0; currentSubst_=savedSubst0; return Value(); }
        rootGcOperand(av);
        args[k] = av;
        if (k < tplParams.size())
            unifyWithArg(tplParams[k], av.type, methodSubst, ctx);
    }
    for (size_t k = 0; k < nargs; ++k) {
        if (!isLambdaArg(k)) continue;
        TypePtr expected;
        if (k < tplParams.size()) {
            expected = substType(tplParams[k], methodSubst);
            if (expected->kind == TypeKind::Func) {
                auto conv = std::make_shared<Type>(*expected);
                for (auto& p : conv->params)
                    if (p->hasTypeParam()) p = Type::makeUnknown();
                if (conv->elem && conv->elem->hasTypeParam())
                    conv->elem = nullptr;
                expected = conv;
            }
            expectedTypes_.push_back(expected);
        }
        Value av = genExpr(call->argList()->arg(k)->expr());
        if (k < tplParams.size()) expectedTypes_.pop_back();
        if (!av.valid()) { currentTypeParams_=savedParams0; currentPkgPrefix_=savedPrefix0; currentSubst_=savedSubst0; return Value(); }
        rootGcOperand(av);
        args[k] = av;
        if (k < tplParams.size())
            unifyWithArg(tplParams[k], av.type, methodSubst, ctx);
    }

    currentTypeParams_ = savedParams0;
    currentPkgPrefix_  = savedPrefix0;
    currentSubst_      = savedSubst0;
    if (diags_.hasErrors()) return Value();

    // ---- 4. 校验方法级类型参数全部推断出 ----
    for (const auto& tp : gm.typeParams)
        if (!methodSubst.count(tp)) {
            error(ctx, "无法推断泛型方法 '" + ci->name + "." + gm.methodName +
                       "' 的类型参数 " + tp);
            return Value();
        }

    // ---- 5. 实例名：List$Int.map$String ----
    std::string instName = ci->name + "." + gm.methodName;
    for (const auto& tp : gm.typeParams)
        instName += "$" + methodSubst[tp]->monoName();

    // ---- 6. 已实例化则直接用缓存的签名生成调用 ----
    auto cached = methodInstanceInfos_.find(instName);
    const MethodInfo* mi = nullptr;
    if (cached != methodInstanceInfos_.end()) {
        mi = &cached->second;
    } else {
        // 在类级+方法级合并替换下解析方法签名
        TypeSubst full = classSubst;
        for (const auto& kv : methodSubst) full[kv.first] = kv.second;
        auto savedSubst2 = currentSubst_;
        auto savedPrefix2 = currentPkgPrefix_;
        currentSubst_ = full;
        currentPkgPrefix_ = gm.pkgPrefix;   // 签名里同包类型裸名（如 List）需模板前缀
        // 保留类级/方法级类型参数名：resolveType 据此把它们识别为类型参数，
        // 再经 substType 展开为具体类型（否则 T/R 会被当成未定义类名）。
        currentTypeParams_.clear();
        if (tmpl) for (const auto& tp : tmpl->typeParams) currentTypeParams_.insert(tp);
        for (const auto& tp : gm.typeParams) currentTypeParams_.insert(tp);

        MethodInfo nmi;
        nmi.name       = gm.methodName;
        nmi.irName     = "@" + instName;
        nmi.ownerClass = ci->name;
        nmi.returnType = fn->returnType() ? resolveType(fn->returnType()->type())
                                          : Type::makeUnit();
        if (auto* pl = fn->paramList())
            for (auto* p : pl->param()) {
                nmi.paramNames.push_back(p->IDENT()->getText());
                nmi.paramTypes.push_back(resolveType(p->type()));
            }
        currentSubst_ = savedSubst2;
        currentPkgPrefix_ = savedPrefix2;

        methodInstanceInfos_[instName] = nmi;
        PendingMethodInstance pi;
        pi.tmpl = new GenericMethod(gm);   // 独立拷贝，防模板对象失效
        pi.instClass = ci->name;
        pi.classSubst = classSubst;
        pi.methodSubst = methodSubst;
        pi.pkgPrefix = gm.pkgPrefix;
        pi.instName = instName;
        pendingMethodInstances_.push_back(std::move(pi));
        mi = &methodInstanceInfos_[instName];
    }

    // ---- 7. 生成静态调用（this + 实参）----
    if (args.size() != mi->paramTypes.size()) {
        error(ctx, "方法 '" + ci->name + "." + gm.methodName + "' 需要 " +
                   std::to_string(mi->paramTypes.size()) + " 个参数，实际提供 " +
                   std::to_string(args.size()) + " 个");
        return Value();
    }
    std::string argStr = "ptr " + recv.ir;
    for (size_t k = 0; k < args.size(); ++k) {
        if (!isAssignable(args[k].type, mi->paramTypes[k])) {
            error(ctx, "方法 '" + ci->name + "." + gm.methodName + "' 第 " +
                       std::to_string(k + 1) + " 个参数类型不匹配：期望 " +
                       mi->paramTypes[k]->toString() + "，实际 " +
                       args[k].type->toString());
            return Value();
        }
        args[k] = coerce(args[k], mi->paramTypes[k], 0, 0);
        auto* aex = call->argList() ? call->argList()->arg(k)->expr() : nullptr;
        argStr += ", " + formatCallArg(mi->paramTypes[k], aex, args[k]);
    }
    if (mi->returnType->isUnit()) {
        em_.emit("call void " + mi->irName + "(" + argStr + ")");
        return Value("", Type::makeUnit());
    }
    std::string reg = em_.nextTemp();
    em_.emit(reg + " = call " + mi->returnType->llvmType() + " " +
             mi->irName + "(" + argStr + ")");
    return Value(reg, mi->returnType);
}

void IRGen::genPendingMethodInstances() {
    while (!pendingMethodInstances_.empty()) {
        auto batch = pendingMethodInstances_;
        pendingMethodInstances_.clear();
        for (auto& pi : batch) {
            if (generatedMethodInstances_.count(pi.instName)) continue;
            generatedMethodInstances_.insert(pi.instName);
            auto it = methodInstanceInfos_.find(pi.instName);
            if (it == methodInstanceInfos_.end()) continue;
            auto ci = lookupClass(pi.instClass);
            if (!ci) continue;
            MethodInfo mi = it->second;
            auto* fn = pi.tmpl->decl;
            if (!fn) continue;

            // 类级+方法级合并替换下生成方法体
            TypeSubst full = pi.classSubst;
            for (const auto& kv : pi.methodSubst) full[kv.first] = kv.second;
            auto savedSubst  = currentSubst_;
            auto savedParams = currentTypeParams_;
            auto savedPrefix = currentPkgPrefix_;
            auto savedClass  = currentClass_;
            auto savedImports = currentImports_;
            auto savedImportPath = currentImportPath_;
            currentSubst_ = full;
            currentTypeParams_.clear();
            if (auto tmplC = lookupClass(ci->instanceOf))
                for (const auto& tp : tmplC->typeParams) currentTypeParams_.insert(tp);
            for (const auto& tp : pi.tmpl->typeParams) currentTypeParams_.insert(tp);
            currentPkgPrefix_ = pi.pkgPrefix;
            restoreImportsForPkgPrefix(currentPkgPrefix_);
            currentClass_ = ci;
            genMethodBodyFromMI(fn, mi, ci);
            currentSubst_  = savedSubst;
            currentTypeParams_ = savedParams;
            currentPkgPrefix_  = savedPrefix;
            currentClass_  = savedClass;
            currentImports_ = savedImports;
            currentImportPath_ = savedImportPath;

            delete pi.tmpl;
        }
    }
}

void IRGen::genMethodBodyFromMI(HaoLangParser::FuncDeclContext* fn,
                                const MethodInfo& mi, const ClassInfoPtr& ci) {
    if (!fn->block()) return;
    em_.resetFunctionState();
    currentReturn_ = mi.returnType;
    sawReturn_ = false;
    blockTerminated_ = false;
    inMain_ = false;
    currentClass_ = ci;
    inConstructor_ = false;
    thisAddr_.clear();

    std::string sig = "define " + mi.returnType->llvmType() + " " +
                      mi.irName + "(ptr %this.arg";
    for (size_t i = 0; i < mi.paramTypes.size(); ++i)
        sig += ", " + mi.paramTypes[i]->llvmType() + " %" + mi.paramNames[i] + ".arg";
    sig += ") {";

    em_.emitBlank();
    em_.emitRaw(sig);
    em_.emitLabel("entry");
    genFunctionBody(fn->block(), mi.paramNames, mi.paramTypes,
                    mi.returnType, /*isMain=*/false, /*hasThis=*/true, ci->name,
                    fn, "方法 '" + ci->name + "." + mi.name + "'");
    em_.flushEntryAllocas();
    em_.emitRaw("}");
    currentClass_ = nullptr;
    thisAddr_.clear();
}

// ------------------------------------------------------------
//  静态字段全局变量
// ------------------------------------------------------------
//  静态字段是类级全局变量（@Class.X = global <type> <init>），
//  不占对象布局槽位。初始值只支持编译期常量（字面量 / 负号 / 字符串 /
//  括号）；非常量运行期初始化（如 new/func 调用）留待 v0.17.0 静态构造器。
std::string IRGen::evalStaticInit(antlr4::tree::ParseTree* node, TypeKind kind) {
    if (!node) return "";
    // ---- 单子下降：expr 链（assign→ternary→nullCoalesce→...→primary）----
    if (node->children.size() == 1) {
        auto r = evalStaticInit(node->children[0], kind);
        if (!r.empty()) return r;
    }
    // ---- 括号 (expr) ----
    if (auto* pp = dynamic_cast<HaoLangParser::ParenPrimaryContext*>(node)) {
        auto r = evalStaticInit(pp->expr(), kind);
        if (!r.empty()) return r;
    }
    // ---- 一元负号 -expr ----
    if (auto* ue = dynamic_cast<HaoLangParser::UnaryExprContext*>(node)) {
        if (ue->MINUS()) {
            auto inner = evalStaticInit(ue->unaryExpr(), kind);
            if (!inner.empty() && inner[0] != '@') {
                if (inner[0] == '-') return inner.substr(1);
                return "-" + inner;
            }
        }
        return "";
    }
    // ---- 字面量 ----
    if (auto* lit = dynamic_cast<HaoLangParser::LiteralContext*>(node)) {
        if (auto* t = lit->INT_LIT()) {
            std::string s = StringUtil::stripUnderscores(t->getText());
            // 无符号 / 超大十六进制须用 strtoull，避免 strtoll 溢出夹成 LLONG_MAX
            bool asUnsigned = (kind == TypeKind::Byte || kind == TypeKind::UShort ||
                               kind == TypeKind::UInt || kind == TypeKind::ULong ||
                               kind == TypeKind::UIntPtr);
            unsigned long long uv = 0;
            long long v = 0;
            if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
                uv = std::strtoull(s.c_str() + 2, nullptr, 16);
            else if (s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B'))
                uv = std::strtoull(s.c_str() + 2, nullptr, 2);
            else if (s[0] == '-')
                v = std::strtoll(s.c_str(), nullptr, 10);
            else
                uv = std::strtoull(s.c_str(), nullptr, 10);
            if (s[0] != '-') v = static_cast<long long>(uv);
            if (asUnsigned && s[0] != '-')
                return std::to_string(uv);
            return std::to_string(v);
        }
        if (auto* t = lit->FLOAT_LIT()) {
            std::string s = StringUtil::stripUnderscores(t->getText());
            if (s.find('.') == std::string::npos &&
                s.find('e') == std::string::npos &&
                s.find('E') == std::string::npos)
                s += ".0";
            // LLVM IR：float 类型常量用十六进制（双精度位模式），避免大十进制溢出
            if (kind == TypeKind::Float) {
                float fv = strtof(s.c_str(), nullptr);
                double dv = static_cast<double>(fv);
                uint64_t bits = 0;
                static_assert(sizeof(dv) == sizeof(bits), "double size");
                memcpy(&bits, &dv, sizeof(bits));
                char buf[32];
                snprintf(buf, sizeof(buf), "0x%016llX",
                         static_cast<unsigned long long>(bits));
                return std::string(buf);
            }
            return s;
        }
        if (lit->TRUE())  return "1";
        if (lit->FALSE()) return "0";
        if (auto* t = lit->STRING_LIT()) {
            // 返回 intern 的 C 串（@…）。静态全局不能直接存为 HaoString*，
            // emitStaticFieldGlobals / staticinit 会把 String 改走 runtime 包装；
            // emitNewFactory 用 @… → hao_str_from_cstr。
            return em_.internString(StringUtil::unescapeStringLiteral(t->getText()));
        }
        return "";
    }
    return "";
}

void IRGen::emitStaticFieldGlobals(const ClassInfoPtr& ci) {
    for (const auto& f : ci->staticFields) {
        std::string gname = "@" + ci->name + "." + f.name;
        std::string lbase = f.type->llvmType();
        std::string init;
        if (f.defaultExpr) {
            init = evalStaticInit(
                static_cast<antlr4::tree::ParseTree*>(f.defaultExpr), f.type->kind);
            // String 字面量返回 @cstr，不能写进 global ptr；与非常量一样走 staticinit
            if (init.empty() || f.type->kind == TypeKind::String) {
                init = zeroValueFor(f.type);
            }
        } else {
            init = zeroValueFor(f.type);
        }
        em_.addGlobal(gname + " = private global " + lbase + " " + init);
        if (isGcPointerType(f.type))
            staticGcRootGlobals_.push_back(gname);
    }
}

void IRGen::emitStaticGcRootRegistration() {
    // 去重（泛型/多路径可能重复登记同名全局）；无字段时仍生成空函数供 main 调用
    std::set<std::string> seen;
    std::vector<std::string> uniq;
    for (const auto& g : staticGcRootGlobals_) {
        if (seen.insert(g).second) uniq.push_back(g);
    }
    em_.emitBlank();
    em_.emitRaw("define void @hao.registerStaticRoots() {");
    em_.emitLabel("entry");
    for (const auto& g : uniq)
        em_.emit("call void @hao_gc_add_root_slot(ptr " + g + ")");
    em_.emit("ret void");
    em_.emitRaw("}");
}

// 类型的零值常量（用于全局零初始化）
std::string IRGen::zeroValueFor(const TypePtr& t) {
    // Int?/String/Class 等 llvmType=ptr → null（勿写 ptr 0，虽同值但语义不清且易漏可空）
    if (t->llvmType() == "ptr") return "null";
    if (t->kind == TypeKind::Double || t->kind == TypeKind::Float) return "0.0";
    if (t->kind == TypeKind::Bool)   return "0";
    return "0";
}

// ------------------------------------------------------------
//  静态构造器（C# 风格 static ClassName()）
// ------------------------------------------------------------
//  类首次被引用（访问静态成员 / 首次 new）前自动执行一次静态初始化。
//  生成三个符号：
//    @Class.initguard  私有全局守卫（0=未开始 1=进行中 2=完成）
//    @Class.staticinit  静态初始化函数：非常量静态字段初始化 + 静态构造器体
//    @Class.ensureInit  惰性触发：guard==0 时调 staticinit
//  静态构造器体里访问同类静态成员会再次走 ensureInit，但 guard 已置 1，
//  不会递归（Java 语义：初始化进行中访问静态字段读到当前值）。
void IRGen::markStaticInitFlags() {
    for (auto& [name, ci] : classes_) {
        (void)name;
        if (!ci || ci->isGenericTemplate()) continue;
        if (ci->hasStaticInit) continue;
        if (ci->staticCtorNode || ci->isEnum) {
            ci->hasStaticInit = true;
            continue;
        }
        for (const auto& f : ci->staticFields) {
            // 合成 Class 无 defaultExpr，亦须 ensureInit
            if (f.name == "Class" && f.type && f.type->kind == TypeKind::Class &&
                f.type->className == "reflect$Class" && !f.defaultExpr) {
                ci->hasStaticInit = true;
                break;
            }
            if (!f.defaultExpr) continue;
            auto e = evalStaticInit(
                static_cast<antlr4::tree::ParseTree*>(f.defaultExpr), f.type->kind);
            // String / lambda / 其它非常量：须走 staticinit
            if (e.empty() || f.type->kind == TypeKind::String) {
                ci->hasStaticInit = true;
                break;
            }
        }
    }
}

void IRGen::ensureClassStaticField(const ClassInfoPtr& ci) {
    if (!ci || ci->isGenericTemplate() || !ci->hasVTable) return;
    if (!lookupClass("reflect$Class")) return;
    if (const FieldInfo* ex = ci->findStaticField("Class")) {
        if (!ex->type || ex->type->kind != TypeKind::Class ||
            ex->type->className != "reflect$Class") {
            diags_.error(ex->line, ex->column,
                         "静态字段 'Class' 为编译器保留名，类型须为 reflect.Class");
        }
        return;
    }
    FieldInfo sf;
    sf.name = "Class";
    sf.type = Type::makeClass("reflect$Class");
    sf.isStatic = true;
    sf.isMutable = false;
    sf.ownerClass = ci->name;
    sf.visibility = FieldInfo::Vis::Public;
    sf.line = ci->line;
    sf.column = ci->column;
    ci->staticFields.push_back(sf);
    ci->hasStaticInit = true;
}

void IRGen::synthesizeClassStaticFields() {
    for (auto& [cname, ci] : classes_) {
        (void)cname;
        ensureClassStaticField(ci);
    }
}

void IRGen::genStaticConstructor(const ClassInfoPtr& ci) {
    bool hasCtor = ci->staticCtorNode != nullptr;
    bool hasRuntimeField = false;
    bool hasClassToken = false;
    for (const auto& f : ci->staticFields) {
        if (f.name == "Class" && f.type && f.type->kind == TypeKind::Class &&
            f.type->className == "reflect$Class" && !f.defaultExpr) {
            hasClassToken = true;
        }
        if (!f.defaultExpr) continue;
        auto e = evalStaticInit(
            static_cast<antlr4::tree::ParseTree*>(f.defaultExpr), f.type->kind);
        // String 字面量虽有 @cstr，仍须 runtime 包成 HaoString
        if (e.empty() || f.type->kind == TypeKind::String) {
            hasRuntimeField = true;
            break;
        }
    }
    if (!hasCtor && !hasRuntimeField && !hasClassToken) return;
    ci->hasStaticInit = true;

    em_.addGlobal("@" + ci->name + ".initguard = private global i32 0");

    // ---- @Class.staticinit ----
    em_.resetFunctionState();
    currentReturn_ = Type::makeUnit();
    sawReturn_ = false;
    blockTerminated_ = false;
    inMain_ = false;
    currentClass_ = ci;
    inConstructor_ = false;
    thisAddr_.clear();

    em_.emitRaw("define void @" + ci->name + ".staticinit() {");
    em_.emitLabel("entry");
    /* v0.53.5：入口 safepoint（分配密集的静态/枚举初始化也能握手） */
    em_.emit("call void @hao_gc_safepoint()");

    // 合成 TypeName.Class ← classOfMeta(@T.meta)（枚举/普通类共用）
    if (hasClassToken) {
        std::string tok = em_.nextTemp();
        em_.emit(tok + " = call ptr @reflect$classOfMeta(ptr @" + ci->name +
                 ".meta)");
        emitGlobalGcStore("@" + ci->name + ".Class", tok,
                          Type::makeClass("reflect$Class"));
    }

    if (ci->isEnum) {
        // ---- 枚举：创建各常量实例并存入静态字段 ----
        auto* ed = static_cast<HaoLangParser::EnumDeclContext*>(ci->staticCtorNode);
        int ord = 0;
        TypePtr enumTy = Type::makeClass(ci->name);
        for (auto* ec : ed->enumConstant()) {
            std::string cname = ec->IDENT()->getText();
            std::string obj = emitObjectNew(ci->slotCount(), objectPtrBitmap(ci.get()));
            std::string vtp = em_.nextTemp();
            em_.emit(vtp + " = getelementptr ptr, ptr " + obj + ", i64 0");
            em_.emit("store ptr " + ci->vtableIRName + ", ptr " + vtp);
            std::string nameC = em_.internString(cname);
            std::string nameS = em_.nextTemp();
            em_.emit(nameS + " = call ptr @hao_str_from_cstr(ptr " + nameC + ")");
            em_.emit("call void " + ci->ctorIRName + "(ptr " + obj +
                     ", ptr " + nameS + ", i32 " +
                     std::to_string(ord) + ")");
            emitGlobalGcStore("@" + ci->name + "." + cname, obj, enumTy);
            ++ord;
        }
        em_.emit("ret void");
    } else {
    // 非常量静态字段运行时初始化（声明顺序）
    for (const auto& f : ci->staticFields) {
        if (!f.defaultExpr) continue;
        auto e = evalStaticInit(
            static_cast<antlr4::tree::ParseTree*>(f.defaultExpr), f.type->kind);
        // 非 String 常量已在全局；String / 非常量（含 lambda）走 genExpr
        if (!e.empty() && f.type->kind != TypeKind::String) continue;
        auto* dexpr = static_cast<HaoLangParser::ExprContext*>(f.defaultExpr);
        expectedTypes_.push_back(f.type);
        ExpectedTypeGuard eg{this};
        analyzeLambdas(dexpr);
        Value dv = genExpr(dexpr);
        if (!dv.valid()) return;
        if (!isAssignable(dv.type, f.type)) {
            error(dexpr, "静态字段 '" + f.name + "' 的初始值类型 " +
                         dv.type->toString() + " 与声明类型 " +
                         f.type->toString() + " 不匹配");
            return;
        }
        dv = coerce(dv, f.type, 0, 0);
        emitGlobalGcStore("@" + ci->name + "." + f.name, dv.ir, f.type);
    }

    // 显式静态构造器体
    if (hasCtor) {
        auto* sc = static_cast<HaoLangParser::StaticCtorDeclContext*>(ci->staticCtorNode);
        genFunctionBody(sc->block(), {}, {}, Type::makeUnit(),
                        /*isMain=*/false, /*hasThis=*/false, ci->name, sc,
                        "静态构造器 '" + ci->name + "'");
    } else if (!blockTerminated_) {
        em_.emit("ret void");
    }
    }
    em_.flushEntryAllocas();
    em_.emitRaw("}");
    currentClass_ = nullptr;
    thisAddr_.clear();

    // ---- @Class.ensureInit ----
    em_.resetFunctionState();
    std::string g = em_.nextTemp();
    std::string n = em_.nextTemp();
    std::string initL = em_.nextLabel("q.init");
    std::string doneL = em_.nextLabel("q.done");
    em_.emitRaw("define void @" + ci->name + ".ensureInit() {");
    em_.emitLabel("entry");
    em_.emit(g + " = load i32, ptr @" + ci->name + ".initguard");
    em_.emit(n + " = icmp eq i32 " + g + ", 0");
    em_.emit("br i1 " + n + ", label %" + initL + ", label %" + doneL);
    em_.emitLabel(initL);
    em_.emit("store i32 1, ptr @" + ci->name + ".initguard");
    em_.emit("call void @" + ci->name + ".staticinit()");
    em_.emit("store i32 2, ptr @" + ci->name + ".initguard");
    em_.emit("br label %" + doneL);
    em_.emitLabel(doneL);
    em_.emit("ret void");
    em_.emitRaw("}");
}

void IRGen::emitStaticEnsureInit(const ClassInfoPtr& ci) {
    if (ci->hasStaticInit)
        em_.emit("call void @" + ci->name + ".ensureInit()");
}
} // namespace hao
