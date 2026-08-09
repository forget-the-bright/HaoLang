/*
 * HaoLang 运行时 —— 字符串（v0.27 HaoString 堆头）
 * ------------------------------------------------------------
 *  语言 String / String? = HaoString*（null = 空指针）。
 *  堆头：{len, cap, data[]}，data 为 UTF-8 + NUL（便于 C FFI）。
 *  .length / charAt / 下标：Unicode 码点语义。
 */
#include "runtime_internal.h"
#include <limits.h>
#include <errno.h>

#ifndef offsetof
#define offsetof(t, m) ((size_t)&(((t*)0)->m))
#endif

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

/* 字节下标 → 码点下标 */
static int32_t utf8_cp_index_of_byte(const char* s, int32_t n, int32_t byte_idx) {
    if (byte_idx < 0) return -1;
    if (byte_idx > n) byte_idx = n;
    int32_t i = 0, cps = 0;
    while (i < byte_idx && i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) i += 1;
        else if ((c & 0xE0) == 0xC0) i += 2;
        else if ((c & 0xF0) == 0xE0) i += 3;
        else if ((c & 0xF8) == 0xF0) i += 4;
        else i += 1;
        cps++;
    }
    return cps;
}

/* ---- 分配 / 构造 ---- */

HaoString* hao_str_alloc(int32_t byte_len) {
    if (byte_len < 0) byte_len = 0;
    /* cap = byte_len+1 须落在 int32；并防 size_t 溢出 */
    if (byte_len >= INT32_MAX) byte_len = INT32_MAX - 1;
    size_t hdr = offsetof(HaoString, data);
    if (hdr > SIZE_MAX - (size_t)byte_len - 1) {
        fputs("panic: 字符串过大\n", stderr);
        exit(1);
    }
    size_t sz = hdr + (size_t)byte_len + 1;
    HaoString* s = (HaoString*)gc_alloc_ex(sz, GC_KIND_OPAQUE, 0);
    s->len = byte_len;
    s->cap = byte_len + 1;
    s->data[byte_len] = '\0';
    return s;
}

HaoString* hao_str_from_bytes(const char* bytes, int32_t byte_len) {
    if (byte_len < 0) byte_len = 0;
    HaoString* s = hao_str_alloc(byte_len);
    if (byte_len > 0 && bytes) memcpy(s->data, bytes, (size_t)byte_len);
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
    return s ? s->data : NULL;
}

HaoString* hao_str_byte_slice(HaoString* s, int32_t start, int32_t end) {
    if (!s) return hao_str_from_cstr("");
    hao_gc_add_root(s);
    if (start < 0) start = 0;
    if (end > s->len) end = s->len;
    if (end < start) end = start;
    HaoString* r = hao_str_from_bytes(s->data + start, end - start);
    hao_gc_remove_root(s);
    return r;
}

int32_t hao_str_byte_len(HaoString* s) {
    return s ? s->len : 0;
}

/* 子串首次出现的字节下标；未找到 -1。from 为起始字节（夹紧）。 */
int32_t hao_str_byte_index_of(HaoString* s, HaoString* sub, int32_t from) {
    if (!s) s = hao_str_from_cstr("");
    if (!sub || sub->len == 0) {
        if (from < 0) from = 0;
        if (from > s->len) from = s->len;
        return from;
    }
    if (from < 0) from = 0;
    if (sub->len > s->len || from > s->len - sub->len) return -1;
    int32_t lim = s->len - sub->len;
    for (int32_t i = from; i <= lim; i++) {
        if (memcmp(s->data + i, sub->data, (size_t)sub->len) == 0)
            return i;
    }
    return -1;
}

/* ---- 拼接 / 转串 ---- */

