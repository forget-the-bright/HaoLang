// ============================================================
//  HaoLang 类型系统（v0.27：+ Char 码点；String=ptr→HaoString 头）
//  v0.55.58：语言可见四属性 isValueType / isGcManaged / isUnsafePointer
//            （口径 docs/类型属性.md；与 isReferenceType 配套）
// ============================================================
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <map>

namespace hao {

// ------------------------------------------------------------
//  类型种类
//  Int=i32 / UInt=u32 / Long=i64 / ULong=u64 / …
//  无符号与有符号同宽共用 LLVM 整数类型，差异在 sext/zext、icmp、div
// ------------------------------------------------------------
enum class TypeKind {
    Unknown,
    Unit,
    SByte,     // i8  有符号 -128～127
    Byte,      // u8  无符号 0～255
    Short,     // i16
    UShort,    // u16
    Int,       // i32
    UInt,      // u32
    Long,      // i64
    ULong,     // u64
    UIntPtr,   // 指针宽无符号（本宿主固定 64 → 同 ULong）
    Float,     // f32
    Double,    // f64
    Bool,      // 存 i8（0/1；对齐 Go/C#/Rust/Java boolean[]）
    Char,      // i32 Unicode 码点
    String,
    Array,
    Func,
    Class,
    Interface,
    TypeParam,
    Wildcard,  // 类型实参槽中的 ? / ? extends T / ? super T（非可空后缀）
};

// 通配方差（仅 TypeKind::Wildcard）
enum class WildcardVariance { Unbounded, Extends, Super };

class Type;
using TypePtr = std::shared_ptr<Type>;

class Type {
public:
    TypeKind kind = TypeKind::Unknown;
    bool nullable = false;
    TypePtr elem;   // Array 元素；Func 返回；Wildcard 的 extends/super 上界
    std::vector<TypePtr> params;
    std::string className;
    std::vector<TypePtr> typeArgs;
    WildcardVariance wildVar = WildcardVariance::Unbounded;

    Type() = default;
    explicit Type(TypeKind k) : kind(k) {}

    static TypePtr makeUnit()    { return std::make_shared<Type>(TypeKind::Unit); }
    static TypePtr makeSByte()   { return std::make_shared<Type>(TypeKind::SByte); }
    static TypePtr makeByte()    { return std::make_shared<Type>(TypeKind::Byte); }
    static TypePtr makeShort()   { return std::make_shared<Type>(TypeKind::Short); }
    static TypePtr makeUShort()  { return std::make_shared<Type>(TypeKind::UShort); }
    static TypePtr makeInt()     { return std::make_shared<Type>(TypeKind::Int); }
    static TypePtr makeUInt()    { return std::make_shared<Type>(TypeKind::UInt); }
    static TypePtr makeLong()    { return std::make_shared<Type>(TypeKind::Long); }
    static TypePtr makeULong()   { return std::make_shared<Type>(TypeKind::ULong); }
    static TypePtr makeUIntPtr() { return std::make_shared<Type>(TypeKind::UIntPtr); }
    static TypePtr makeFloat()   { return std::make_shared<Type>(TypeKind::Float); }
    static TypePtr makeDouble()  { return std::make_shared<Type>(TypeKind::Double); }
    static TypePtr makeBool()    { return std::make_shared<Type>(TypeKind::Bool); }
    static TypePtr makeChar()    { return std::make_shared<Type>(TypeKind::Char); }
    static TypePtr makeString()  { return std::make_shared<Type>(TypeKind::String); }
    static TypePtr makeUnknown() { return std::make_shared<Type>(TypeKind::Unknown); }

