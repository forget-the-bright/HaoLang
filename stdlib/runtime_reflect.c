/*
 * HaoLang 运行时 —— 反射（v0.19.0）
 * ------------------------------------------------------------
 *  每个类由编译器生成一份 HaoClassMeta 描述（类名/父类/接口/字段/
 *  方法/注解 + 该类的虚表指针），并注册进全局 hao_all_metas 数组。
 *  由于对象槽位 0 存虚表指针、且每个类的虚表是唯一全局常量，运行时
 *  从对象反查虚表即可定位其所属类的元数据。
 *
 *  字段值操作：字段元数据含槽位偏移，按类型（Int/Double/Bool/String）
 *  读写对象槽位。值以字符串形式跨反射边界传递（get 转字符串、set
 *  解析字符串），对基本类型与 String 字段完全可用。
 */
#include "runtime_internal.h"

/* 字段元数据（全部字段用 8 字节，避免 C/LLVM 结构体对齐差异） */
typedef struct {
    const char* name;
    const char* typeStr;   /* "Int"/"Double"/"Bool"/"String"/类型名 */
    int64_t slot;          /* 对象槽位偏移 */
    int64_t isStatic;
    int64_t isMutable;
    const char* visibility;
} HaoFieldMeta;

/* 注解元数据（v0.29：name + 编码后的参数串） */
typedef struct {
    const char* name;
    const char* value; /* 单值="/path"；多值 "path=/x;method=GET" */
} HaoAnnoMeta;

/* 方法元数据 */
typedef struct {
    const char* name;
    const char* retType;
    const char* const* paramTypes;
    int64_t paramCount;
    const char* visibility;
    int64_t isStatic;
    void* invoke;  /* 反射 invoke thunk（v0.20.0）：int64_t(*)(int64_t obj, void* argSlots) */
    const HaoAnnoMeta* annos; /* v0.29：方法级注解 */
    int64_t annoCount;
} HaoMethodMeta;

/* 类元数据 */
typedef struct {
    const char* name;
    const char* superName;
    const char* const* ifaces;
    int64_t ifaceCount;
    const HaoFieldMeta* fields;
    int64_t fieldCount;
    const HaoMethodMeta* methods;
    int64_t methodCount;
    const HaoAnnoMeta* annos;
    int64_t annoCount;
    void* vtablePtr;      /* 该类的虚表指针，用于从对象反查 */
    int64_t isAbstract;
    int64_t isEnum;
    void* factory;        /* v0.31：ptr (*)(void* argslots)，0～N 参；不可实例化则为 NULL */
    int64_t ctorParamCount;
    const char* const* ctorParamTypes;
} HaoClassMeta;

typedef void* (*HaoFactoryFn)(void*);

/* 全局元数据注册表（编译器生成的各 <cls>_meta 指针，NULL 结尾） */
extern HaoClassMeta* hao_all_metas[];
static const HaoClassMeta** g_metas = (const HaoClassMeta**)hao_all_metas;

/* 从对象反查类元数据：读槽位 0 的虚表指针，线性查找 */
void* hao_reflect_getclass(void* obj) {
    if (!obj) return NULL;
    void* vt = *(void**)obj;
    for (const HaoClassMeta** m = g_metas; m && *m; ++m)
        if ((*m)->vtablePtr == vt) return (void*)*m;
    return NULL;
}

/* ---- 全局类型表 / 无参实例化（v0.30 包扫描）---- */
int32_t hao_reflect_type_count(void) {
    int32_t n = 0;
    for (const HaoClassMeta** m = g_metas; m && *m; ++m) n++;
    return n;
}

void* hao_reflect_type_at(int32_t i) {
    if (i < 0) return NULL;
    int32_t n = 0;
    for (const HaoClassMeta** m = g_metas; m && *m; ++m) {
        if (n == i) return (void*)*m;
        n++;
    }
    return NULL;
}

void* hao_reflect_new_instance(void* meta) {
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    if (!m || !m->factory) return NULL;
    if (m->ctorParamCount != 0) return NULL; /* 有参须走 new_instance_args */
    return ((HaoFactoryFn)m->factory)(NULL);
}

