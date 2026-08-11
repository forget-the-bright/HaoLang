/*
 * HaoLang 运行时 —— sync 并发原语库
 * ------------------------------------------------------------
 *  原子整数操作基于 clang/GCC `__atomic`（无 Hao 原子 IR）。
 *  存储单元：NativeHandle → malloc(8) 胞（hao_sync_cell_new）。
 *  Mutex.lock/unlock 已上移 Hao CAS + safepoint；本文件不再提供 spin lock。
 */
#include "runtime_internal.h"

static int64_t* sync_cell(HaoNativeHandle* h) {
    int64_t* p = (int64_t*)hao_handle_raw(h);
    if (!p) {
        fputs("panic: sync atomic on closed/empty handle\n", stderr);
        abort();
    }
    return p;
}

/* 原子加，返回加后新值。 */
int64_t hao_sync_atomic_add(HaoNativeHandle* h, int64_t delta) {
    return __atomic_add_fetch(sync_cell(h), delta, __ATOMIC_SEQ_CST);
}

/* 原子加，返回加前旧值。 */
int64_t hao_sync_atomic_fetch_add(HaoNativeHandle* h, int64_t delta) {
    return __atomic_fetch_add(sync_cell(h), delta, __ATOMIC_SEQ_CST);
}

/* 原子交换，返回旧值。 */
int64_t hao_sync_atomic_exchange(HaoNativeHandle* h, int64_t value) {
    return __atomic_exchange_n(sync_cell(h), value, __ATOMIC_SEQ_CST);
}

/* 比较并交换：当前值==expected 则置为 desired 并返回 1，否则返回 0。 */
int8_t hao_sync_atomic_compare_exchange(HaoNativeHandle* h, int64_t expected,
                                        int64_t desired) {
    int64_t exp = expected;
    return __atomic_compare_exchange_n(sync_cell(h), &exp, desired, 0,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
               ? 1
               : 0;
}