    static TypePtr makeArray(TypePtr e) {
        auto t = std::make_shared<Type>(TypeKind::Array);
        t->elem = std::move(e);
        return t;
    }
    static TypePtr makeClass(std::string name) {
        auto t = std::make_shared<Type>(TypeKind::Class);
        t->className = std::move(name);
        return t;
    }
    static TypePtr makeClass(std::string name, std::vector<TypePtr> args) {
        auto t = std::make_shared<Type>(TypeKind::Class);
        t->className = std::move(name);
        t->typeArgs = std::move(args);
        return t;
    }
    static TypePtr makeInterface(std::string name) {
        auto t = std::make_shared<Type>(TypeKind::Interface);
        t->className = std::move(name);
        return t;
    }
    static TypePtr makeInterface(std::string name, std::vector<TypePtr> args) {
        auto t = std::make_shared<Type>(TypeKind::Interface);
        t->className = std::move(name);
        t->typeArgs = std::move(args);
        return t;
    }
    static TypePtr makeTypeParam(std::string name) {
        auto t = std::make_shared<Type>(TypeKind::TypeParam);
        t->className = std::move(name);
        return t;
    }
    static TypePtr makeWildcard() {
        return std::make_shared<Type>(TypeKind::Wildcard);
    }
    static TypePtr makeWildcardExtends(TypePtr bound) {
        auto t = std::make_shared<Type>(TypeKind::Wildcard);
        t->wildVar = WildcardVariance::Extends;
        t->elem = std::move(bound);
        return t;
    }
    static TypePtr makeWildcardSuper(TypePtr bound) {
        auto t = std::make_shared<Type>(TypeKind::Wildcard);
        t->wildVar = WildcardVariance::Super;
        t->elem = std::move(bound);
        return t;
    }
    static TypePtr makeNull() {
        auto t = std::make_shared<Type>(TypeKind::Class);
        t->className = "";
        t->nullable = true;
        return t;
    }
    static TypePtr makeFunc(std::vector<TypePtr> ps, TypePtr ret) {
        auto t = std::make_shared<Type>(TypeKind::Func);
        t->params = std::move(ps);
        t->elem = std::move(ret);
        return t;
    }

    bool isNull() const {
        return kind == TypeKind::Class && className.empty() && nullable;
    }
    bool isReferenceType() const {
        return kind == TypeKind::String || kind == TypeKind::Class ||
               kind == TypeKind::Interface || kind == TypeKind::Array ||
               kind == TypeKind::Func || kind == TypeKind::TypeParam;
    }

    // 类型实参中是否含不定泛型 ?
    bool hasWildcardArg() const {
        if (kind == TypeKind::Wildcard) return true;
        if (elem && elem->hasWildcardArg()) return true;
        for (const auto& a : typeArgs) if (a->hasWildcardArg()) return true;
        for (const auto& p : params)   if (p->hasWildcardArg()) return true;
        return false;
    }

    // is/as 查 typeids 用：全具体实参 → mono；裸/含 ? → 模板名（类型族）
    std::string typeIdKey() const {
        if (kind != TypeKind::Class && kind != TypeKind::Interface)
            return className;
        if (typeArgs.empty() || hasWildcardArg()) return className;
        return monoName();
    }

    // ------------------------------------------------------------
    //  语言可见类型属性（docs/类型属性.md）
    //  IsValueType ⟂ IsReference；值类型 T? 仍是值（语义），但 GcManaged。
    // ------------------------------------------------------------

    /** IsValueType：复制即独立；含值类型可空（Int? 等）；与 isReferenceType 互斥。 */
    bool isValueType() const {
        if (isReferenceType()) return false;
        if (kind == TypeKind::Unit || kind == TypeKind::Unknown) return false;
        return isInteger() || isFloating() || kind == TypeKind::Bool;
    }

    /** GcManaged：语言值是指向 GC 堆对象的指针（引用类型或值类型装箱块）。 */
    bool isGcManaged() const {
        return isReferenceType() || isBoxedNullable();
    }

    /**
     * IsUnsafePointer：原生裸指针，不参与 GC。
     * 当前无对应 TypeKind，恒 false（不对用户开放）。
     * 资源路径正式方案：lang.NativeHandle（isGcManaged；内部才持裸针；v0.55.61）。
     * 禁止用 UIntPtr/Long?/extern ptr 冒充资源句柄。
     */
    bool isUnsafePointer() const { return false; }

    // 整数族（含无符号、UIntPtr、Char 码点）
    bool isInteger() const {
        switch (kind) {
            case TypeKind::SByte: case TypeKind::Byte:
            case TypeKind::Short: case TypeKind::UShort:
            case TypeKind::Int:   case TypeKind::UInt:
            case TypeKind::Long:  case TypeKind::ULong:
            case TypeKind::UIntPtr:
            case TypeKind::Char:
                return true;
            default: return false;
        }
    }

    bool isUnsigned() const {
        switch (kind) {
            case TypeKind::Byte: case TypeKind::UShort:
            case TypeKind::UInt: case TypeKind::ULong:
            case TypeKind::UIntPtr:
                return true;
            default: return false;
        }
    }

    bool isSignedInt() const {
        return isInteger() && !isUnsigned();
    }