void* hao_reflect_new_instance_args(void* meta, int64_t* argSlots) {
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    if (!m || !m->factory) return NULL;
    if (m->ctorParamCount == 0)
        return ((HaoFactoryFn)m->factory)(NULL);
    if (!argSlots) return NULL;
    if (hao_array_len(argSlots) != m->ctorParamCount) return NULL;
    return ((HaoFactoryFn)m->factory)(argSlots);
}

int32_t hao_reflect_has_factory(void* meta) {
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    return (m && m->factory) ? 1 : 0;
}

int32_t hao_reflect_ctor_param_count(void* meta) {
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    return m ? (int32_t)m->ctorParamCount : 0;
}

HaoString* hao_reflect_ctor_param_type(void* meta, int32_t i) {
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    if (!m || i < 0 || i >= m->ctorParamCount || !m->ctorParamTypes)
        return hao_str_from_cstr("");
    return hao_str_from_cstr(m->ctorParamTypes[i] ? m->ctorParamTypes[i] : "");
}

/* 按名字在类元数据中找字段 */
static const HaoFieldMeta* find_field(const HaoClassMeta* m, const char* name) {
    if (!m || !name) return NULL;
    for (int i = 0; i < m->fieldCount; ++i)
        if (strcmp(m->fields[i].name, name) == 0) return &m->fields[i];
    return NULL;
}

/* ---- 元数据字符串访问器（供 reflect.hao 调用；返回 HaoString*）---- */
HaoString* hao_reflect_name(void* meta) {
    return hao_str_from_cstr(meta ? ((const HaoClassMeta*)meta)->name : "");
}
HaoString* hao_reflect_super(void* meta) {
    return hao_str_from_cstr(meta ? ((const HaoClassMeta*)meta)->superName : "");
}
int32_t hao_reflect_is_abstract(void* meta) {
    return meta ? ((const HaoClassMeta*)meta)->isAbstract : 0;
}
int32_t hao_reflect_is_enum(void* meta) {
    return meta ? ((const HaoClassMeta*)meta)->isEnum : 0;
}
int32_t hao_reflect_iface_count(void* meta) {
    return meta ? ((const HaoClassMeta*)meta)->ifaceCount : 0;
}
HaoString* hao_reflect_iface_at(void* meta, int32_t i) {
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    if (!m || i < 0 || i >= m->ifaceCount) return hao_str_from_cstr("");
    return hao_str_from_cstr(m->ifaces[i] ? m->ifaces[i] : "");
}
int32_t hao_reflect_field_count(void* meta) {
    return meta ? ((const HaoClassMeta*)meta)->fieldCount : 0;
}
HaoString* hao_reflect_field_name(void* meta, int32_t i) {
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    if (!m || i < 0 || i >= m->fieldCount) return hao_str_from_cstr("");
    return hao_str_from_cstr(m->fields[i].name ? m->fields[i].name : "");
}
HaoString* hao_reflect_field_type(void* meta, int32_t i) {
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    if (!m || i < 0 || i >= m->fieldCount) return hao_str_from_cstr("");
    return hao_str_from_cstr(m->fields[i].typeStr ? m->fields[i].typeStr : "");
}
int32_t hao_reflect_method_count(void* meta) {
    return meta ? ((const HaoClassMeta*)meta)->methodCount : 0;
}
HaoString* hao_reflect_method_name(void* meta, int32_t i) {
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    if (!m || i < 0 || i >= m->methodCount) return hao_str_from_cstr("");
    return hao_str_from_cstr(m->methods[i].name ? m->methods[i].name : "");
}
int32_t hao_reflect_anno_count(void* meta) {
    return meta ? ((const HaoClassMeta*)meta)->annoCount : 0;
}
HaoString* hao_reflect_anno_name(void* meta, int32_t i) {
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    if (!m || i < 0 || i >= m->annoCount) return hao_str_from_cstr("");
    return hao_str_from_cstr(m->annos[i].name ? m->annos[i].name : "");
}
HaoString* hao_reflect_anno_value(void* meta, int32_t i) {
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    if (!m || i < 0 || i >= m->annoCount) return hao_str_from_cstr("");
    return hao_str_from_cstr(m->annos[i].value ? m->annos[i].value : "");
}
int32_t hao_reflect_method_anno_count(void* meta, int32_t mi) {
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    if (!m || mi < 0 || mi >= m->methodCount) return 0;
    return (int32_t)m->methods[mi].annoCount;
}
HaoString* hao_reflect_method_anno_name(void* meta, int32_t mi, int32_t ai) {
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    if (!m || mi < 0 || mi >= m->methodCount) return hao_str_from_cstr("");
    const HaoMethodMeta* mm = &m->methods[mi];
    if (ai < 0 || ai >= mm->annoCount || !mm->annos)
        return hao_str_from_cstr("");
    return hao_str_from_cstr(mm->annos[ai].name ? mm->annos[ai].name : "");
}
HaoString* hao_reflect_method_anno_value(void* meta, int32_t mi, int32_t ai) {
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    if (!m || mi < 0 || mi >= m->methodCount) return hao_str_from_cstr("");
    const HaoMethodMeta* mm = &m->methods[mi];
    if (ai < 0 || ai >= mm->annoCount || !mm->annos)
        return hao_str_from_cstr("");
    return hao_str_from_cstr(mm->annos[ai].value ? mm->annos[ai].value : "");
}

