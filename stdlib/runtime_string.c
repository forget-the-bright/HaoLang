/*
 * HaoLang 运行时 —— 字符串（v0.55.60：头对象 + UTF-8 [Byte]）
 * ------------------------------------------------------------
 *  语言 String / String? = HaoString*（null = 空指针）。
 *  头：GC_SLOTS{ bytes→[Byte], cp_len }；载荷 cap≥len+1 且 data[len]=NUL。
 *  .length / charAt / 下标：Unicode 码点语义。
 */
#include "runtime_internal.h"
#include <limits.h>
#include <errno.h>

/* ---- UTF-8 辅助 ---- */

static int32_t utf8_cp_count(const char* s, int32_t n) {
    int32_t i = 0, cps = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) i += 1;
        else if ((c & 0xE0) == 0xC0) i += 2;
        else if ((c & 0xF0) == 0xE0) i += 3;
        else if ((c & 0xF8) == 0xF0) i += 4;
        else i += 1;
        if (i > n) break;
        cps++;
    }
    return cps;
}

/* ---- 局部访问载荷 ---- */

static char* sdata(HaoString* s) {
    return (s && s->bytes) ? (char*)s->bytes : NULL;
}

static int32_t sblen(HaoString* s) {
    if (!s || !s->bytes) return 0;
    return (int32_t)hao_array_len(s->bytes);
}

/* ---- 分配 / 构造 ---- */

HaoString* hao_str_alloc(int32_t byte_len) {
    if (byte_len < 0) byte_len = 0;
    /* cap = byte_len+1 须落在 int32 */
    if (byte_len >= INT32_MAX) byte_len = INT32_MAX - 1;

    void* arr = hao_array_new((int64_t)byte_len + 1, 1, 0);
    hao_gc_add_root(arr);
    ((char*)arr)[byte_len] = '\0';
    *(int64_t*)((char*)arr - HAO_ARR_LEN_OFF) = (int64_t)byte_len;

    /* bitmap bit0：slot0 bytes 为 GC 指针 */
    HaoString* s = (HaoString*)hao_object_new(2, 1);
    hao_gc_add_root(s);
    hao_gc_barrier(&s->bytes, arr);
    s->bytes = arr;
    s->cp_len = -1;
    hao_gc_remove_root(arr);
    hao_gc_remove_root(s);
    return s;
}

HaoString* hao_str_from_bytes(const char* bytes, int32_t byte_len) {
    if (byte_len < 0) byte_len = 0;
    HaoString* s = hao_str_alloc(byte_len);
    if (byte_len > 0 && bytes) {
        char* d = sdata(s);
        if (d) memcpy(d, bytes, (size_t)byte_len);
    }
    /* 造串填 cp_len（对齐 Java 构造时缓存） */
    {
        char* d = sdata(s);
        s->cp_len = (int64_t)utf8_cp_count(d ? d : "", byte_len);
    }
    return s;
}

HaoString* hao_str_from_cstr(const char* c) {
    if (!c) c = "";
    size_t n = strlen(c);
    if (n > (size_t)(INT32_MAX - 1)) {
        fputs("panic: C 字符串过长\n", stderr);
        exit(1);
    }
    return hao_str_from_bytes(c, (int32_t)n);
}

const char* hao_str_cstr(const HaoString* s) {
    /* 桥私有：返回 GC 堆内字节指针。
     * 禁止：作为对外合法 FFI 模式借给 libc / 跨 safepoint / 跨线程长期持有。
     * 合法出桥：hao_ffi_dup_cstr / hao_ffi_dup_bytes。
     * 审计允许：runtime_* 内同步读自身串（hash/parse/reflect 名比对）；填 Hao 自有缓冲用 hao_str_data。
     * 审计清单见 runtime_internal.h「cstr/data 桥私有调用约定」。 */
    if (!s || !s->bytes) return NULL;
    return (const char*)s->bytes;
}

char* hao_str_data(HaoString* s) {
    /* 仅「写入本串已挂根缓冲」；禁止导出给 C 长期持有 */
    return sdata(s);
}

int32_t hao_str_byte_len(HaoString* s) {
    return sblen(s);
}

int8_t hao_str_eq(HaoString* a, HaoString* b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    int32_t la = sblen(a), lb = sblen(b);
    if (la != lb) return 0;
    char* da = sdata(a);
    char* db = sdata(b);
    if (!da || !db) return la == 0 ? 1 : 0;
    return memcmp(da, db, (size_t)la) == 0 ? 1 : 0;
}

void* hao_make_args(int argc, char** argv) {
    int n = argc > 0 ? argc - 1 : 0;
    if (n < 0) n = 0;
    /* [String]：与 hao_array_new(..., is_ptr=1) 同路径 */
    HaoString** elems = (HaoString**)hao_array_new(n, 8, 1);
    hao_gc_add_root(elems); /* 填充循环内 from_cstr 可触发 GC */
    for (int i = 0; i < n; ++i) {
        const char* a = (argv && argv[i + 1]) ? argv[i + 1] : "";
        HaoString* s = hao_str_from_cstr(a);
        hao_gc_barrier(&elems[i], s);
        elems[i] = s;
    }
    hao_gc_remove_root(elems);
    return elems;
}