    bool isFloating() const {
        return kind == TypeKind::Float || kind == TypeKind::Double;
    }

    bool isBoxedNullable() const {
        return nullable && (isInteger() || isFloating() || kind == TypeKind::Bool);
    }

    bool isNumeric() const { return isInteger() || isFloating(); }
    bool isUnknown() const { return kind == TypeKind::Unknown; }
    bool isUnit()    const { return kind == TypeKind::Unit; }

    // 位宽字节数（整数/浮点）；非数值返回 0
    static int bitWidthBits(TypeKind k) {
        switch (k) {
            case TypeKind::SByte: case TypeKind::Byte:   return 8;
            case TypeKind::Short: case TypeKind::UShort: return 16;
            case TypeKind::Int:   case TypeKind::UInt:
            case TypeKind::Char:                        return 32;
            case TypeKind::Long:  case TypeKind::ULong:
            case TypeKind::UIntPtr:                     return 64;
            case TypeKind::Float:                        return 32;
            case TypeKind::Double:                       return 64;
            default: return 0;
        }
    }
    int bitWidthBits() const { return bitWidthBits(kind); }

    // 数值提升序（越大越宽）：同宽有符号/无符号同 rank，混合时另判
    // SByte/Byte=1 Short/UShort=2 Int/UInt/Char=3 Long/ULong/UIntPtr=4 Float=5 Double=6
    static int numericRank(TypeKind k) {
        switch (k) {
            case TypeKind::SByte: case TypeKind::Byte:   return 1;
            case TypeKind::Short: case TypeKind::UShort: return 2;
            case TypeKind::Int:   case TypeKind::UInt:
            case TypeKind::Char:                        return 3;
            case TypeKind::Long:  case TypeKind::ULong:
            case TypeKind::UIntPtr:                     return 4;
            case TypeKind::Float:                        return 5;
            case TypeKind::Double:                       return 6;
            default: return 0;
        }
    }
    int numericRank() const { return numericRank(kind); }

    // 按位宽取有符号/无符号 kind
    static TypeKind signedOfWidth(int bits) {
        if (bits <= 8)  return TypeKind::SByte;
        if (bits <= 16) return TypeKind::Short;
        if (bits <= 32) return TypeKind::Int;
        return TypeKind::Long;
    }
    static TypeKind unsignedOfWidth(int bits) {
        if (bits <= 8)  return TypeKind::Byte;
        if (bits <= 16) return TypeKind::UShort;
        if (bits <= 32) return TypeKind::UInt;
        return TypeKind::ULong;
    }
    static TypePtr makeOfKind(TypeKind k) {
        return std::make_shared<Type>(k);
    }

    // 两边都已 64 位且符号性冲突 → 禁止隐式混合（须显式转换）
    static bool isMixedSignedUnsigned64(TypeKind a, TypeKind b) {
        if (!Type(a).isInteger() || !Type(b).isInteger()) return false;
        if (bitWidthBits(a) != 64 || bitWidthBits(b) != 64) return false;
        return Type(a).isUnsigned() != Type(b).isUnsigned();
    }

    // 二元算术提升：
    // 有 Double→Double；有 Float→Float；
    // 整数：取较宽；同宽有符号+无符号→更宽有符号；
    // 两边都已 64 且符号性冲突 → nullptr（调用方报错，禁止收成 Long）
    // 否则至少 Int（窄类型升 32）
    static TypePtr binaryNumericPromote(TypeKind a, TypeKind b) {
        int ra = numericRank(a), rb = numericRank(b);
        if (ra == 0 || rb == 0) return nullptr;
        if (ra >= 6 || rb >= 6) return makeDouble();
        if (ra >= 5 || rb >= 5) return makeFloat();

        int wa = bitWidthBits(a), wb = bitWidthBits(b);
        int w = wa > wb ? wa : wb;
        if (w < 32) w = 32;

        bool ua = (a == TypeKind::Byte || a == TypeKind::UShort ||
                   a == TypeKind::UInt || a == TypeKind::ULong ||
                   a == TypeKind::UIntPtr);
        bool ub = (b == TypeKind::Byte || b == TypeKind::UShort ||
                   b == TypeKind::UInt || b == TypeKind::ULong ||
                   b == TypeKind::UIntPtr);

        if (wa == wb && ua != ub) {
            // 同宽混合 → 更宽有符号；已是 64 → 拒绝（无更宽有符号）
            if (w >= 64) return nullptr;
            return makeOfKind(signedOfWidth(w * 2));
        }
        if (ua && ub) return makeOfKind(unsignedOfWidth(w));
        return makeOfKind(signedOfWidth(w));
    }

