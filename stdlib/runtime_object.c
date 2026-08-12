/*
 * HaoLang 运行时 —— 对象分配与运行时类型判定
 * ------------------------------------------------------------
 *  对象内存布局：连续的字段槽位，每个字段固定 8 字节。字段偏移在
 *  编译期确定（按声明顺序），IR 中直接 getelementptr 定位。
 *  闭包 env（[fnptr | cap...]）也复用 hao_object_new 分配。
 *
 *  类型判定（is/as）：对象槽位 0 存虚表指针，每个类的虚表是唯一
 *  全局常量，因此虚表地址可直接作为类型标识；子类判定由编译期生成的
 *  「类型及其所有子类虚表列表」（NULL 结尾）支持。
 */
#include "runtime_internal.h"

/* 分配含 nfields 个槽的对象；bitmap 标明哪些槽是 GC 指针。*/
void* hao_object_new(int64_t nfields, int64_t bitmap) {
    if (nfields < 0) nfields = 0;
    if ((uint64_t)nfields > (UINT64_MAX / 8ULL)) {
        fputs("panic: 对象字段数过大\n", stderr);
        exit(1);
    }
    size_t bytes = (size_t)nfields * 8;
    if (bytes < 16) bytes = 16;
    /* 超过 32 槽时位图装不下：退回 FULL 保守扫 */
    uint8_t kind = (nfields > 32) ? GC_KIND_FULL : GC_KIND_SLOTS;
    uint64_t meta = (kind == GC_KIND_FULL) ? 0 : ((uint64_t)bitmap & 0xFFFFFFFFu);
    return gc_alloc_ex(bytes, kind, meta);
}

int8_t hao_type_is(void* obj, void** allowed) {
    if (!obj || !allowed) return 0;
    void* vt = *(void**)obj;
    for (void** p = allowed; *p; ++p)
        if (*p == vt) return 1;
    return 0;
}

int32_t hao_object_hashCode(void* obj) {
    uintptr_t p = (uintptr_t)obj;
    return (int32_t)(p ^ (p >> 32));
}

int8_t hao_object_equals(void* a, void* b) {
    return a == b;
}

int hao_fmt_u64_dec(char* out, int cap, uint64_t u) {
    char tmp[24];
    int n = 0, len = 0;
    if (!out || cap <= 0) return 0;
    do {
        tmp[n++] = (char)('0' + (u % 10));
        u /= 10;
    } while (u && n < (int)sizeof(tmp));
    if (n >= cap) n = cap - 1;
    for (int i = 0; i < n; ++i) out[len++] = tmp[n - 1 - i];
    out[len] = '\0';
    return len;
}

int hao_fmt_i64_dec(char* out, int cap, int64_t v) {
    uint64_t u;
    int len = 0;
    if (!out || cap <= 0) return 0;
    if (v < 0) {
        if (cap < 2) { out[0] = '\0'; return 0; }
        out[len++] = '-';
        u = (uint64_t)(-(v + 1)) + 1ull;
    } else {
        u = (uint64_t)v;
    }
    return len + hao_fmt_u64_dec(out + len, cap - len, u);
}

int hao_fmt_ptr_angle(char* out, int cap, const void* p) {
    static const char* hex = "0123456789abcdef";
    uint64_t u = (uint64_t)(uintptr_t)p;
    char tmp[16];
    int n = 0, len = 0, i;
    if (!out || cap < 5) {
        if (out && cap > 0) out[0] = '\0';
        return 0;
    }
    out[len++] = '<';
    out[len++] = '0';
    out[len++] = 'x';
    do {
        tmp[n++] = hex[u & 0xf];
        u >>= 4;
    } while (u && n < 16);
    while (n < 16) tmp[n++] = '0'; /* 固定 16 位宽，对齐旧 snprintf %016llx */
    if (len + n + 2 >= cap) n = cap - len - 2;
    for (i = 0; i < n; ++i) out[len++] = tmp[n - 1 - i];
    out[len++] = '>';
    out[len] = '\0';
    return len;
}

HaoString* hao_object_toString(void* obj) {
    char buf[32];
    int n = hao_fmt_ptr_angle(buf, (int)sizeof buf, obj);
    return hao_str_from_bytes(buf, n);
}
