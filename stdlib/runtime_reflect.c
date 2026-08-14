/*
 * HaoLang 运行时 —— 反射（v0.19.0）
 * ------------------------------------------------------------
 *  每个类由编译器生成一份 HaoClassMeta 描述（类名/父类/接口/字段/
 *  方法/注解 + 该类的虚表指针），并注册进全局 hao_all_metas 数组。
 *  由于对象槽位 0 存虚表指针、且每个类的虚表是唯一全局常量，运行时
 *  从对象反查虚表即可定位其所属类的元数据。
 *
 *  字段值操作：字段元数据含槽位偏移，按类型读写对象槽位。
 *  兼容路径 getField(name) 仍转字符串；JSON 热路径用 field_get_at(index)
 *  （对标 Fastjson ObjectWriter / .NET JsonTypeInfo：按索引 + 缓存描述，禁按名线性查找）。
 */
#include "runtime_internal.h"
#include <errno.h>
#include <limits.h>

/* 字段元数据（全部字段用 8 字节，避免 C/LLVM 结构体对齐差异） */
typedef struct {
    const char* name;
    const char* typeStr;   /* "Int"/"Double"/"Bool"/"String"/类型名 */
    int64_t slot;          /* 对象槽位偏移 */
    int64_t isStatic;
    int64_t isMutable;
    const char* visibility;
} HaoFieldMeta;

/* 注解元数据（v0.29：name + 编码后的参数串；v0.50：+ 注解类型 meta） */
typedef struct {
    const char* name;
    const char* value; /* 单值="/path"；多值 "path=/x;method=GET" */
    void* classMeta;   /* @Anno.meta，供 Class 令牌比对 */
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
    int64_t isAnnotation; /* v0.50：@interface 注解类型 */
    void* classMirror;    /* v0.50.1：reflect.Class 单例（可变；meta 为 global） */
    void* jsonWrite;      /* v0.76：$jsonWrite(sb,self,features,indent,stack,depth)；无则 NULL */
} HaoClassMeta;

typedef void (*HaoJsonWriteFn)(void* sb, void* self, int32_t features, int32_t indent,
                               void* stack, int32_t depth);
typedef void* (*HaoFactoryFn)(void*);

/* 全局元数据注册表（编译器生成的各 <cls>_meta 指针，NULL 结尾） */
extern HaoClassMeta* hao_all_metas[];
static const HaoClassMeta** g_metas = (const HaoClassMeta**)hao_all_metas;

/* v0.56：对外 API 入参为 NativeHandle；内部解包永生 meta（drop 空） */
static void* meta_raw(HaoNativeHandle* h) {
    return hao_handle_raw(h);
}
static HaoNativeHandle* wrap_meta(void* meta) {
    return hao_handle_wrap(meta);
}



/* 从对象反查类元数据：读槽位 0 的虚表指针，线性查找 */
HaoNativeHandle* hao_reflect_getclass(void* obj) {
    if (!obj) return wrap_meta(NULL);
    /* 与 field_get 对齐：非堆/已回收 → 清晰 panic，避免错位 CRT AV 被 loc 读成神秘 UAF */
    if (!hao_gc_expect_heap_object(obj)) {
        char buf[96];
        int n = 0;
        const char* a = "reflect getclass 非对象/已回收 obj=";
        while (*a && n < (int)sizeof(buf) - 1) buf[n++] = *a++;
        n = n + hao_fmt_ptr_angle(buf + n, (int)sizeof(buf) - n, obj);
        if (n < (int)sizeof(buf)) buf[n] = '\0';
        hao_panic_msg(buf);
    }
    void* vt = *(void**)obj;
    for (const HaoClassMeta** m = g_metas; m && *m; ++m)
        if ((*m)->vtablePtr == vt) return wrap_meta((void*)*m);
    return wrap_meta(NULL);
}

/* ---- 全局类型表 / 无参实例化（v0.30 包扫描）---- */
int32_t hao_reflect_type_count(void) {
    int32_t n = 0;
    for (const HaoClassMeta** m = g_metas; m && *m; ++m) n++;
    return n;
}

HaoNativeHandle* hao_reflect_type_at(int32_t i) {
    if (i < 0) return wrap_meta(NULL);
    int32_t n = 0;
    for (const HaoClassMeta** m = g_metas; m && *m; ++m) {
        if (n == i) return wrap_meta((void*)*m);
        n++;
    }
    return wrap_meta(NULL);
}

