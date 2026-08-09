// ============================================================
//  HaoLang 符号表与作用域链
// ============================================================
#pragma once

#include "Type.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace hao {

// ------------------------------------------------------------
//  符号
// ------------------------------------------------------------
enum class SymbolKind {
    Variable,   // val / var / 参数
    Function,
    Class,
};

// 注解使用（v0.19.0）：@Name(args) 标记在类/方法/字段上。
// args 是编译期常量求值后的键值对（键空表示单值注解，如 @Deprecated）。
struct AnnotationUse {
    std::string name;                              // 注解类型内部名（pkg$Anno，供调试/兼容）
    std::string className;                         // 注解 ClassInfo 名；空则无 meta
    std::vector<std::pair<std::string, std::string>> args;  // {键, 值字符串}
};

// 类的字段
struct FieldInfo {
    std::string name;
    TypePtr type;
    int slot = 0;        // 在对象中的槽位下标（每槽 8 字节）
    bool isMutable = true;

    // 是否静态字段（类级全局变量，不占对象布局槽位）
    bool isStatic = false;

    // 声明处的初始值表达式（void* 以避免头文件依赖 ANTLR 类型）。
    // 由 new 表达式在调用构造函数之前生成，语义同 Java/C#。
    void* defaultExpr = nullptr;

    // 声明该字段的类名。子类继承父类字段时用于可见性校验。
    std::string ownerClass;

    // 字段上的注解（v0.19.0）
    std::vector<AnnotationUse> annotations;

    enum class Vis { Public, Protected, Private, Internal };
    Vis visibility = Vis::Public;

    size_t line = 0;
    size_t column = 0;
};

// 类的方法
struct MethodInfo {
    std::string name;
    std::string irName;              // IR 中的全局名，如 @Point.describe
    std::vector<TypePtr> paramTypes; // 不含隐式 this
    std::vector<std::string> paramNames;
    TypePtr returnType;

    // 虚表槽位。>= 0 表示该方法参与动态分派（实现了接口方法，
    // 或被子类覆写 / 覆写了父类方法）；-1 表示静态绑定，零开销。
    int vtableSlot = -1;

    // 是否带 override 修饰
    bool isOverride = false;

    // 抽象方法：接口方法，或抽象类中无实现体的方法
    bool isAbstract = false;

    // 是否静态方法（无隐式 this，ClassName.f() 直接静态调用）
    bool isStatic = false;

    // 声明该方法的类名。用于继承：子类继承父类方法时，
    // 虚表项仍指向父类的实现（@Animal.info），不重复生成代码。
    std::string ownerClass;

    // 方法上的注解（v0.19.0）
    std::vector<AnnotationUse> annotations;

    // 可见性
    enum class Vis { Public, Protected, Private, Internal };
    Vis visibility = Vis::Public;

    size_t line = 0;
    size_t column = 0;

    // 方法签名是否一致（用于校验接口实现与 override）
    bool signatureMatches(const MethodInfo& o) const {
        if (paramTypes.size() != o.paramTypes.size()) return false;
        for (size_t i = 0; i < paramTypes.size(); ++i)
            if (!paramTypes[i]->equals(*o.paramTypes[i])) return false;
        return returnType->equals(*o.returnType);
    }

    // 形如 "(Int, String) -> Bool" 的签名描述，用于错误信息
    std::string signatureString() const {
        std::string s = "(";
        for (size_t i = 0; i < paramTypes.size(); ++i) {
            if (i) s += ", ";
            s += paramTypes[i]->toString();
        }
        return s + ") -> " + returnType->toString();
    }
};

// ------------------------------------------------------------
//  接口
// ------------------------------------------------------------
struct InterfaceInfo {
    std::string name;
    // 方法按声明顺序排列，下标即虚表槽位
    std::vector<MethodInfo> methods;

    // ---- 泛型接口（v0.18.0）----
    //  与泛型类同路线：typeParams 非空 => 本条目是泛型接口模板，不参与运行时；
    //  接口实例（Iterable$Int）是模板副本 + substType 替换，instanceOf 指向模板名。
    std::vector<std::string> typeParams;
    // 模板的语法树节点（void* 以避免头文件依赖 ANTLR 类型），实例化时从中取方法
    void* declNode = nullptr;
    std::string instanceOf;
    std::vector<TypePtr> typeArgs;

    bool isGenericTemplate() const { return !typeParams.empty(); }
    bool isGenericInstance() const { return !instanceOf.empty(); }

