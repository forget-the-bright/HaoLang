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
 *
 *  扫描：≤32 槽 SLOTS；>32 槽 GC_KIND_BITMAP（位图挂槽区尾）。禁止 FULL。
 */
#include "runtime_internal.h"

static size_t gc_bitmap_trailer_bytes(size_t nslots) {
    /* u32 字向上取整，再对齐到 8 */
    size_t nw = (nslots + 31u) / 32u;
    size_t b = nw * 4u;
    return (b + 7u) & ~(size_t)7u;
}

static void gc_write_bitmap_trailer(void* user, size_t nslots, const uint64_t* words,
                                    size_t nwords) {
    size_t i;
    uint32_t* dst;
    if (!user || nslots == 0) return;
    dst = (uint32_t*)((char*)user + nslots * 8u);
    {
        size_t nw32 = (nslots + 31u) / 32u;
        for (i = 0; i < nw32; ++i) dst[i] = 0;
    }
    if (!words || nwords <= 0) return;
    for (i = 0; i < nslots; ++i) {
        size_t wi = i / 64u;
        unsigned sh = (unsigned)(i % 64u);
        if (wi >= (size_t)nwords) break;
        if ((words[wi] >> sh) & 1ull)
            dst[i / 32u] |= (1u << (i % 32u));
    }
}

/* 分配含 nfields 个槽的对象；bitmap 标明哪些槽是 GC 指针（≤64 槽）。*/
void* hao_object_new(int64_t nfields, int64_t bitmap) {
    uint64_t w0;
    if (nfields < 0) nfields = 0;
    if ((uint64_t)nfields > (UINT64_MAX / 8ULL)) {
        fputs("panic: 对象字段数过大\n", stderr);
        exit(1);
    }
    if (nfields > 64) {
        fputs("panic: hao_object_new 仅支持 ≤64 槽；更大请用 hao_object_new_map\n",
              stderr);
        exit(1);
    }
    w0 = (uint64_t)bitmap;
    if (nfields <= 32) {
        size_t bytes = (size_t)nfields * 8u;
        if (bytes < 16) bytes = 16;
        return gc_alloc_ex(bytes, GC_KIND_SLOTS, w0 & 0xffffffffu);
    }
    {
        size_t slot_bytes = (size_t)nfields * 8u;
        size_t trail = gc_bitmap_trailer_bytes((size_t)nfields);
        void* p = gc_alloc_ex(slot_bytes + trail, GC_KIND_BITMAP, (uint64_t)nfields);
        gc_write_bitmap_trailer(p, (size_t)nfields, &w0, 1);
        return p;
    }
}

void* hao_object_new_map(int64_t nfields, const uint64_t* words, int64_t nwords) {
    size_t slot_bytes, trail;
    void* p;
    if (nfields < 0) nfields = 0;
    if (nfields == 0) {
        return gc_alloc_ex(16, GC_KIND_SLOTS, 0);
    }
    if ((uint64_t)nfields > 4096u) {
        fputs("panic: 对象字段数超过 4096\n", stderr);
        exit(1);
    }
    if (nfields <= 32) {
        uint64_t bm = (words && nwords > 0) ? words[0] : 0;
        size_t bytes = (size_t)nfields * 8u;
        if (bytes < 16) bytes = 16;
        return gc_alloc_ex(bytes, GC_KIND_SLOTS, bm & 0xffffffffu);
    }
    slot_bytes = (size_t)nfields * 8u;
    trail = gc_bitmap_trailer_bytes((size_t)nfields);
    p = gc_alloc_ex(slot_bytes + trail, GC_KIND_BITMAP, (uint64_t)nfields);
    gc_write_bitmap_trailer(p, (size_t)nfields, words, (size_t)(nwords > 0 ? nwords : 0));
    return p;
}

int8_t hao_type_is(void* obj, void** allowed) {
    if (!obj || !allowed) return 0;
    void* vt = *(void**)obj;
    for (void** p = allowed; *p; ++p)
        if (*p == vt) return 1;
    return 0;
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
    while (n < 16) tmp[n++] = '0';
    if (len + n + 2 >= cap) n = cap - len - 2;
    for (i = 0; i < n; ++i) out[len++] = tmp[n - 1 - i];
    out[len++] = '>';
    out[len] = '\0';
    return len;
}

/* v0.79：Object.toString/hashCode/equals 已上移 Hao；保留 hao_fmt_* 供 panic/reflect 诊断 */
