/*
 * HaoLang 运行时 —— sync 并发原语库（v0.13.0）
 * ------------------------------------------------------------
 *  Mutex 自旋锁 + 原子整数操作，全部基于 clang/GCC 的 `__atomic`
 *  内建（跨平台，MSVC 编译也走 clang，直接可用，无系统依赖）。
 *
 *  锁/计数的存储单元是 HaoLang 的 `Int?` 装箱单元（8 字节 GC 块，
 *  值类型装箱为指针）：HaoLang 侧 `val m = new Int?` 或字段 `= 0`
 *  得到一个 8 字节 i64 单元，C 侧把它当 `int64_t*` 原子内存操作。
 *  锁状态 0=解锁 1=锁定；GC 是 non-moving，指针稳定，安全。
 *
 *  注意：`__atomic_compare_exchange_n` 的 expected 会就地更新为当前值，
 *  自旋锁每次失败后必须把 expected 重置回 0 再重试。
 */
#include "runtime_internal.h"

/* 获取互斥锁（原子自旋，忙等直到成功）。 */
void hao_sync_lock(int64_t* m) {
    int64_t expected = 0;
    while (!__atomic_compare_exchange_n(m, &expected, 1, 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        expected = 0;  /* 失败，expected 被更新为当前值(1)，重置后重试 */
        /* 自旋须 safepoint：否则软 STW 永远等不齐（禁止无 park 空转） */
        hao_gc_safepoint();
    }
}

/* 释放互斥锁。 */
void hao_sync_unlock(int64_t* m) {
    __atomic_store_n(m, 0, __ATOMIC_SEQ_CST);
}

/* 原子加，返回加后新值。 */
int64_t hao_sync_atomic_add(int64_t* p, int64_t delta) {
    return __atomic_add_fetch(p, delta, __ATOMIC_SEQ_CST);
}

/* 原子加，返回加前旧值。 */
int64_t hao_sync_atomic_fetch_add(int64_t* p, int64_t delta) {
    return __atomic_fetch_add(p, delta, __ATOMIC_SEQ_CST);
}

/* 原子交换，返回旧值。 */
int64_t hao_sync_atomic_exchange(int64_t* p, int64_t value) {
    return __atomic_exchange_n(p, value, __ATOMIC_SEQ_CST);
}

/* 比较并交换：当前值==expected 则置为 desired 并返回 1，否则返回 0。 */
int8_t hao_sync_atomic_compare_exchange(int64_t* p, int64_t expected, int64_t desired) {
    int64_t exp = expected;
    return __atomic_compare_exchange_n(p, &exp, desired, 0,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 1 : 0;
}