/* ---- 字段值读写（字符串化）---- */
static const char* field_type(const HaoClassMeta* m, const char* fname) {
    const HaoFieldMeta* f = find_field(m, fname);
    return f ? f->typeStr : "";
}

/* 读字段值转字符串。obj 为对象实例。 */
HaoString* hao_reflect_field_get(void* meta, void* obj, HaoString* fname) {
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    const char* name = hao_str_cstr(fname);
    const HaoFieldMeta* f = find_field(m, name);
    if (!f || !obj) return NULL;
    char* base = (char*)obj + (size_t)f->slot * 8;
    char buf[64];
    const char* t = f->typeStr;
    if (t && strcmp(t, "Int") == 0) {
        snprintf(buf, sizeof buf, "%d", (int)(*(int32_t*)base));
    } else if (t && strcmp(t, "Long") == 0) {
        snprintf(buf, sizeof buf, "%lld", (long long)(*(int64_t*)base));
    } else if (t && strcmp(t, "UInt") == 0) {
        snprintf(buf, sizeof buf, "%u", (unsigned)(*(uint32_t*)base));
    } else if (t && strcmp(t, "ULong") == 0) {
        snprintf(buf, sizeof buf, "%llu", (unsigned long long)(*(uint64_t*)base));
    } else if (t && strcmp(t, "UIntPtr") == 0) {
        snprintf(buf, sizeof buf, "%llu", (unsigned long long)(*(uint64_t*)base));
    } else if (t && strcmp(t, "Short") == 0) {
        snprintf(buf, sizeof buf, "%d", (int)(*(int16_t*)base));
    } else if (t && strcmp(t, "UShort") == 0) {
        snprintf(buf, sizeof buf, "%u", (unsigned)(*(uint16_t*)base));
    } else if (t && strcmp(t, "Byte") == 0) {
        snprintf(buf, sizeof buf, "%u", (unsigned)(*(uint8_t*)base));
    } else if (t && strcmp(t, "SByte") == 0) {
        snprintf(buf, sizeof buf, "%d", (int)(*(int8_t*)base));
    } else if (t && strcmp(t, "Float") == 0) {
        snprintf(buf, sizeof buf, "%g", (double)(*(float*)base));
    } else if (t && strcmp(t, "Double") == 0) {
        snprintf(buf, sizeof buf, "%g", *(double*)base);
    } else if (t && strcmp(t, "Bool") == 0) {
        strcpy(buf, (*(int8_t*)base) ? "true" : "false");
    } else if (t && strcmp(t, "Char") == 0) {
        snprintf(buf, sizeof buf, "%d", (int)(*(int32_t*)base));
    } else if (t && strcmp(t, "String") == 0) {
        HaoString* s = *(HaoString**)base;
        return s;
    } else {
        snprintf(buf, sizeof buf, "<0x%016llx>",
                 (unsigned long long)(uintptr_t)(*(void**)base));
    }
    return hao_str_from_cstr(buf);
}