    // 位运算提升：仅整数；有 64→64；否则至少 32；符号性同上
    static TypePtr binaryBitwisePromote(TypeKind a, TypeKind b) {
        if (!Type(a).isInteger() || !Type(b).isInteger()) return nullptr;
        int wa = bitWidthBits(a), wb = bitWidthBits(b);
        int w = wa > wb ? wa : wb;
        if (w < 32) w = 32;

        bool ua = Type(a).isUnsigned();
        bool ub = Type(b).isUnsigned();
        if (wa == wb && ua != ub) {
            if (w >= 64) return nullptr;
            return makeOfKind(signedOfWidth(w * 2));
        }
        if (ua && ub) return makeOfKind(unsignedOfWidth(w));
        return makeOfKind(signedOfWidth(w));
    }

    // 移位一元提升（对齐 Java：结果类型=左操作数提升后类型；移位量不参与二元混合）
    static TypePtr unaryBitwisePromote(TypeKind a) {
        if (!Type(a).isInteger()) return nullptr;
        int w = bitWidthBits(a);
        if (w < 32) w = 32;
        if (Type(a).isUnsigned()) return makeOfKind(unsignedOfWidth(w));
        return makeOfKind(signedOfWidth(w));
    }

    // 移位量掩码位数：32 位族→5，64 位族→6
    static int shiftMaskBits(TypeKind k) {
        return bitWidthBits(k) >= 64 ? 6 : 5;
    }

    int64_t arrayElemSize() const {
        // 可空装箱 / 引用类型元素均为指针宽（8）；勿按裸 Int/Bool 位宽取 esz
        if (llvmType() == "ptr") return 8;
        switch (kind) {
            case TypeKind::SByte: case TypeKind::Byte:
            case TypeKind::Bool:                         return 1;
            case TypeKind::Short: case TypeKind::UShort: return 2;
            case TypeKind::Int:   case TypeKind::UInt:
            case TypeKind::Char:
            case TypeKind::Float:                        return 4;
            default:                                     return 8;
        }
    }

    std::string arrayGepType() const {
        if (llvmType() == "ptr") return "ptr";
        switch (kind) {
            case TypeKind::SByte: case TypeKind::Byte:
            case TypeKind::Bool:                         return "i8";
            case TypeKind::Short: case TypeKind::UShort: return "i16";
            case TypeKind::Int:   case TypeKind::UInt:
            case TypeKind::Char:                        return "i32";
            case TypeKind::Float:                        return "float";
            case TypeKind::Long:  case TypeKind::ULong:
            case TypeKind::UIntPtr:                     return "i64";
            case TypeKind::Double:                       return "double";
            default:                                     return "i64";
        }
    }

    TypePtr asNullable() const {
        auto t = std::make_shared<Type>(*this);
        t->nullable = true;
        return t;
    }

    bool sameShape(const Type& o) const {
        if (kind != o.kind) return false;
        switch (kind) {
            case TypeKind::Array:
                return elem && o.elem && elem->sameShape(*o.elem);
            case TypeKind::Class:
            case TypeKind::Interface: {
                if (className != o.className) return false;
                if (typeArgs.size() != o.typeArgs.size()) return false;
                for (size_t i = 0; i < typeArgs.size(); ++i)
                    if (!typeArgs[i]->sameShape(*o.typeArgs[i])) return false;
                return true;
            }
            case TypeKind::TypeParam:
                return className == o.className;
            case TypeKind::Wildcard:
                if (wildVar != o.wildVar) return false;
                if (wildVar == WildcardVariance::Unbounded) return true;
                return elem && o.elem && elem->sameShape(*o.elem);
            case TypeKind::Func: {
                if (params.size() != o.params.size()) return false;
                for (size_t i = 0; i < params.size(); ++i)
                    if (!params[i]->sameShape(*o.params[i])) return false;
                return elem && o.elem && elem->sameShape(*o.elem);
            }
            default:
                return true;
        }
    }

    bool equals(const Type& o) const {
        return nullable == o.nullable && sameShape(o);
    }

