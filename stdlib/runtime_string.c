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

/* 第 cp_idx 个码点的起始字节下标；越界返回 -1 */
static int32_t utf8_byte_of_cp(const char* s, int32_t n, int32_t cp_idx) {
    if (cp_idx < 0) return -1;
    int32_t i = 0, cps = 0;
    while (i < n) {
        if (cps == cp_idx) return i;
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) i += 1;
        else if ((c & 0xE0) == 0xC0) i += 2;
        else if ((c & 0xF0) == 0xE0) i += 3;
        else if ((c & 0xF8) == 0xF0) i += 4;
        else i += 1;
        if (i > n) break;
        cps++;
    }
    if (cps == cp_idx) return i; /* 允许 end == len（半开） */
    return -1;
}

/* 从字节下标解码一个码点，推进 *pio；失败返回 -1 */
static int32_t utf8_decode(const char* s, int32_t n, int32_t* pio) {
    int32_t i = *pio;
    if (i < 0 || i >= n) return -1;
    unsigned char c = (unsigned char)s[i];
    int32_t cp;
    int32_t need;
    if (c < 0x80) { cp = c; need = 1; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; need = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; need = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; need = 4; }
    else { *pio = i + 1; return (int32_t)c; }
    if (i + need > n) { *pio = n; return -1; }
    for (int32_t k = 1; k < need; k++) {
        unsigned char x = (unsigned char)s[i + k];
        if ((x & 0xC0) != 0x80) { *pio = i + 1; return -1; }
        cp = (cp << 6) | (x & 0x3F);
    }
    *pio = i + need;
    return cp;
}

static int utf8_encode(int32_t cp, char out[4]) {
    if (cp < 0) cp = 0xFFFD;
    if (cp <= 0x7F) { out[0] = (char)cp; return 1; }
    if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp > 0x10FFFF) cp = 0xFFFD;
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* ---- 局部访问载荷 ---- */

static char* sdata(HaoString* s) {
    return (s && s->bytes) ? (char*)s->bytes : NULL;
}

static int32_t sblen(HaoString* s) {
    if (!s || !s->bytes) return 0;
    return (int32_t)hao_array_len(s->bytes);
}