/* 写字段值（严格解析，与 Integer.parse 等同策略）。返回 0 成功、非 0 失败。 */
int32_t hao_reflect_field_set(void* meta, void* obj, HaoString* fname,
                              HaoString* value) {
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    const char* name = hao_str_cstr(fname);
    const HaoFieldMeta* f = find_field(m, name);
    if (!f || !obj || !value) return 1;
    char* base = (char*)obj + (size_t)f->slot * 8;
    const char* t = f->typeStr;
    const char* v = hao_str_cstr(value);
    if (!v) return 1;
    if (t && strcmp(t, "Int") == 0) {
        void* box = hao_parse_int(value);
        if (!box) return 1;
        *(int32_t*)base = hao_unbox_i32(box);
    } else if (t && strcmp(t, "Long") == 0) {
        void* box = hao_parse_long(value);
        if (!box) return 1;
        *(int64_t*)base = hao_unbox_i64(box);
    } else if (t && strcmp(t, "UInt") == 0) {
        void* box = hao_parse_uint(value);
        if (!box) return 1;
        *(uint32_t*)base = (uint32_t)hao_unbox_i32(box);
    } else if (t && strcmp(t, "ULong") == 0) {
        void* box = hao_parse_ulong(value);
        if (!box) return 1;
        *(uint64_t*)base = (uint64_t)hao_unbox_i64(box);
    } else if (t && strcmp(t, "UIntPtr") == 0) {
        void* box = hao_parse_ulong(value);
        if (!box) return 1;
        *(uint64_t*)base = (uint64_t)hao_unbox_i64(box);
    } else if (t && strcmp(t, "Short") == 0) {
        void* box = hao_parse_int(value);
        if (!box) return 1;
        int32_t n = hao_unbox_i32(box);
        if (n < INT16_MIN || n > INT16_MAX) return 1;
        *(int16_t*)base = (int16_t)n;
    } else if (t && strcmp(t, "UShort") == 0) {
        void* box = hao_parse_int(value);
        if (!box) return 1;
        int32_t n = hao_unbox_i32(box);
        if (n < 0 || n > 65535) return 1;
        *(uint16_t*)base = (uint16_t)n;
    } else if (t && strcmp(t, "Byte") == 0) {
        void* box = hao_parse_int(value);
        if (!box) return 1;
        int32_t n = hao_unbox_i32(box);
        if (n < 0 || n > 255) return 1;
        *(uint8_t*)base = (uint8_t)n;
    } else if (t && strcmp(t, "SByte") == 0) {
        void* box = hao_parse_int(value);
        if (!box) return 1;
        int32_t n = hao_unbox_i32(box);
        if (n < INT8_MIN || n > INT8_MAX) return 1;
        *(int8_t*)base = (int8_t)n;
    } else if (t && strcmp(t, "Float") == 0) {
        void* box = hao_parse_float(value);
        if (!box) return 1;
        *(float*)base = hao_unbox_f32(box);
    } else if (t && strcmp(t, "Double") == 0) {
        void* box = hao_parse_double(value);
        if (!box) return 1;
        *(double*)base = hao_unbox_f64(box);
    } else if (t && strcmp(t, "Bool") == 0) {
        void* box = hao_parse_bool(value);
        if (box) {
            *(int8_t*)base = (int8_t)hao_unbox_i32(box);
        } else if (strcmp(v, "1") == 0) {
            *(int8_t*)base = 1;
        } else if (strcmp(v, "0") == 0) {
            *(int8_t*)base = 0;
        } else {
            return 1;
        }
    } else if (t && strcmp(t, "Char") == 0) {
        void* box = hao_parse_int(value);
        if (!box) return 1;
        *(int32_t*)base = hao_unbox_i32(box);
    } else if (t && strcmp(t, "String") == 0) {
        HaoString* ns = hao_str_from_cstr(v);
        *(HaoString**)base = ns;
        hao_gc_barrier(obj, ns);
    } else {
        return 1;
    }
    return 0;
}

/* ---- 反射方法调用 invoke（v0.20.0）----
 *  思路：值为统一 8 字节槽，编译器为每个方法生成一个 thunk
 *    int64_t thunk(int64_t obj, void* argSlots)
 *  把 [Int] 参数槽按方法真实签名编组调用（虚方法经虚表分派保持多态），
 *  返回 8 字节结果。此处按方法名（含父类链）找到 thunk 并调用。
 *  实参槽约定：Int=值、Bool=0或1、Double=位模式、String/对象=指针当 Int。
 *  用 hao_reflect_ptrtoint 等助手在 Int 与具体类型间转换。
 */

