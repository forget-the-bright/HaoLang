/*
 * HaoLang 运行时 —— 数组
 * ------------------------------------------------------------
 *  布局：[pad|cap|len|esz|elem×esz]，返回指针指向元素区（16 对齐）。
 *  esz ∈ {1,2,4,8}；is_ptr 标明元素是否为 GC 指针（精确堆扫 / 写屏障）。
 */
#include "runtime_internal.h"

void* hao_array_new(int64_t len, int64_t esz, int64_t is_ptr) {
    if (len < 0) {
        hao_panic_msg("数组长度不能为负数");
    }
    if (esz != 1 && esz != 2 && esz != 4 && esz != 8) {
        hao_panic_msg("不支持的数组元素宽度");
    }
    if ((uint64_t)len > (UINT64_MAX - (uint64_t)HAO_ARR_HEADER) / (uint64_t)esz) {
        hao_panic_msg("数组过大");
    }
    int64_t cap = len > 0 ? len : 1;
    int64_t ptr_flag = (is_ptr && esz == 8) ? 1 : 0;
    uint64_t meta = ptr_flag ? 1ULL : 0ULL;
    char* base = (char*)gc_alloc_ex(
        (size_t)HAO_ARR_HEADER + (size_t)cap * (size_t)esz,
        GC_KIND_ARRAY, meta);
    /* pad bit1 = is_ptr（扩容时复制）；bit0 保持 0 */
    *(int64_t*)(base + 0)                    = ptr_flag ? 2 : 0;
    *(int64_t*)(base + HAO_ARR_CAP_OFF_BASE) = cap;
    *(int64_t*)(base + HAO_ARR_LEN_OFF_BASE) = len;
    *(int64_t*)(base + HAO_ARR_ESZ_OFF_BASE) = esz;
    return base + HAO_ARR_HEADER;
}

/* 从 C 常量串造 [Byte]（仅 IR 字面量；长度=strlen，无尾 NUL 进 len） */
void* hao_array_from_cstr(const char* c) {
    if (!c) c = "";
    size_t n = strlen(c);
    if (n > (size_t)INT32_MAX) {
        hao_panic_msg("C 字符串过长无法造 [Byte]");
    }
    void* arr = hao_array_new((int64_t)n, 1, 0);
    if (n > 0 && arr) memcpy(arr, c, n);
    return arr;
}

int64_t hao_array_len(void* arr) {
    if (!arr) return 0;
    return *(int64_t*)((char*)arr - HAO_ARR_LEN_OFF);
}

int64_t hao_array_cap(void* arr) {
    if (!arr) return 0;
    return *(int64_t*)((char*)arr - HAO_ARR_CAP_OFF);
}

static int64_t hao_array_esz(void* arr) {
    return *(int64_t*)((char*)arr - HAO_ARR_ESZ_OFF);
}

int64_t hao_array_check(void* arr, int64_t idx) {
    int64_t len = hao_array_len(arr);
    if (idx < 0 || idx >= len) hao_panic_index(idx, len);
    return idx;
}

/* 指针数组元素 → 托管引用（对标 Java 数组元素是 Object 引用；由 Hao 挂根） */
void* hao_array_get_obj(void* arr, int64_t idx) {
    if (!arr) return NULL;
    hao_array_check(arr, idx);
    if (hao_array_esz(arr) != 8) {
        hao_panic_msg("hao_array_get_obj 仅支持指针宽元素");
    }
    return *(void**)((char*)arr + (size_t)idx * 8);
}

void* hao_array_clone(void* arr) {
    if (!arr) return NULL;
    int64_t len = hao_array_len(arr);
    int64_t esz = hao_array_esz(arr);
    int64_t pad = *(int64_t*)((char*)arr - HAO_ARR_HEADER);
    int is_ptr = (pad & 2) ? 1 : 0;
    void* dst = hao_array_new(len, esz, is_ptr);
    if (len <= 0) return dst;
    char* oldbase = (char*)arr - HAO_ARR_HEADER;
    char* newbase = (char*)dst - HAO_ARR_HEADER;
    size_t nbytes = (size_t)HAO_ARR_HEADER + (size_t)len * (size_t)esz;
    hao_gc_array_copy_and_shade(newbase, oldbase, nbytes, len, is_ptr);
    int64_t newcap = len > 0 ? len : 1;
    *(int64_t*)(newbase + 0) = is_ptr ? 2 : 0;
    *(int64_t*)(newbase + HAO_ARR_CAP_OFF_BASE) = newcap;
    *(int64_t*)(newbase + HAO_ARR_LEN_OFF_BASE) = len;
    *(int64_t*)(newbase + HAO_ARR_ESZ_OFF_BASE) = esz;
    return dst;
}

/* v0.77：对标 System.arraycopy / Array.Copy（永久 C 底座；非业务） */
void hao_arraycopy(void* dst, int64_t dstPos, void* src, int64_t srcPos,
                   int64_t n) {
    if (n == 0) return;
    if (!dst || !src) {
        hao_panic_msg("arraycopy null 数组");
    }
    if (n < 0 || dstPos < 0 || srcPos < 0) {
        hao_panic_msg("arraycopy 下标/长度非法");
    }
    int64_t dlen = hao_array_len(dst);
    int64_t slen = hao_array_len(src);
    int64_t desz = hao_array_esz(dst);
    int64_t sesz = hao_array_esz(src);
    if (desz != sesz) {
        hao_panic_msg("arraycopy 元素宽度不一致");
    }
    if (dstPos > dlen - n || srcPos > slen - n) {
        hao_panic_msg("arraycopy 越界");
    }
    char* dbase = (char*)dst - HAO_ARR_HEADER;
    char* sbase = (char*)src - HAO_ARR_HEADER;
    int64_t is_ptr = (*(int64_t*)(dbase + 0) >> 1) & 1;
    int64_t src_ptr = (*(int64_t*)(sbase + 0) >> 1) & 1;
    if (is_ptr != src_ptr) {
        hao_panic_msg("arraycopy is_ptr 不一致");
    }
    hao_gc_add_root(dst);
    hao_gc_add_root(src);
    size_t bytes = (size_t)n * (size_t)desz;
    char* d = (char*)dst + (size_t)dstPos * (size_t)desz;
    char* s = (char*)src + (size_t)srcPos * (size_t)sesz;
    if (is_ptr && desz == 8) {
        /* 逐槽屏障（可重叠：先拷到临时或按方向） */
        if (d == s) {
            /* no-op */
        } else if (d > s && d < s + bytes) {
            int64_t i = n - 1;
            while (i >= 0) {
                void* v = *(void**)(s + (size_t)i * 8);
                hao_gc_barrier(d + (size_t)i * 8, v);
                *(void**)(d + (size_t)i * 8) = v;
                i -= 1;
            }
        } else {
            int64_t i = 0;
            while (i < n) {
                void* v = *(void**)(s + (size_t)i * 8);
                hao_gc_barrier(d + (size_t)i * 8, v);
                *(void**)(d + (size_t)i * 8) = v;
                i += 1;
            }
        }
    } else {
        memmove(d, s, bytes);
    }
    hao_gc_remove_root(src);
    hao_gc_remove_root(dst);
}