/* 公开：码点下标 → 字节下标（substring Hao 原语）；越界 -1 */
int32_t hao_str_byte_of_cp(HaoString* s, int32_t cp_idx) {
    if (!s) return cp_idx == 0 ? 0 : -1;
    char* d = sdata(s);
    int32_t n = sblen(s);
    return utf8_byte_of_cp(d ? d : "", n, cp_idx);
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

void hao_str_set_byte_len(HaoString* s, int32_t n) {
    if (!s || !s->bytes) return;
    int64_t cap = hao_array_cap(s->bytes);
    if (cap < 1) return;
    if (n < 0) n = 0;
    if ((int64_t)n >= cap) n = (int32_t)(cap - 1);
    char* d = (char*)s->bytes;
    d[n] = '\0';
    *(int64_t*)((char*)s->bytes - HAO_ARR_LEN_OFF) = (int64_t)n;
    s->cp_len = -1;
}

HaoString* hao_str_byte_slice(HaoString* s, int32_t start, int32_t end) {
    if (!s) return hao_str_from_cstr("");
    hao_gc_add_root(s);
    int32_t len = sblen(s);
    char* d = sdata(s);
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (end < start) end = start;
    HaoString* r = hao_str_from_bytes(d ? d + start : "", end - start);
    if (r) hao_gc_add_root(r); /* 摘 s 前挂新串，防并发 collect */
    hao_gc_remove_root(s);
    if (r) hao_gc_remove_root(r);
    return r;
}

int32_t hao_str_byte_len(HaoString* s) {
    return sblen(s);
}

/* ---- 拼接 / 转串 ---- */

HaoString* hao_str_concat(HaoString* a, HaoString* b) {
    const char* da;
    const char* db;
    int32_t la, lb;
    if (!a) { da = "null"; la = 4; }
    else { da = sdata(a); la = sblen(a); if (!da) { da = ""; la = 0; } }
    if (!b) { db = "null"; lb = 4; }
    else { db = sdata(b); lb = sblen(b); if (!db) { db = ""; lb = 0; } }
    if (la < 0) la = 0;
    if (lb < 0) lb = 0;
    /* 有符号相加溢出会变成负数，hao_str_alloc 夹成 0 后仍 memcpy → 堆破坏 */
    if ((uint64_t)(uint32_t)la + (uint64_t)(uint32_t)lb > (uint64_t)(INT32_MAX - 1)) {
        fprintf(stderr, "panic: string concat length overflow\n");
        abort();
    }
    /* alloc 可触发 GC：形参直接挂根（禁先 is_heap_ptr，其内 safepoint） */
    if (a) hao_gc_add_root(a);
    if (b) hao_gc_add_root(b);
    HaoString* r = hao_str_alloc(la + lb);
    if (a) { da = sdata(a); la = sblen(a); if (la < 0) la = 0; if (!da) { da = ""; la = 0; } }
    if (b) { db = sdata(b); lb = sblen(b); if (lb < 0) lb = 0; if (!db) { db = ""; lb = 0; } }
    char* rd = sdata(r);
    if (rd) {
        memcpy(rd, da, (size_t)la);
        memcpy(rd + la, db, (size_t)lb);
    }
    /* 新串须先入根再摘 a/b：否则并发 STW 只见旧根、扫掉 r → str_len UAF */
    if (r) hao_gc_add_root(r);
    if (a) hao_gc_remove_root(a);
    if (b) hao_gc_remove_root(b);
    if (r) hao_gc_remove_root(r);
    return r;
}

/* 手写十进制，避开 Win64 栈未齐时 libcmt snprintf 的 movdqa AV（v0.55.54）
 * 仅供 float/double toStr 旁路不再需要；整型 toStr 已上移 Hao。 */

HaoString* hao_float_to_str(float v) {
    char buf[64];
    int n = snprintf(buf, sizeof buf, "%g", (double)v);
    return hao_str_from_bytes(buf, n);
}

HaoString* hao_double_to_str(double v) {
    char buf[64];
    int n = snprintf(buf, sizeof buf, "%g", v);
    return hao_str_from_bytes(buf, n);
}

/* 码点个数（String.length） */
int64_t hao_str_len(HaoString* s) {
    uintptr_t v;
    if (!s) return 0;
    /*
     * 崩溃日志曾见 rcx=ASCII「collectC」：把 String 载荷当 HaoString*。
     * 非规范/明显非堆指针直接 panic，避免 0xC0000005 难读。
     */
    v = (uintptr_t)s;
    if ((v & (sizeof(void*) - 1)) != 0 || v < (uintptr_t)0x10000 ||
#if UINTPTR_MAX > 0xffffffffu
        v > (uintptr_t)0x00007FFFFFFFFFFFULL ||
#endif
        !hao_gc_expect_heap_ptr(s)) {
        fprintf(stderr, "panic: hao_str_len 非法指针 %p\n", (void*)s);
        fflush(stderr);
        abort();
    }
    if (s->cp_len >= 0) return s->cp_len;
    char* d = sdata(s);
    int32_t n = sblen(s);
    int64_t cps = (int64_t)utf8_cp_count(d ? d : "", n);
    s->cp_len = cps;
    return cps;
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

/* v0.56：substring / char_to_str / byte_index_of 已上移 Hao；char_at 仍供 IR s[i] */

/* 按码点下标取 Char；越界 -1 */
int32_t hao_str_char_at(HaoString* s, int64_t i) {
    /* 码点下标用 i64 入参，避免语言侧 Long 经 i32 截断后静默错位 */
    if (!s || i < 0 || i > (int64_t)INT32_MAX) return -1;
    int32_t ii = (int32_t)i;
    char* d = sdata(s);
    int32_t n = sblen(s);
    if (!d) return -1;
    int32_t b = utf8_byte_of_cp(d, n, ii);
    if (b < 0 || b >= n) return -1;
    int32_t pos = b;
    return utf8_decode(d, n, &pos);
}

/* 公开 getBytes：拷贝载荷（不含尾 NUL） */
void* hao_str_get_bytes(HaoString* s) {
    int32_t n = sblen(s);
    char* d = sdata(s);
    if (s) hao_gc_add_root(s);
    void* arr = hao_array_new((int64_t)n, 1, 0);
    if (n > 0 && d && arr) memcpy(arr, d, (size_t)n);
    if (s) hao_gc_remove_root(s);
    return arr;
}

/* 从 [Byte] 建串；逻辑长度 = 数组 len，工厂补尾 NUL */
HaoString* hao_str_from_byte_arr(void* arr) {
    if (!arr) return hao_str_from_cstr("");
    hao_gc_add_root(arr);
    int64_t n64 = hao_array_len(arr);
    if (n64 < 0) n64 = 0;
    if (n64 > (int64_t)(INT32_MAX - 1)) {
        hao_gc_remove_root(arr);
        fputs("panic: byte 数组过长无法建串\n", stderr);
        exit(1);
    }
    HaoString* s = hao_str_from_bytes((const char*)arr, (int32_t)n64);
    hao_gc_remove_root(arr);
    return s;
}

/* ---- 解析（float/double 留 C：dup 出桥后再 strto*）---- */

void* hao_parse_double(HaoString* s) {
    char* p = hao_ffi_dup_cstr(s);
    char* end = NULL;
    double v;
    if (!p || !*p) { free(p); return NULL; }
    errno = 0;
    v = strtod(p, &end);
    if (end == p || (end && *end != '\0') || errno == ERANGE) {
        free(p);
        return NULL;
    }
    free(p);
    return hao_box_f64(v);
}

void* hao_parse_float(HaoString* s) {
    char* p = hao_ffi_dup_cstr(s);
    char* end = NULL;
    float v;
    if (!p || !*p) { free(p); return NULL; }
    errno = 0;
    v = strtof(p, &end);
    if (end == p || (end && *end != '\0') || errno == ERANGE) {
        free(p);
        return NULL;
    }
    free(p);
    return hao_box_f32(v);
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