/* 按名字在类元数据（含父类链）中找方法 */
static const HaoMethodMeta* find_method(const HaoClassMeta* m, const char* name) {
    for (const HaoClassMeta* c = m; c; ) {
        for (int i = 0; i < c->methodCount; ++i)
            if (strcmp(c->methods[i].name, name) == 0) return &c->methods[i];
        if (!c->superName || !*c->superName) break;
        const HaoClassMeta* parent = NULL;
        for (const HaoClassMeta** g = g_metas; g && *g; ++g)
            if (strcmp((*g)->name, c->superName) == 0) { parent = *g; break; }
        c = parent;
    }
    return NULL;
}

typedef int64_t (*HaoInvokeFn)(int64_t, void*);

/* 按名查形参数量；未找到 -1（供 Hao 层抛 Exception） */
int32_t hao_reflect_method_param_count(void* meta, HaoString* name) {
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    if (!m || !name) return -1;
    const HaoMethodMeta* mm = find_method(m, hao_str_cstr(name));
    if (!mm || !mm->invoke) return -1;
    return (int32_t)mm->paramCount;
}

int64_t hao_reflect_invoke(void* meta, void* obj, HaoString* name, int64_t* argSlots) {
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    const HaoMethodMeta* mm = find_method(m, hao_str_cstr(name));
    if (!mm || !mm->invoke)
        hao_panic_msg("reflect: method not found");
    /* 含 0 参：长度必须精确匹配，防 OOB / 多余槽被忽略 */
    int64_t need = mm->paramCount;
    int64_t got = argSlots ? hao_array_len(argSlots) : 0;
    if (need > 0 && !argSlots)
        hao_panic_msg("reflect: null arg slots");
    if (got != need)
        hao_panic_msg("reflect: arity mismatch");
    HaoInvokeFn fn = (HaoInvokeFn)mm->invoke;
    return fn((int64_t)(intptr_t)obj, argSlots);
}
HaoString* hao_reflect_invoke_str(void* meta, void* obj, HaoString* name, int64_t* argSlots) {
    return (HaoString*)(intptr_t)hao_reflect_invoke(meta, obj, name, argSlots);
}
int8_t hao_reflect_invoke_bool(void* meta, void* obj, HaoString* name, int64_t* argSlots) {
    return (int8_t)hao_reflect_invoke(meta, obj, name, argSlots);
}
double hao_reflect_invoke_double(void* meta, void* obj, HaoString* name, int64_t* argSlots) {
    int64_t r = hao_reflect_invoke(meta, obj, name, argSlots);
    double d; memcpy(&d, &r, 8); return d;
}
float hao_reflect_invoke_float(void* meta, void* obj, HaoString* name, int64_t* argSlots) {
    int64_t r = hao_reflect_invoke(meta, obj, name, argSlots);
    int32_t bits = (int32_t)r;
    float f; memcpy(&f, &bits, 4); return f;
}
void hao_reflect_invoke_void(void* meta, void* obj, HaoString* name, int64_t* argSlots) {
    hao_reflect_invoke(meta, obj, name, argSlots);
}

/* ---- 反射槽位转换助手：Int ↔ 具体类型（供构造 [Int] 实参/解释结果）---- */
int64_t hao_reflect_ptrtoint(void* p) { return (int64_t)(intptr_t)p; }
void*   hao_reflect_inttoptr(int64_t i) { return (void*)(intptr_t)i; }
int64_t hao_reflect_dbl_bits(double d) { int64_t r; memcpy(&r, &d, 8); return r; }
double  hao_reflect_bits_dbl(int64_t i) { double r; memcpy(&r, &i, 8); return r; }
int64_t hao_reflect_flt_bits(float f) {
    int32_t bits; memcpy(&bits, &f, 4);
    return (int64_t)(uint32_t)bits;
}
float hao_reflect_bits_flt(int64_t i) {
    int32_t bits = (int32_t)i;
    float f; memcpy(&f, &bits, 4); return f;
}
int64_t hao_reflect_bool_val(int8_t b) { return b ? 1 : 0; }
int8_t  hao_reflect_val_bool(int64_t i) { return i ? 1 : 0; }