HaoString* hao_str_concat(HaoString* a, HaoString* b) {
    const char* da;
    const char* db;
    int32_t la, lb;
    if (!a) { da = "null"; la = 4; }
    else { da = a->data; la = a->len; }
    if (!b) { db = "null"; lb = 4; }
    else { db = b->data; lb = b->len; }
    if (la < 0) la = 0;
    if (lb < 0) lb = 0;
    /* 有符号相加溢出会变成负数，hao_str_alloc 夹成 0 后仍 memcpy → 堆破坏 */
    /* hao_str_alloc 最大载荷 INT32_MAX-1；允许 ==INT32_MAX 会被夹紧后 memcpy 越界 */
    if ((uint64_t)(uint32_t)la + (uint64_t)(uint32_t)lb > (uint64_t)(INT32_MAX - 1)) {
        fprintf(stderr, "panic: string concat length overflow\n");
        abort();
    }
    /* alloc 可触发 GC：形参直接挂根（禁先 is_heap_ptr，其内 safepoint） */
    if (a) hao_gc_add_root(a);
    if (b) hao_gc_add_root(b);
    HaoString* r = hao_str_alloc(la + lb);
    /* alloc 后可能移动语义不适用；仍从原指针读 data（未压缩移动） */
    if (a) { da = a->data; la = a->len; if (la < 0) la = 0; }
    if (b) { db = b->data; lb = b->len; if (lb < 0) lb = 0; }
    memcpy(r->data, da, (size_t)la);
    memcpy(r->data + la, db, (size_t)lb);
    if (a) hao_gc_remove_root(a);
    if (b) hao_gc_remove_root(b);
    return r;
}

HaoString* hao_int_to_str(int32_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%d", (int)v);
    return hao_str_from_bytes(buf, n);
}

HaoString* hao_long_to_str(int64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%lld", (long long)v);
    return hao_str_from_bytes(buf, n);
}

HaoString* hao_uint_to_str(uint32_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%u", (unsigned)v);
    return hao_str_from_bytes(buf, n);
}

HaoString* hao_ulong_to_str(uint64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%llu", (unsigned long long)v);
    return hao_str_from_bytes(buf, n);
}

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

HaoString* hao_bool_to_str(int8_t v) {
    return hao_str_from_cstr(v ? "true" : "false");
}

/* Char 码点 → 单字符 UTF-8 串 */
HaoString* hao_char_to_str(int32_t cp) {
    char buf[4];
    int n = utf8_encode(cp, buf);
    return hao_str_from_bytes(buf, n);
}

/* 码点个数（String.length） */
int64_t hao_str_len(HaoString* s) {
    if (!s) return 0;
    return (int64_t)utf8_cp_count(s->data, s->len);
}

int8_t hao_str_eq(HaoString* a, HaoString* b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->len != b->len) return 0;
    return memcmp(a->data, b->data, (size_t)a->len) == 0 ? 1 : 0;
}

int8_t hao_str_is_empty(HaoString* s) {
    return (!s || s->len == 0) ? 1 : 0;
}

/* [start, end) 半开，按码点；越界钳制 */
HaoString* hao_str_substring(HaoString* s, int32_t start, int32_t end) {
    if (!s) return hao_str_from_cstr("");
    /* 直接挂根；禁止先 is_heap_ptr（其内 safepoint） */
    hao_gc_add_root(s);
    int32_t cps = utf8_cp_count(s->data, s->len);
    if (start < 0) start = 0;
    if (end < start) end = start;
    if (start > cps) start = cps;
    if (end > cps) end = cps;
    int32_t b0 = utf8_byte_of_cp(s->data, s->len, start);
    int32_t b1 = utf8_byte_of_cp(s->data, s->len, end);
    if (b0 < 0) b0 = 0;
    if (b1 < 0) b1 = s->len;
    HaoString* r = hao_str_from_bytes(s->data + b0, b1 - b0);
    hao_gc_remove_root(s);
    return r;
}

