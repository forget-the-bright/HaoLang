/*
 * HaoLang 运行时 —— 数组
 * ------------------------------------------------------------
 *  布局：[pad|cap|len|esz|elem×esz]，返回指针指向元素区（16 对齐）。
 *  esz ∈ {1,2,4,8}；is_ptr 标明元素是否为 GC 指针（精确堆扫 / 写屏障）。
 */
#include "runtime_internal.h"

void* hao_array_new(int64_t len, int64_t esz, int64_t is_ptr) {
    if (len < 0) {
        fprintf(stderr, "panic: 数组长度不能为负数: %lld\n", (long long)len);
        exit(1);
    }
    if (esz != 1 && esz != 2 && esz != 4 && esz != 8) {
        fprintf(stderr, "panic: 不支持的数组元素宽度: %lld\n", (long long)esz);
        exit(1);
    }
    if ((uint64_t)len > (UINT64_MAX - (uint64_t)HAO_ARR_HEADER) / (uint64_t)esz) {
        fprintf(stderr, "panic: 数组过大: len=%lld esz=%lld\n",
                (long long)len, (long long)esz);
        exit(1);
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

/* 取指针数组元素（esz=8）为 i64；越界 panic。供 json/Map.keys 等（v0.49） */
int64_t hao_array_get_ptr(void* arr, int64_t idx) {
    if (!arr) return 0;
    hao_array_check(arr, idx);
    int64_t esz = hao_array_esz(arr);
    if (esz != 8) {
        fputs("panic: hao_array_get_ptr 仅支持指针宽元素\n", stderr);
        exit(1);
    }
    return (int64_t)(intptr_t)(*(void**)((char*)arr + (size_t)idx * 8));
}

void* hao_array_push(void* arr, int64_t val) {
    if (!arr) {
        fputs("panic: 对 null 数组执行 push\n", stderr);
        exit(1);
    }
    int64_t len = hao_array_len(arr);
    int64_t cap = hao_array_cap(arr);
    int64_t esz = hao_array_esz(arr);

    char* oldbase = (char*)arr - HAO_ARR_HEADER;
    int64_t is_ptr = (*(int64_t*)(oldbase + 0) >> 1) & 1;

    if (len >= cap) {
        uint64_t nc = cap ? (uint64_t)cap * 2ULL : 4ULL;
        if (nc < (uint64_t)len + 1ULL) nc = (uint64_t)len + 1ULL;
        if (esz <= 0 ||
            nc > (uint64_t)INT64_MAX ||
            nc > (UINT64_MAX - (uint64_t)HAO_ARR_HEADER) / (uint64_t)esz) {
            fputs("panic: 数组扩容过大\n", stderr);
            exit(1);
        }
        int64_t newcap = (int64_t)nc;
        size_t oldBytes = (size_t)HAO_ARR_HEADER + (size_t)cap * (size_t)esz;
        uint64_t meta = is_ptr ? 1ULL : 0ULL;
        char* newbase = (char*)gc_alloc_ex(
            (size_t)HAO_ARR_HEADER + (size_t)newcap * (size_t)esz,
            GC_KIND_ARRAY, meta);
        /* v0.53.2：memcpy+shade 原子（持 GC 锁、中间无 safepoint） */
        hao_gc_array_copy_and_shade(newbase, oldbase, oldBytes, len,
                                    is_ptr && esz == 8 ? 1 : 0);
        arr = newbase + HAO_ARR_HEADER;
        *(int64_t*)((char*)arr - HAO_ARR_CAP_OFF) = newcap;
    }

    char* slot = (char*)arr + (size_t)len * (size_t)esz;
    if (esz == 1) {
        *(uint8_t*)slot = (uint8_t)(val & 0xFF);
    } else if (esz == 2) {
        *(int16_t*)slot = (int16_t)val;
    } else if (esz == 4) {
        *(int32_t*)slot = (int32_t)val;
    } else {
        /* v0.53：与 IR 一致，先 shade 再 publish */
        if (is_ptr)
            hao_gc_barrier(arr, (void*)(uintptr_t)val);
        *(int64_t*)slot = val;
    }
    *(int64_t*)((char*)arr - HAO_ARR_LEN_OFF) = len + 1;
    return arr;
}

int64_t hao_array_pop(void* arr) {
    if (!arr) {
        fputs("panic: 对 null 数组执行 pop\n", stderr);
        exit(1);
    }
    int64_t len = hao_array_len(arr);
    if (len <= 0) {
        fputs("panic: 对空数组执行 pop\n", stderr);
        exit(1);
    }
    int64_t esz = hao_array_esz(arr);
    char* slot = (char*)arr + (size_t)(len - 1) * (size_t)esz;
    int64_t val;
    if (esz == 1) {
        val = (int64_t)*(uint8_t*)slot;
    } else if (esz == 2) {
        val = (int64_t)*(int16_t*)slot;
    } else if (esz == 4) {
        val = (int64_t)*(int32_t*)slot;
    } else {
        val = *(int64_t*)slot;
    }
    *(int64_t*)((char*)arr - HAO_ARR_LEN_OFF) = len - 1;
    return val;
}