    const MethodInfo* findMethod(const std::string& n) const {
        for (const auto& m : methods) if (m.name == n) return &m;
        return nullptr;
    }
    int slotOf(const std::string& n) const {
        for (size_t i = 0; i < methods.size(); ++i)
            if (methods[i].name == n) return static_cast<int>(i);
        return -1;
    }
};

using InterfaceInfoPtr = std::shared_ptr<InterfaceInfo>;

// 类的完整信息
struct ClassInfo {
    std::string name;
    // 所属包 importPath（""=main / "calc" / "util/strings"），供 internal 可见性
    std::string importPath;

    // ---- 泛型 ----
    //  采用单态化：泛型类声明本身不生成代码，只作为模板保存；
    //  每遇到一个新的类型参数组合（如 Box<Int>）就生成一份专用代码，
    //  实例名为 Box$Int。这样泛型没有运行时开销，
    //  与 C++ 模板、Rust 泛型同路线。
    //
    //  typeParams 非空 => 本条目是泛型模板，不参与代码生成
    std::vector<std::string> typeParams;
    // 模板的语法树节点（void* 以避免头文件依赖 ANTLR 类型），
    // 实例化时重新遍历它并施加类型替换
    void* declNode = nullptr;

    //  以下两项仅实例出现：
    //  instanceOf 指向模板名，typeArgs 是实际类型参数
    std::string instanceOf;
    std::vector<TypePtr> typeArgs;

    bool isGenericTemplate() const { return !typeParams.empty(); }
    bool isGenericInstance() const { return !instanceOf.empty(); }

    // ---- 继承 ----
    //  基类名，空表示无父类。
    //  字段布局：父类字段在前，子类新增字段追加在后，
    //  这样父类字段的槽位在子类中保持不变，父类型引用可直接访问。
    //
    //      Animal { age }        -> [0]=vtable, [1]=age
    //      Dog : Animal { name } -> [0]=vtable, [1]=age, [2]=name
    //
    //  虚表布局：子类继承父类的全部槽位，覆写时替换对应项，
    //  新增虚方法追加在后。因此父类型引用调用虚方法总能命中。
    std::string baseName;
    ClassInfo* base = nullptr;      // 解析后指向父类，便于沿链查找

    // 抽象类不能被实例化
    bool isAbstract = false;

    // 是否是 enum 枚举类（常量集合 + 名称/序数，Java 风格）
    bool isEnum = false;

    // 是否是 annotation 注解类型（v0.19.0）
    bool isAnnotation = false;

    // 类上的注解（v0.19.0）
    std::vector<AnnotationUse> annotations;

    // fields / methods 包含从父类继承来的项（扁平化），
    // 便于查找与虚表构建；ownerClass 标明真正的声明者。
    std::vector<FieldInfo> fields;
    std::vector<MethodInfo> methods;

    // ---- 静态成员（类级，不参与对象布局 / 虚表 / 继承）----
    //  静态字段是每个类一份的全局变量，不占对象槽位；
    //  静态方法无隐式 this，`ClassName.f()` 直接静态调用。
    //  静态成员不属于继承体系，故不随父类扁平化。
    std::vector<FieldInfo> staticFields;
    std::vector<MethodInfo> staticMethods;

    // C# 风格静态构造器（static ClassName() { ... }），
    // 类首次被引用（访问静态成员 / 首次 new）前自动执行一次。
    void* staticCtorNode = nullptr;

    // 代码生成阶段由 genStaticConstructor 判定并置位：
    // 该类是否有需要惰性运行的静态初始化（显式静态构造器，或
    // 含非常量初始值的静态字段——其初始化代码被合成进静态构造器）。
    bool hasStaticInit = false;

    std::string ctorIRName;                    // 构造函数 IR 名，空表示无
    std::vector<TypePtr> ctorParamTypes;
    std::vector<std::string> ctorParamNames;

    // ---- 接口实现与虚表 ----
    //  虚表槽位采用「全局编号」：接口方法由接口注册顺序统一分配，
    //  类自己的虚方法（被覆写或覆写父类的）追加在接口方法之后。
    //  所有类的虚表都按同一编号空间布局，因此调用点只需知道
    //  方法的槽位号即可正确分派，无需了解对象的具体类型。
    //
    //  对象槽位 0 存虚表指针，字段从槽位 1 开始。
    //  完全不参与动态分派的类（无接口、无继承、无虚方法）不带虚表，
    //  字段从槽位 0 开始，不为其白付 8 字节。
    std::vector<std::string> interfaceNames;
    bool hasVTable = false;
    std::string vtableIRName;                  // 如 @C.vtable
    std::vector<std::string> vtableEntries;    // 按全局槽位排列