/* 码点下标 from 起搜；对齐 Java String.indexOf(sub, fromIndex) */
int32_t hao_str_index_of_from(HaoString* s, HaoString* sub, int32_t from) {
    if (!s) s = hao_str_from_cstr("");
    int32_t cps = utf8_cp_count(s->data, s->len);
    if (from < 0) from = 0;
    if (!sub || sub->len == 0) {
        return from > cps ? cps : from;
    }
    if (from >= cps) return -1;
    if (sub->len > s->len) return -1;
    int32_t b0 = utf8_byte_of_cp(s->data, s->len, from);
    if (b0 < 0) b0 = 0;
    /* 用 s->len - sub->len 作上界，避免 i+sub->len 在接近 INT32_MAX 时有符号回绕 */
    int32_t lim = s->len - sub->len;
    for (int32_t i = b0; i <= lim; i++) {
        if (memcmp(s->data + i, sub->data, (size_t)sub->len) == 0)
            return utf8_cp_index_of_byte(s->data, s->len, i);
    }
    return -1;
}

int32_t hao_str_index_of(HaoString* s, HaoString* sub) {
    return hao_str_index_of_from(s, sub, 0);
}

int32_t hao_str_last_index_of(HaoString* s, HaoString* sub) {
    if (!s) s = hao_str_from_cstr("");
    if (!sub || sub->len == 0) return utf8_cp_count(s->data, s->len);
    if (sub->len > s->len) return -1;
    int32_t last = -1;
    int32_t lim = s->len - sub->len;
    for (int32_t i = 0; i <= lim; i++) {
        if (memcmp(s->data + i, sub->data, (size_t)sub->len) == 0)
            last = utf8_cp_index_of_byte(s->data, s->len, i);
    }
    return last;
}

int8_t hao_str_contains(HaoString* s, HaoString* sub) {
    return hao_str_index_of(s, sub) >= 0 ? 1 : 0;
}

int8_t hao_str_starts_with(HaoString* s, HaoString* prefix) {
    if (!s) return (!prefix || prefix->len == 0) ? 1 : 0;
    if (!prefix || prefix->len == 0) return 1;
    if (prefix->len > s->len) return 0;
    return memcmp(s->data, prefix->data, (size_t)prefix->len) == 0 ? 1 : 0;
}

int8_t hao_str_ends_with(HaoString* s, HaoString* suffix) {
    if (!s) return (!suffix || suffix->len == 0) ? 1 : 0;
    if (!suffix || suffix->len == 0) return 1;
    if (suffix->len > s->len) return 0;
    return memcmp(s->data + (s->len - suffix->len), suffix->data,
                  (size_t)suffix->len) == 0 ? 1 : 0;
}

/* 按码点下标取 Char；越界 -1 */
int32_t hao_str_char_at(HaoString* s, int64_t i) {
    /* 码点下标用 i64 入参，避免语言侧 Long 经 i32 截断后静默错位 */
    if (!s || i < 0 || i > (int64_t)INT32_MAX) return -1;
    int32_t ii = (int32_t)i;
    int32_t b = utf8_byte_of_cp(s->data, s->len, ii);
    if (b < 0 || b >= s->len) return -1;
    int32_t pos = b;
    return utf8_decode(s->data, s->len, &pos);
}

HaoString* hao_str_trim(HaoString* s) {
    if (!s) return hao_str_from_cstr("");
    hao_gc_add_root(s);
    int32_t a = 0, b = s->len;
    while (a < b) {
        char c = s->data[a];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        a++;
    }
    while (b > a) {
        char c = s->data[b - 1];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        b--;
    }
    HaoString* r = hao_str_from_bytes(s->data + a, b - a);
    hao_gc_remove_root(s);
    return r;
}