    bool assignableTo(const Type& target) const {
        if (nullable && !target.nullable) return false;
        if (sameShape(target)) return true;
        // 双方都可空且形态不同：禁止静默拓宽（Int? 装箱 i32，Long? 装箱 i64）
        if (nullable && target.nullable) return false;

        int from = numericRank(kind);
        int to   = numericRank(target.kind);
        // 拓宽：较窄 → 较宽；同宽有符号/无符号互转允许（coerce 负责）
        if (from > 0 && to > 0 && from < to) return true;
        if (isInteger() && target.isInteger() && from == to) return true;
        if (isInteger() && target.isInteger() && from > to) return true;
        if (isFloating() && target.isFloating() && from > to) return true;
        // 浮点→整型：显式标注赋值允许（fptosi；对齐 Java/C# 截断转换）
        if (isFloating() && target.isInteger()) return true;
        return false;
    }

    static const char* kindDisplayName(TypeKind k) {
        switch (k) {
            case TypeKind::Unknown: return "?";
            case TypeKind::Unit:    return "Unit";
            case TypeKind::SByte:   return "SByte";
            case TypeKind::Byte:    return "Byte";
            case TypeKind::Short:   return "Short";
            case TypeKind::UShort:  return "UShort";
            case TypeKind::Int:     return "Int";
            case TypeKind::UInt:    return "UInt";
            case TypeKind::Long:    return "Long";
            case TypeKind::ULong:   return "ULong";
            case TypeKind::UIntPtr: return "UIntPtr";
            case TypeKind::Float:   return "Float";
            case TypeKind::Double:  return "Double";
            case TypeKind::Bool:    return "Bool";
            case TypeKind::Char:    return "Char";
            case TypeKind::String:  return "String";
            default: return "?";
        }
    }

    std::string toString() const {
        std::string base;
        switch (kind) {
            case TypeKind::Unknown: case TypeKind::Unit:
            case TypeKind::SByte: case TypeKind::Byte:
            case TypeKind::Short: case TypeKind::UShort:
            case TypeKind::Int: case TypeKind::UInt:
            case TypeKind::Long: case TypeKind::ULong:
            case TypeKind::UIntPtr:
            case TypeKind::Float: case TypeKind::Double:
            case TypeKind::Bool: case TypeKind::Char: case TypeKind::String:
                base = kindDisplayName(kind);
                break;
            case TypeKind::Class: case TypeKind::Interface:
            case TypeKind::TypeParam:
                base = className;
                break;
            case TypeKind::Wildcard:
                if (wildVar == WildcardVariance::Extends && elem)
                    base = "? extends " + elem->toString();
                else if (wildVar == WildcardVariance::Super && elem)
                    base = "? super " + elem->toString();
                else
                    base = "?";
                break;
            case TypeKind::Array:
                base = "[" + (elem ? elem->toString() : "?") + "]";
                break;
            case TypeKind::Func: {
                base = "(";
                for (size_t i = 0; i < params.size(); ++i) {
                    if (i) base += ", ";
                    base += params[i]->toString();
                }
                base += ") -> " + (elem ? elem->toString() : "?");
                break;
            }
        }
        if (!typeArgs.empty()) {
            base += "<";
            for (size_t i = 0; i < typeArgs.size(); ++i) {
                if (i) base += ", ";
                base += typeArgs[i]->toString();
            }
            base += ">";
        }
        return nullable ? base + "?" : base;
    }

    std::string llvmType() const {
        if (isBoxedNullable()) return "ptr";
        switch (kind) {
            case TypeKind::Unit:    return "void";
            case TypeKind::SByte: case TypeKind::Byte:   return "i8";
            case TypeKind::Short: case TypeKind::UShort: return "i16";
            case TypeKind::Int:   case TypeKind::UInt:
            case TypeKind::Char:                        return "i32";
            case TypeKind::Long:  case TypeKind::ULong:
            case TypeKind::UIntPtr:                     return "i64";
            case TypeKind::Float:                        return "float";
            case TypeKind::Double:                       return "double";
            case TypeKind::Bool:                         return "i8";
            case TypeKind::String: case TypeKind::Array:
            case TypeKind::Class: case TypeKind::Interface:
            case TypeKind::Func: case TypeKind::TypeParam:
            case TypeKind::Wildcard:
                return "ptr";
            case TypeKind::Unknown: return "i64";
        }
        return "i64";
    }