    // 该接口在本类 interfaceNames 中的下标；-1 表示未实现
    int interfaceIndex(const std::string& iname) const {
        for (size_t i = 0; i < interfaceNames.size(); ++i)
            if (interfaceNames[i] == iname) return static_cast<int>(i);
        return -1;
    }

    // 字段起始槽位：有虚表为 1，否则为 0
    int fieldBase() const { return hasVTable ? 1 : 0; }

    // 声明位置，供跨阶段校验（如接口实现检查）报错时定位
    size_t line = 0;
    size_t column = 0;

    const FieldInfo* findField(const std::string& n) const {
        for (const auto& f : fields) if (f.name == n) return &f;
        return nullptr;
    }
    const MethodInfo* findMethod(const std::string& n) const {
        for (const auto& m : methods) if (m.name == n) return &m;
        return nullptr;
    }
    const MethodInfo* findMethod(const std::string& n,
                                 const std::vector<TypePtr>& params) const {
        for (const auto& m : methods) {
            if (m.name != n || m.paramTypes.size() != params.size()) continue;
            bool ok = true;
            for (size_t i = 0; i < params.size(); ++i) {
                if (!m.paramTypes[i] || !params[i] ||
                    !m.paramTypes[i]->equals(*params[i])) {
                    ok = false;
                    break;
                }
            }
            if (ok) return &m;
        }
        return nullptr;
    }
    std::vector<const MethodInfo*> findMethods(const std::string& n) const {
        std::vector<const MethodInfo*> out;
        for (const auto& m : methods)
            if (m.name == n) out.push_back(&m);
        return out;
    }
    const FieldInfo* findStaticField(const std::string& n) const {
        for (const auto& f : staticFields) if (f.name == n) return &f;
        return nullptr;
    }
    const MethodInfo* findStaticMethod(const std::string& n) const {
        for (const auto& m : staticMethods) if (m.name == n) return &m;
        return nullptr;
    }
    // 按精确参数类型匹配静态重载（元素 equals，不含可赋值拓宽）
    const MethodInfo* findStaticMethod(const std::string& n,
                                       const std::vector<TypePtr>& params) const {
        for (const auto& m : staticMethods) {
            if (m.name != n || m.paramTypes.size() != params.size()) continue;
            bool ok = true;
            for (size_t i = 0; i < params.size(); ++i) {
                if (!m.paramTypes[i] || !params[i] ||
                    !m.paramTypes[i]->equals(*params[i])) {
                    ok = false;
                    break;
                }
            }
            if (ok) return &m;
        }
        return nullptr;
    }
    std::vector<const MethodInfo*> findStaticMethods(const std::string& n) const {
        std::vector<const MethodInfo*> out;
        for (const auto& m : staticMethods)
            if (m.name == n) out.push_back(&m);
        return out;
    }
    MethodInfo* findMethodMut(const std::string& n) {
        for (auto& m : methods) if (m.name == n) return &m;
        return nullptr;
    }

    // 只在本类自己声明的成员中查找（不含继承）
    const MethodInfo* findOwnMethod(const std::string& n) const {
        for (const auto& m : methods)
            if (m.name == n && m.ownerClass == name) return &m;
        return nullptr;
    }
    const MethodInfo* findOwnMethod(const std::string& n,
                                    const std::vector<TypePtr>& params) const {
        for (const auto& m : methods) {
            if (m.name != n || m.ownerClass != name) continue;
            if (m.paramTypes.size() != params.size()) continue;
            bool ok = true;
            for (size_t i = 0; i < params.size(); ++i) {
                if (!m.paramTypes[i] || !params[i] ||
                    !m.paramTypes[i]->equals(*params[i])) {
                    ok = false;
                    break;
                }
            }
            if (ok) return &m;
        }
        return nullptr;
    }

    bool implementsInterface(const std::string& iname) const {
        // 沿继承链查找：父类实现的接口，子类同样满足
        for (const ClassInfo* c = this; c; c = c->base)
            for (const auto& n : c->interfaceNames)
                if (n == iname) return true;
        return false;
    }

    // 本类是否为 other 的子类（含自身）
    bool isSubclassOf(const std::string& other) const {
        for (const ClassInfo* c = this; c; c = c->base)
            if (c->name == other) return true;
        return false;
    }

    // 对象需要的槽位总数
    size_t slotCount() const { return fields.size() + (hasVTable ? 1 : 0); }
};

using ClassInfoPtr = std::shared_ptr<ClassInfo>;