HaoString* hao_str_to_upper(HaoString* s) {
    if (!s) return hao_str_from_cstr("");
    hao_gc_add_root(s);
    HaoString* r = hao_str_alloc(s->len);
    for (int32_t i = 0; i < s->len; i++) {
        char c = s->data[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        r->data[i] = c;
    }
    hao_gc_remove_root(s);
    return r;
}

HaoString* hao_str_to_lower(HaoString* s) {
    if (!s) return hao_str_from_cstr("");
    hao_gc_add_root(s);
    HaoString* r = hao_str_alloc(s->len);
    for (int32_t i = 0; i < s->len; i++) {
        char c = s->data[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        r->data[i] = c;
    }
    hao_gc_remove_root(s);
    return r;
}

int32_t hao_str_compare(HaoString* a, HaoString* b) {
    const char* da = a ? a->data : "";
    const char* db = b ? b->data : "";
    int32_t la = a ? a->len : 0;
    int32_t lb = b ? b->len : 0;
    int32_t n = la < lb ? la : lb;
    int c = memcmp(da, db, (size_t)n);
    if (c < 0) return -1;
    if (c > 0) return 1;
    if (la < lb) return -1;
    if (la > lb) return 1;
    return 0;
}

/* ---- 解析（入参 HaoString*；内部用 data）---- */

void* hao_parse_int(HaoString* s) {
    const char* p = hao_str_cstr(s);
    if (!p || !*p) return NULL;
    char* end = NULL;
    long long v = strtoll(p, &end, 10);
    if (end == p || (end && *end != '\0')) return NULL;
    if (v < (long long)INT32_MIN || v > (long long)INT32_MAX) return NULL;
    return hao_box_i32((int32_t)v);
}

void* hao_parse_long(HaoString* s) {
    const char* p = hao_str_cstr(s);
    if (!p || !*p) return NULL;
    char* end = NULL;
    errno = 0;
    long long v = strtoll(p, &end, 10);
    if (end == p || (end && *end != '\0')) return NULL;
    if (errno == ERANGE) return NULL;
    return hao_box_i64((int64_t)v);
}

void* hao_parse_uint(HaoString* s) {
    const char* p = hao_str_cstr(s);
    if (!p || !*p) return NULL;
    if (p[0] == '-') return NULL;
    char* end = NULL;
    errno = 0;
    unsigned long long v = strtoull(p, &end, 10);
    if (end == p || (end && *end != '\0')) return NULL;
    if (errno == ERANGE || v > 4294967295ULL) return NULL;
    return hao_box_i32((int32_t)(uint32_t)v);
}

void* hao_parse_ulong(HaoString* s) {
    const char* p = hao_str_cstr(s);
    if (!p || !*p) return NULL;
    if (p[0] == '-') return NULL;
    char* end = NULL;
    errno = 0;
    unsigned long long v = strtoull(p, &end, 10);
    if (end == p || (end && *end != '\0')) return NULL;
    if (errno == ERANGE) return NULL;
    return hao_box_i64((int64_t)v);
}

void* hao_parse_double(HaoString* s) {
    const char* p = hao_str_cstr(s);
    if (!p || !*p) return NULL;
    char* end = NULL;
    errno = 0;
    double v = strtod(p, &end);
    if (end == p || (end && *end != '\0')) return NULL;
    if (errno == ERANGE) return NULL;
    return hao_box_f64(v);
}

void* hao_parse_float(HaoString* s) {
    const char* p = hao_str_cstr(s);
    if (!p || !*p) return NULL;
    char* end = NULL;
    errno = 0;
    float v = strtof(p, &end);
    if (end == p || (end && *end != '\0')) return NULL;
    if (errno == ERANGE) return NULL;
    return hao_box_f32(v);
}

void* hao_parse_bool(HaoString* s) {
    const char* p = hao_str_cstr(s);
    if (!p) return NULL;
    if (strcmp(p, "true") == 0 || strcmp(p, "TRUE") == 0 || strcmp(p, "True") == 0)
        return hao_box_i32(1);
    if (strcmp(p, "false") == 0 || strcmp(p, "FALSE") == 0 || strcmp(p, "False") == 0)
        return hao_box_i32(0);
    return NULL;
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