void* hao_reflect_new_instance(HaoNativeHandle* h) {
    void* meta = meta_raw(h);
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    if (!m || !m->factory) return NULL;
    if (m->ctorParamCount != 0) return NULL; /* 有参须走 new_instance_args */
    return ((HaoFactoryFn)m->factory)(NULL);
}

void* hao_reflect_new_instance_args(HaoNativeHandle* h, int64_t* argSlots) {
    void* meta = meta_raw(h);
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    if (!m || !m->factory) return NULL;
    if (m->ctorParamCount == 0)
        return ((HaoFactoryFn)m->factory)(NULL);
    if (!argSlots) return NULL;
    if (hao_array_len(argSlots) != m->ctorParamCount) return NULL;
    return ((HaoFactoryFn)m->factory)(argSlots);
}

int32_t hao_reflect_has_factory(HaoNativeHandle* h) {
    void* meta = meta_raw(h);
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    return (m && m->factory) ? 1 : 0;
}

int32_t hao_reflect_ctor_param_count(HaoNativeHandle* h) {
    void* meta = meta_raw(h);
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    return m ? (int32_t)m->ctorParamCount : 0;
}

HaoString* hao_reflect_ctor_param_type(HaoNativeHandle* h, int32_t i) {
    void* meta = meta_raw(h);
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
HaoString* hao_reflect_name(HaoNativeHandle* h) {
    void* meta = meta_raw(h);
    return hao_str_from_cstr(meta ? ((const HaoClassMeta*)meta)->name : "");
}
HaoString* hao_reflect_super(HaoNativeHandle* h) {
    void* meta = meta_raw(h);
    return hao_str_from_cstr(meta ? ((const HaoClassMeta*)meta)->superName : "");
}
int32_t hao_reflect_is_abstract(HaoNativeHandle* h) {
    void* meta = meta_raw(h);
    return meta ? ((const HaoClassMeta*)meta)->isAbstract : 0;
}
int32_t hao_reflect_is_enum(HaoNativeHandle* h) {
    void* meta = meta_raw(h);
    return meta ? ((const HaoClassMeta*)meta)->isEnum : 0;
}
int32_t hao_reflect_iface_count(HaoNativeHandle* h) {
    void* meta = meta_raw(h);
    return meta ? ((const HaoClassMeta*)meta)->ifaceCount : 0;
}
HaoString* hao_reflect_iface_at(HaoNativeHandle* h, int32_t i) {
    void* meta = meta_raw(h);
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    if (!m || i < 0 || i >= m->ifaceCount) return hao_str_from_cstr("");
    return hao_str_from_cstr(m->ifaces[i] ? m->ifaces[i] : "");
}
int32_t hao_reflect_field_count(HaoNativeHandle* h) {
    void* meta = meta_raw(h);
    return meta ? ((const HaoClassMeta*)meta)->fieldCount : 0;
}
HaoString* hao_reflect_field_name(HaoNativeHandle* h, int32_t i) {
    void* meta = meta_raw(h);
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    if (!m || i < 0 || i >= m->fieldCount) return hao_str_from_cstr("");
    return hao_str_from_cstr(m->fields[i].name ? m->fields[i].name : "");
}
HaoString* hao_reflect_field_type(HaoNativeHandle* h, int32_t i) {
    void* meta = meta_raw(h);
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    if (!m || i < 0 || i >= m->fieldCount) return hao_str_from_cstr("");
    return hao_str_from_cstr(m->fields[i].typeStr ? m->fields[i].typeStr : "");
}
int32_t hao_reflect_method_count(HaoNativeHandle* h) {
    void* meta = meta_raw(h);
    return meta ? ((const HaoClassMeta*)meta)->methodCount : 0;
}
HaoString* hao_reflect_method_name(HaoNativeHandle* h, int32_t i) {
    void* meta = meta_raw(h);
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    if (!m || i < 0 || i >= m->methodCount) return hao_str_from_cstr("");
    return hao_str_from_cstr(m->methods[i].name ? m->methods[i].name : "");
}
int32_t hao_reflect_anno_count(HaoNativeHandle* h) {
    void* meta = meta_raw(h);
    return meta ? ((const HaoClassMeta*)meta)->annoCount : 0;
}
HaoString* hao_reflect_anno_name(HaoNativeHandle* h, int32_t i) {
    void* meta = meta_raw(h);
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    if (!m || i < 0 || i >= m->annoCount) return hao_str_from_cstr("");
    return hao_str_from_cstr(m->annos[i].name ? m->annos[i].name : "");
}
HaoString* hao_reflect_anno_value(HaoNativeHandle* h, int32_t i) {
    void* meta = meta_raw(h);
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    if (!m || i < 0 || i >= m->annoCount) return hao_str_from_cstr("");
    return hao_str_from_cstr(m->annos[i].value ? m->annos[i].value : "");
}
int32_t hao_reflect_method_anno_count(HaoNativeHandle* h, int32_t mi) {
    void* meta = meta_raw(h);
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    if (!m || mi < 0 || mi >= m->methodCount) return 0;
    return (int32_t)m->methods[mi].annoCount;
}
HaoString* hao_reflect_method_anno_name(HaoNativeHandle* h, int32_t mi, int32_t ai) {
    void* meta = meta_raw(h);
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    if (!m || mi < 0 || mi >= m->methodCount) return hao_str_from_cstr("");
    const HaoMethodMeta* mm = &m->methods[mi];
    if (ai < 0 || ai >= mm->annoCount || !mm->annos)
        return hao_str_from_cstr("");
    return hao_str_from_cstr(mm->annos[ai].name ? mm->annos[ai].name : "");
}
HaoString* hao_reflect_method_anno_value(HaoNativeHandle* h, int32_t mi, int32_t ai) {
    void* meta = meta_raw(h);
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    if (!m || mi < 0 || mi >= m->methodCount) return hao_str_from_cstr("");
    const HaoMethodMeta* mm = &m->methods[mi];
    if (ai < 0 || ai >= mm->annoCount || !mm->annos)
        return hao_str_from_cstr("");
    return hao_str_from_cstr(mm->annos[ai].value ? mm->annos[ai].value : "");
}

HaoNativeHandle* hao_reflect_anno_class(HaoNativeHandle* h, int32_t i) {
    void* meta = meta_raw(h);
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    if (!m || i < 0 || i >= m->annoCount || !m->annos) return wrap_meta(NULL);
    return wrap_meta(m->annos[i].classMeta);
}

HaoNativeHandle* hao_reflect_method_anno_class(HaoNativeHandle* h, int32_t mi, int32_t ai) {
    void* meta = meta_raw(h);
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    if (!m || mi < 0 || mi >= m->methodCount) return wrap_meta(NULL);
    const HaoMethodMeta* mm = &m->methods[mi];
    if (ai < 0 || ai >= mm->annoCount || !mm->annos) return wrap_meta(NULL);
    return wrap_meta(mm->annos[ai].classMeta);
}

int32_t hao_reflect_is_interface(HaoNativeHandle* h) {
    (void)h;
    /* 接口目前不进 hao_all_metas；Class 令牌仅覆盖 class/annotation/enum */
    return 0;
}

int32_t hao_reflect_is_annotation(HaoNativeHandle* h) {
    void* meta = meta_raw(h);
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    return (m && m->isAnnotation) ? 1 : 0;
}

int32_t hao_reflect_meta_hash(HaoNativeHandle* h) {
    void* meta = meta_raw(h);
    uintptr_t p = (uintptr_t)meta;
    return (int32_t)(p ^ (p >> 32));
}

/* v0.50.1：每类型一个 reflect.Class 单例（存于 meta.classMirror） */
void* hao_reflect_class_mirror(HaoNativeHandle* h) {
    void* meta = meta_raw(h);
    if (!meta) return NULL;
    HaoClassMeta* m = (HaoClassMeta*)meta;
    return __atomic_load_n(&m->classMirror, __ATOMIC_ACQUIRE);
}

/* 首次写入赢；失败则返回已有单例。成功时挂 GC 根。 */
void* hao_reflect_set_class_mirror(HaoNativeHandle* h, void* obj) {
    void* meta = meta_raw(h);
    if (!meta || !obj) return obj;
    HaoClassMeta* m = (HaoClassMeta*)meta;
    void* expected = NULL;
    if (__atomic_compare_exchange_n(&m->classMirror, &expected, obj, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        hao_gc_add_root(obj); /* Class 镜恒为堆对象；禁 is_heap_ptr 前置 */
        return obj;
    }
    return expected; /* 已有赢家 */
}

/* ---- 字段值读写（字符串化）---- */
static const char* field_type(const HaoClassMeta* m, const char* fname) {
    const HaoFieldMeta* f = find_field(m, fname);
    return f ? f->typeStr : "";
}

/* 类型名是否为 base 或 base?（字段 typeStr 可能带可空后缀） */
static int type_is_base(const char* t, const char* base) {
    if (!t || !base) return 0;
    size_t n = strlen(base);
    if (strncmp(t, base, n) != 0) return 0;
    return t[n] == '\0' || t[n] == '?';
}

/* 非空标量 / String|String?：不走 get_obj（用 field_get）。T? 数值走 get_obj。 */
static int is_scalar_field_type(const char* t) {
    if (!t || !*t) return 1;
    if (type_is_base(t, "String")) return 1;
    return strcmp(t, "Int") == 0 || strcmp(t, "Long") == 0
        || strcmp(t, "UInt") == 0 || strcmp(t, "ULong") == 0
        || strcmp(t, "UIntPtr") == 0 || strcmp(t, "Short") == 0
        || strcmp(t, "UShort") == 0 || strcmp(t, "Byte") == 0
        || strcmp(t, "SByte") == 0 || strcmp(t, "Float") == 0
        || strcmp(t, "Double") == 0 || strcmp(t, "Bool") == 0
        || strcmp(t, "Char") == 0 || strcmp(t, "Unit") == 0;
}

/* 引用字段原始对象指针；标量/String/空 → NULL（v0.49） */
void* hao_reflect_field_get_obj(HaoNativeHandle* h, void* obj, HaoString* fname) {
    void* meta = meta_raw(h);
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    char* name = hao_ffi_dup_cstr(fname);
    const HaoFieldMeta* f = find_field(m, name);
    void* out = NULL;
    free(name);
    if (!f || !obj) return NULL;
    if (is_scalar_field_type(f->typeStr)) return NULL;
    {
        char* base = (char*)obj + (size_t)f->slot * 8;
        out = *(void**)base;
    }
    return out;
}

/* 按下标读字段槽位型（格式化在 Hao；v0.79） */
int64_t hao_reflect_field_bits_at(HaoNativeHandle* h, void* obj, int32_t i) {
    void* meta = meta_raw(h);
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    const HaoFieldMeta* f;
    char* base;
    const char* t;
    int64_t out = 0;
    if (!obj || !m || i < 0 || i >= m->fieldCount) return 0;
    hao_gc_add_root(obj);
    if (!hao_gc_expect_heap_object(obj)) {
        char buf[96];
        int n = 0;
        const char* a = "reflect field_bits_at 非对象/已回收 obj=";
        while (*a && n < (int)sizeof(buf) - 1) buf[n++] = *a++;
        n = n + hao_fmt_ptr_angle(buf + n, (int)sizeof(buf) - n, obj);
        if (n < (int)sizeof(buf)) buf[n] = '\0';
        hao_panic_msg(buf);
    }
    f = &m->fields[i];
    base = (char*)obj + (size_t)f->slot * 8;
    t = f->typeStr;
    if (t && strcmp(t, "Int") == 0) {
        out = (int64_t)(*(int32_t*)base);
    } else if (t && strcmp(t, "Long") == 0) {
        out = *(int64_t*)base;
    } else if (t && strcmp(t, "UInt") == 0) {
        out = (int64_t)(uint64_t)(*(uint32_t*)base);
    } else if (t && (strcmp(t, "ULong") == 0 || strcmp(t, "UIntPtr") == 0)) {
        out = (int64_t)(*(uint64_t*)base);
    } else if (t && strcmp(t, "Short") == 0) {
        out = (int64_t)(*(int16_t*)base);
    } else if (t && strcmp(t, "UShort") == 0) {
        out = (int64_t)(uint64_t)(*(uint16_t*)base);
    } else if (t && strcmp(t, "Byte") == 0) {
        out = (int64_t)(uint64_t)(*(uint8_t*)base);
    } else if (t && strcmp(t, "SByte") == 0) {
        out = (int64_t)(*(int8_t*)base);
    } else if (t && strcmp(t, "Float") == 0) {
        float fv = *(float*)base;
        int32_t fb;
        memcpy(&fb, &fv, 4);
        out = (int64_t)(uint32_t)fb;
    } else if (t && strcmp(t, "Double") == 0) {
        double dv = *(double*)base;
        memcpy(&out, &dv, 8);
    } else if (t && strcmp(t, "Bool") == 0) {
        out = (*(int8_t*)base) ? 1 : 0;
    } else if (t && strcmp(t, "Char") == 0) {
        out = (int64_t)(*(int32_t*)base);
    } else if (t && strcmp(t, "Unit") == 0) {
        out = 0;
    } else if (type_is_base(t, "String")) {
        HaoString* s = *(HaoString**)base;
        if (s && !hao_gc_expect_heap_ptr(s)) {
            char buf[160];
            int n = 0;
            const char* a = "reflect String 字段悬空/脏指针 s=";
            while (*a && n < (int)sizeof(buf) - 1) buf[n++] = *a++;
            n = n + hao_fmt_ptr_angle(buf + n, (int)sizeof(buf) - n, s);
            if (n < (int)sizeof(buf)) buf[n] = '\0';
            hao_panic_msg(buf);
        }
        out = (int64_t)(uintptr_t)s;
    } else {
        out = (int64_t)(uintptr_t)(*(void**)base);
    }
    hao_gc_remove_root(obj);
    return out;
}

void* hao_reflect_field_get_obj_at(HaoNativeHandle* h, void* obj, int32_t i) {
    void* meta = meta_raw(h);
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    const HaoFieldMeta* f;
    void* out = NULL;
    if (!obj || !m || i < 0 || i >= m->fieldCount) return NULL;
    f = &m->fields[i];
    if (is_scalar_field_type(f->typeStr)) return NULL;
    {
        char* base = (char*)obj + (size_t)f->slot * 8;
        out = *(void**)base;
    }
    return out;
}

int32_t hao_reflect_field_is_static(HaoNativeHandle* h, int32_t i) {
    void* meta = meta_raw(h);
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    if (!m || i < 0 || i >= m->fieldCount) return 0;
    return m->fields[i].isStatic ? 1 : 0;
}

/* v0.79：field_get 拼串已删；按名路径在 Hao 循环 getFieldAt */

/* field_set 专用解析（非导出；整型/布尔 parse 已上移 Hao） */
static int field_parse_i32(const char* p, int32_t* out) {
    char* end = NULL;
    long long v;
    if (!p || !*p) return 0;
    errno = 0;
    v = strtoll(p, &end, 10);
    if (end == p || (end && *end != '\0')) return 0;
    if (v < (long long)INT32_MIN || v > (long long)INT32_MAX) return 0;
    *out = (int32_t)v;
    return 1;
}
static int field_parse_i64(const char* p, int64_t* out) {
    char* end = NULL;
    long long v;
    if (!p || !*p) return 0;
    errno = 0;
    v = strtoll(p, &end, 10);
    if (end == p || (end && *end != '\0')) return 0;
    if (errno == ERANGE) return 0;
    *out = (int64_t)v;
    return 1;
}
static int field_parse_u32(const char* p, uint32_t* out) {
    char* end = NULL;
    unsigned long long v;
    if (!p || !*p || p[0] == '-') return 0;
    errno = 0;
    v = strtoull(p, &end, 10);
    if (end == p || (end && *end != '\0')) return 0;
    if (errno == ERANGE || v > 4294967295ULL) return 0;
    *out = (uint32_t)v;
    return 1;
}
static int field_parse_u64(const char* p, uint64_t* out) {
    char* end = NULL;
    unsigned long long v;
    if (!p || !*p || p[0] == '-') return 0;
    errno = 0;
    v = strtoull(p, &end, 10);
    if (end == p || (end && *end != '\0')) return 0;
    if (errno == ERANGE) return 0;
    *out = (uint64_t)v;
    return 1;
}
static int field_parse_bool(const char* p, int8_t* out) {
    if (!p) return 0;
    if (strcmp(p, "true") == 0 || strcmp(p, "TRUE") == 0 || strcmp(p, "True") == 0) {
        *out = 1; return 1;
    }
    if (strcmp(p, "false") == 0 || strcmp(p, "FALSE") == 0 || strcmp(p, "False") == 0) {
        *out = 0; return 1;
    }
    return 0;
}

/* field_set: parse value into object slot; 0=ok */
int32_t hao_reflect_field_set(HaoNativeHandle* h, void* obj, HaoString* fname,
                              HaoString* value) {
    void* meta = meta_raw(h);
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    char* name = hao_ffi_dup_cstr(fname);
    const HaoFieldMeta* f = find_field(m, name);
    free(name);
    char* v;
    char* base;
    const char* t;
    if (!f || !obj || !value) return 1;
    base = (char*)obj + (size_t)f->slot * 8;
    t = f->typeStr;
    v = hao_ffi_dup_cstr(value);
    if (!v) return 1;
    if (t && strcmp(t, "Int") == 0) {
        int32_t n;
        if (!field_parse_i32(v, &n)) { free(v); return 1; }
        *(int32_t*)base = n;
    } else if (t && strcmp(t, "Long") == 0) {
        int64_t n;
        if (!field_parse_i64(v, &n)) { free(v); return 1; }
        *(int64_t*)base = n;
    } else if (t && strcmp(t, "UInt") == 0) {
        uint32_t n;
        if (!field_parse_u32(v, &n)) { free(v); return 1; }
        *(uint32_t*)base = n;
    } else if (t && strcmp(t, "ULong") == 0) {
        uint64_t n;
        if (!field_parse_u64(v, &n)) { free(v); return 1; }
        *(uint64_t*)base = n;
    } else if (t && strcmp(t, "UIntPtr") == 0) {
        uint64_t n;
        if (!field_parse_u64(v, &n)) { free(v); return 1; }
        *(uint64_t*)base = n;
    } else if (t && strcmp(t, "Short") == 0) {
        int32_t n;
        if (!field_parse_i32(v, &n) || n < INT16_MIN || n > INT16_MAX) {
            free(v); return 1;
        }
        *(int16_t*)base = (int16_t)n;
    } else if (t && strcmp(t, "UShort") == 0) {
        int32_t n;
        if (!field_parse_i32(v, &n) || n < 0 || n > 65535) { free(v); return 1; }
        *(uint16_t*)base = (uint16_t)n;
    } else if (t && strcmp(t, "Byte") == 0) {
        int32_t n;
        if (!field_parse_i32(v, &n) || n < 0 || n > 255) { free(v); return 1; }
        *(uint8_t*)base = (uint8_t)n;
    } else if (t && strcmp(t, "SByte") == 0) {
        int32_t n;
        if (!field_parse_i32(v, &n) || n < INT8_MIN || n > INT8_MAX) {
            free(v); return 1;
        }
        *(int8_t*)base = (int8_t)n;
    } else if (t && strcmp(t, "Float") == 0) {
        const char* end = NULL;
        double dv;
        if (hao_parse_double_cstr(v, &end, &dv) != 0) {
            free(v); return 1;
        }
        *(float*)base = (float)dv;
    } else if (t && strcmp(t, "Double") == 0) {
        const char* end = NULL;
        double dv;
        if (hao_parse_double_cstr(v, &end, &dv) != 0) {
            free(v); return 1;
        }
        *(double*)base = dv;
    } else if (t && strcmp(t, "Bool") == 0) {
        int8_t b;
        if (field_parse_bool(v, &b)) {
            *(int8_t*)base = b;
        } else if (strcmp(v, "1") == 0) {
            *(int8_t*)base = 1;
        } else if (strcmp(v, "0") == 0) {
            *(int8_t*)base = 0;
        } else {
            free(v);
            return 1;
        }
    } else if (t && strcmp(t, "Char") == 0) {
        int32_t n;
        if (!field_parse_i32(v, &n)) { free(v); return 1; }
        *(int32_t*)base = n;
    } else if (t && strcmp(t, "String") == 0) {
        HaoString* ns = hao_str_from_cstr(v);
        hao_gc_barrier(base, ns);
        *(HaoString**)base = ns;
    } else {
        free(v);
        return 1;
    }
    free(v);
    return 0;
}


typedef int64_t (*HaoInvokeFn)(int64_t, void*);

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

static int reflect_ret_is_raw_scalar(const char* t) {
    if (!t || !t[0]) return 0;
    if (strchr(t, '?')) return 0; /* Int? 等为装箱指针 */
    if (t[0] == '[') return 0;    /* 数组 */
    if (strcmp(t, "Unit") == 0 || strcmp(t, "void") == 0) return 1;
    if (strcmp(t, "Int") == 0 || strcmp(t, "Long") == 0) return 1;
    if (strcmp(t, "UInt") == 0 || strcmp(t, "ULong") == 0 ||
        strcmp(t, "UIntPtr") == 0)
        return 1;
    if (strcmp(t, "Short") == 0 || strcmp(t, "UShort") == 0) return 1;
    if (strcmp(t, "Byte") == 0 || strcmp(t, "SByte") == 0) return 1;
    if (strcmp(t, "Bool") == 0 || strcmp(t, "Char") == 0) return 1;
    if (strcmp(t, "Float") == 0 || strcmp(t, "Double") == 0) return 1;
    return 0;
}

int32_t hao_reflect_method_param_count(HaoNativeHandle* h, HaoString* name) {
    void* meta = meta_raw(h);
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    if (!m || !name) return -1;
    char* _mn = hao_ffi_dup_cstr(name);
    const HaoMethodMeta* mm = find_method(m, _mn);
    free(_mn);
    if (!mm || !mm->invoke) return -1;
    return (int32_t)mm->paramCount;
}

/* 按方法名查返回类型串（含父类链）；未找到 → ""（v0.49） */
HaoString* hao_reflect_method_return_type(HaoNativeHandle* h, HaoString* mname) {
    void* meta = meta_raw(h);
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    char* name = hao_ffi_dup_cstr(mname);
    if (!m || !name) {
        free(name);
        return hao_str_from_cstr("");
    }
    const HaoMethodMeta* mm = find_method(m, name);
    free(name);
    if (!mm || !mm->retType) return hao_str_from_cstr("");
    return hao_str_from_cstr(mm->retType);
}

/*
 * mode：
 *  0 = typed 标量/void（bool/double/…），不校验 ret
 *  1 = invoke():Long —— 仅 raw scalar，否则 panic（调用前）
 *  2 = invokeObj —— 引用；Unit→null；raw scalar（非 Unit）panic（调用前）
 */
static int64_t reflect_invoke_core(void* meta, void* obj, HaoString* name,
                                   int64_t* argSlots, int mode) {
    const HaoClassMeta* m = (const HaoClassMeta*)meta;
    const HaoMethodMeta* mm;
    /* thunk 可分配：直接挂根（禁先 is_heap_ptr） */
    if (obj) hao_gc_add_root(obj);
    if (name) hao_gc_add_root(name);
    if (argSlots) hao_gc_add_root(argSlots);
    {
        char* _mn2 = hao_ffi_dup_cstr(name);
        mm = find_method(m, _mn2);
        free(_mn2);
    }
    if (!mm || !mm->invoke) {
        if (argSlots) hao_gc_remove_root(argSlots);
        if (name) hao_gc_remove_root(name);
        if (obj) hao_gc_remove_root(obj);
        hao_panic_msg("reflect: method not found");
    }
    if (mode == 1 && !reflect_ret_is_raw_scalar(mm->retType)) {
        if (argSlots) hao_gc_remove_root(argSlots);
        if (name) hao_gc_remove_root(name);
        if (obj) hao_gc_remove_root(obj);
        hao_panic_msg(
            "reflect: invoke():Long 仅用于非可空标量；"
            "引用/可空/数组请用 invokeObj 或 invokeStr（对标 Java Method.invoke→Object）");
    }
    if (mode == 2 && reflect_ret_is_raw_scalar(mm->retType)) {
        const char* t = mm->retType ? mm->retType : "";
        if (strcmp(t, "Unit") != 0 && strcmp(t, "void") != 0) {
            if (argSlots) hao_gc_remove_root(argSlots);
            if (name) hao_gc_remove_root(name);
            if (obj) hao_gc_remove_root(obj);
            hao_panic_msg(
                "reflect: invokeObj 用于引用返回；非可空标量请用 invoke()/invokeBool/…");
        }
        /* Unit/void：不调用亦可；仍调用以保留副作用，返回 0 */
    }
    /* 含 0 参：长度必须精确匹配，防 OOB / 多余槽被忽略 */
    {
        int64_t need = mm->paramCount;
        int64_t got = argSlots ? hao_array_len(argSlots) : 0;
        if (need > 0 && !argSlots) {
            if (name) hao_gc_remove_root(name);
            if (obj) hao_gc_remove_root(obj);
            hao_panic_msg("reflect: null arg slots");
        }
        if (got != need) {
            if (argSlots) hao_gc_remove_root(argSlots);
            if (name) hao_gc_remove_root(name);
            if (obj) hao_gc_remove_root(obj);
            hao_panic_msg("reflect: arity mismatch");
        }
    }
    {
        HaoInvokeFn fn = (HaoInvokeFn)mm->invoke;
        int64_t r = fn((int64_t)(intptr_t)obj, argSlots);
        if (argSlots) hao_gc_remove_root(argSlots);
        if (name) hao_gc_remove_root(name);
        if (obj) hao_gc_remove_root(obj);
        if (mode == 2) {
            const char* t = mm->retType ? mm->retType : "";
            if (strcmp(t, "Unit") == 0 || strcmp(t, "void") == 0) return 0;
        }
        return r;
    }
}

int64_t hao_reflect_invoke(HaoNativeHandle* h, void* obj, HaoString* name, int64_t* argSlots) {
    void* meta = meta_raw(h);
    return reflect_invoke_core(meta, obj, name, argSlots, 1);
}

/* 对标 Java Method.invoke 的引用返回：ptr 由 Hao 调用约定挂 shadow 根 */
void* hao_reflect_invoke_obj(HaoNativeHandle* h, void* obj, HaoString* name, int64_t* argSlots) {
    void* meta = meta_raw(h);
    return (void*)(intptr_t)reflect_invoke_core(meta, obj, name, argSlots, 2);
}

HaoString* hao_reflect_invoke_str(HaoNativeHandle* h, void* obj, HaoString* name, int64_t* argSlots) {
    return (HaoString*)hao_reflect_invoke_obj(h, obj, name, argSlots);
}
int8_t hao_reflect_invoke_bool(HaoNativeHandle* h, void* obj, HaoString* name, int64_t* argSlots) {
    void* meta = meta_raw(h);
    return (int8_t)reflect_invoke_core(meta, obj, name, argSlots, 0);
}
double hao_reflect_invoke_double(HaoNativeHandle* h, void* obj, HaoString* name, int64_t* argSlots) {
    void* meta = meta_raw(h);
    int64_t r = reflect_invoke_core(meta, obj, name, argSlots, 0);
    double d; memcpy(&d, &r, 8); return d;
}
float hao_reflect_invoke_float(HaoNativeHandle* h, void* obj, HaoString* name, int64_t* argSlots) {
    void* meta = meta_raw(h);
    int64_t r = reflect_invoke_core(meta, obj, name, argSlots, 0);
    int32_t bits = (int32_t)r;
    float f; memcpy(&f, &bits, 4); return f;
}
void hao_reflect_invoke_void(HaoNativeHandle* h, void* obj, HaoString* name, int64_t* argSlots) {
    void* meta = meta_raw(h);
    reflect_invoke_core(meta, obj, name, argSlots, 0);
}

/* ---- 反射槽位转换助手：Int ↔ 具体类型（供构造 [Int] 实参/解释结果）---- */
int64_t hao_reflect_ptrtoint(void* p) { return (int64_t)(intptr_t)p; }
void*   hao_reflect_inttoptr(int64_t i) {
    void* p = (void*)(intptr_t)i;
    /* 无 safepoint：钉移交 Hao Object/String 局部根 */
    hao_gc_refl_i64_unpin(p);
    return p;
}
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
/* v0.79：删死导出 bool↔i64 包装 */
/* v0.76：若类有 $jsonWrite 则调用并返回 1；否则 0（回落反射 Bean） */
int32_t hao_json_try_write(void* obj, void* sb, int32_t features, int32_t indent,
                           void* stack, int32_t depth) {
    void* vt;
    const HaoClassMeta* m;
    if (!obj || !sb) return 0;
    if (!hao_gc_expect_heap_object(obj)) return 0;
    vt = *(void**)obj;
    m = NULL;
    for (const HaoClassMeta** p = g_metas; p && *p; ++p) {
        if ((*p)->vtablePtr == vt) { m = *p; break; }
    }
    if (!m || !m->jsonWrite) return 0;
    hao_gc_add_root(obj);
    hao_gc_add_root(sb);
    if (stack) hao_gc_add_root(stack);
    ((HaoJsonWriteFn)m->jsonWrite)(sb, obj, features, indent, stack, depth);
    if (stack) hao_gc_remove_root(stack);
    hao_gc_remove_root(sb);
    hao_gc_remove_root(obj);
    return 1;
}