struct Symbol {
    SymbolKind kind = SymbolKind::Variable;
    std::string name;
    TypePtr type;

    // val 声明为不可变，赋值时报错
    bool isMutable = true;

    // ---- 变量：IR 中的地址寄存器名，如 "%x.addr" ----
    std::string irAddr;

    // 被 lambda 按引用捕获的可变变量会被「装箱」到堆 cell：
    //   boxed=true 时，irAddr 指向一个栈上的 ptr，ptr 再指向堆上
    //   8 字节的真实值。外层与所有捕获该变量的 lambda 共享同一个 cell，
    //   从而保证 lambda 内修改能被外层和其它 lambda 观察到。
    bool boxed = false;

    // 数组形参按引用传递（v0.34.1）：irAddr 即为调用方槽位指针（ptr*），
    // 形参上 `arr += x` 扩容后写回调用方，避免别名仍持旧数组指针。
    bool byRefParam = false;

    // ---- 函数：IR 中的全局名，如 "@main" ----
    std::string irName;

    // ---- 函数：参数与返回类型 ----
    std::vector<TypePtr> paramTypes;
    std::vector<std::string> paramNames;
    TypePtr returnType;

    // ---- 类：字段与方法信息 ----
    ClassInfoPtr classInfo;

    // 顶层函数/类是否为 private（包私有，跨包不可见）
    bool isPrivate = false;

    // extern 函数：声明外部 C 函数（运行时/系统库），无函数体，
    // 调用直接 call @linkName，并在 IR 中输出对应的 declare。
    bool isExtern = false;
    std::string linkName;   // C 符号名（不含 @），如 "hao_println_int"
    std::vector<std::string> linkLibs;   // @link("ws2_32") 声明的外部链接库

    // 声明位置，用于错误信息
    size_t line = 0;
    size_t column = 0;
};

using SymbolPtr = std::shared_ptr<Symbol>;

// ------------------------------------------------------------
//  作用域：单层名字空间，通过 parent 形成链
// ------------------------------------------------------------
class Scope {
public:
    explicit Scope(Scope* parent = nullptr) : parent_(parent) {}

    // 在当前层声明；重复声明返回 false
    bool declare(const SymbolPtr& sym) {
        if (symbols_.count(sym->name)) return false;
        symbols_[sym->name] = sym;
        order_.push_back(sym->name);
        return true;
    }

    // 仅查当前层
    SymbolPtr lookupLocal(const std::string& name) const {
        auto it = symbols_.find(name);
        return it == symbols_.end() ? nullptr : it->second;
    }

    // 沿作用域链向上查找
    SymbolPtr lookup(const std::string& name) const {
        for (const Scope* s = this; s; s = s->parent_) {
            auto it = s->symbols_.find(name);
            if (it != s->symbols_.end()) return it->second;
        }
        return nullptr;
    }

    Scope* parent() const { return parent_; }

    // 按声明顺序遍历（错误信息与调试用）
    const std::vector<std::string>& declOrder() const { return order_; }

private:
    Scope* parent_;
    std::map<std::string, SymbolPtr> symbols_;
    std::vector<std::string> order_;
};

// ------------------------------------------------------------
//  符号表：管理作用域栈的生命周期
// ------------------------------------------------------------
class SymbolTable {
public:
    SymbolTable() {
        // 全局作用域
        scopes_.push_back(std::make_unique<Scope>(nullptr));
    }

    void push() {
        scopes_.push_back(std::make_unique<Scope>(current()));
    }

    void pop() {
        // 全局作用域不允许弹出
        if (scopes_.size() > 1) scopes_.pop_back();
    }

    Scope* current() { return scopes_.back().get(); }
    Scope* global()  { return scopes_.front().get(); }

    bool declare(const SymbolPtr& s) { return current()->declare(s); }
    SymbolPtr lookup(const std::string& n) { return current()->lookup(n); }
    SymbolPtr lookupLocal(const std::string& n) { return current()->lookupLocal(n); }

    // 在全局层声明（函数、类）
    bool declareGlobal(const SymbolPtr& s) { return global()->declare(s); }

    size_t depth() const { return scopes_.size(); }
    bool atGlobal() const { return scopes_.size() == 1; }

    // RAII 作用域守卫，避免忘记 pop
    class Guard {
    public:
        explicit Guard(SymbolTable& t) : t_(t) { t_.push(); }
        ~Guard() { t_.pop(); }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    private:
        SymbolTable& t_;
    };

private:
    std::vector<std::unique_ptr<Scope>> scopes_;
};

} // namespace hao