    std::string monoName() const {
        switch (kind) {
            case TypeKind::SByte: case TypeKind::Byte:
            case TypeKind::Short: case TypeKind::UShort:
            case TypeKind::Int: case TypeKind::UInt:
            case TypeKind::Long: case TypeKind::ULong:
            case TypeKind::UIntPtr:
            case TypeKind::Float: case TypeKind::Double:
            case TypeKind::Bool: case TypeKind::Char: case TypeKind::String:
            case TypeKind::Unit: case TypeKind::Unknown:
                return kindDisplayName(kind);
            case TypeKind::Wildcard:
                return "_";
            default: break;
        }
        std::string n = className;
        for (const auto& a : typeArgs) {
            n += "$";
            if (a->kind == TypeKind::Wildcard) {
                n += "_";
            } else if (a->isInteger() || a->isFloating() ||
                a->kind == TypeKind::Bool || a->kind == TypeKind::Char ||
                a->kind == TypeKind::String || a->kind == TypeKind::Unit) {
                n += kindDisplayName(a->kind);
            } else if (a->kind == TypeKind::Array) {
                n += "Arr" + (a->elem ? a->elem->monoName() : std::string("?"));
            } else {
                n += a->monoName();
            }
        }
        return n;
    }

    bool hasTypeParam() const {
        if (kind == TypeKind::TypeParam) return true;
        if (elem && elem->hasTypeParam()) return true;
        for (const auto& a : typeArgs) if (a->hasTypeParam()) return true;
        for (const auto& p : params)   if (p->hasTypeParam()) return true;
        return false;
    }
};

// 主名 + 宽度别名 + 小写别名 → 同一 TypeKind
inline TypePtr typeFromName(const std::string& name) {
    // PascalCase 主名
    if (name == "SByte")   return Type::makeSByte();
    if (name == "Byte")    return Type::makeByte();
    if (name == "Short")   return Type::makeShort();
    if (name == "UShort")  return Type::makeUShort();
    if (name == "Int")     return Type::makeInt();
    if (name == "UInt")    return Type::makeUInt();
    if (name == "Long")    return Type::makeLong();
    if (name == "ULong")   return Type::makeULong();
    if (name == "UIntPtr") return Type::makeUIntPtr();
    if (name == "Float")   return Type::makeFloat();
    if (name == "Double")  return Type::makeDouble();
    if (name == "Bool")    return Type::makeBool();
    if (name == "Char")    return Type::makeChar();
    if (name == "String")  return Type::makeString();
    if (name == "Unit")    return Type::makeUnit();
    // 宽度别名
    if (name == "i8")      return Type::makeSByte();
    if (name == "u8")      return Type::makeByte();
    if (name == "i16")     return Type::makeShort();
    if (name == "u16")     return Type::makeUShort();
    if (name == "i32")     return Type::makeInt();
    if (name == "u32")     return Type::makeUInt();
    if (name == "i64")     return Type::makeLong();
    if (name == "u64")     return Type::makeULong();
    if (name == "f32")     return Type::makeFloat();
    if (name == "f64")     return Type::makeDouble();
    if (name == "uintptr") return Type::makeUIntPtr();
    // 小写别名
    if (name == "sbyte")   return Type::makeSByte();
    if (name == "byte")    return Type::makeByte();
    if (name == "short")   return Type::makeShort();
    if (name == "ushort")  return Type::makeUShort();
    if (name == "int")     return Type::makeInt();
    if (name == "uint")    return Type::makeUInt();
    if (name == "long")    return Type::makeLong();
    if (name == "ulong")   return Type::makeULong();
    if (name == "float")   return Type::makeFloat();
    if (name == "double")  return Type::makeDouble();
    if (name == "bool")    return Type::makeBool();
    if (name == "char")    return Type::makeChar();
    if (name == "string")  return Type::makeString();
    return nullptr; // 非内建：由调用方解析为类/接口
}

using TypeSubst = std::map<std::string, TypePtr>;

inline TypePtr substType(const TypePtr& t, const TypeSubst& m) {
    if (!t) return t;
    if (t->kind == TypeKind::TypeParam) {
        auto it = m.find(t->className);
        if (it == m.end()) return t;
        if (t->nullable && !it->second->nullable)
            return it->second->asNullable();
        return it->second;
    }
    if (!t->hasTypeParam()) return t;
    auto r = std::make_shared<Type>(*t);
    if (t->elem) r->elem = substType(t->elem, m);
    for (auto& a : r->typeArgs) a = substType(a, m);
    for (auto& p : r->params)   p = substType(p, m);
    return r;
}

} // namespace hao
