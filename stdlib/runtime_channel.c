/*
 * HaoLang 运行时 —— channel（缓冲 / 无缓冲会合）
 * ------------------------------------------------------------
 *  载荷统一为 i64（指针或整型位型）。队列中的堆指针挂 GC 根。
 *  句柄存于 Long? 盒（对标 net/regex）：GC 回收盒时 finalizer
 *  排空未读载荷并释放原生 HaoChan，避免根泄漏。
 *  零 SDK：Windows 用 hao_win_crit/cond；Linux 用 pthread。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "runtime_internal.h"

#ifndef _WIN32
#include <pthread.h>
#endif

typedef struct {
    int64_t* buf;
    int32_t  cap;       /* 0 = 无缓冲会合 */
    int32_t  len;
    int32_t  head;
    int32_t  closed;
    int32_t  has_slot;  /* 无缓冲：单槽有值（try_send 可先占槽，供 select 轮询） */
    int64_t  slot;
#ifdef _WIN32
    unsigned char mtx[HAO_WIN_CRITSEC_BYTES];
    unsigned char cv_send[HAO_WIN_CONDVAR_BYTES];
    unsigned char cv_recv[HAO_WIN_CONDVAR_BYTES];
#else
    pthread_mutex_t mtx;
    pthread_cond_t  cv_send;
    pthread_cond_t  cv_recv;
#endif
} HaoChan;

static HaoChan* chan_load(const int64_t* unit) {
    if (!unit || !*unit) return NULL;
    return (HaoChan*)(intptr_t)(*unit);
}
static void chan_store(int64_t* unit, HaoChan* c) {
    if (unit) *unit = (int64_t)(intptr_t)c;
}

static void chan_lock(HaoChan* c) {
#ifdef _WIN32
    hao_win_crit_enter(c->mtx);
#else
    pthread_mutex_lock(&c->mtx);
#endif
}
static void chan_unlock(HaoChan* c) {
#ifdef _WIN32
    hao_win_crit_leave(c->mtx);
#else
    pthread_mutex_unlock(&c->mtx);
#endif
}
static void chan_wait_send(HaoChan* c) {
#ifdef _WIN32
    hao_win_cond_wait(c->cv_send, c->mtx);
#else
    pthread_cond_wait(&c->cv_send, &c->mtx);
#endif
}
static void chan_wait_recv(HaoChan* c) {
#ifdef _WIN32
    hao_win_cond_wait(c->cv_recv, c->mtx);
#else
    pthread_cond_wait(&c->cv_recv, &c->mtx);
#endif
}
static void chan_wake_send(HaoChan* c) {
#ifdef _WIN32
    hao_win_cond_wake(c->cv_send);
#else
    pthread_cond_signal(&c->cv_send);
#endif
}
static void chan_wake_recv(HaoChan* c) {
#ifdef _WIN32
    hao_win_cond_wake(c->cv_recv);
#else
    pthread_cond_signal(&c->cv_recv);
#endif
}
static void chan_wake_all(HaoChan* c) {
#ifdef _WIN32
    hao_win_cond_wake_all(c->cv_send);
    hao_win_cond_wake_all(c->cv_recv);
#else
    pthread_cond_broadcast(&c->cv_send);
    pthread_cond_broadcast(&c->cv_recv);
#endif
}

/* 仅堆指针挂根；整型位型（sendInt）不得污染根表 */
static void chan_root(int64_t bits) {
    void* p = (void*)(uintptr_t)bits;
    if (p && hao_gc_is_heap_ptr(p)) hao_gc_add_root(p);
}
static void chan_unroot(int64_t bits) {
    void* p = (void*)(uintptr_t)bits;
    if (p && hao_gc_is_heap_ptr(p)) hao_gc_remove_root(p);
}

static int chan_push_buf(HaoChan* c, int64_t v) {
    if (c->len >= c->cap) return 0;
    int32_t i = (c->head + c->len) % c->cap;
    c->buf[i] = v;
    c->len += 1;
    return 1;
}

static int chan_pop_buf(HaoChan* c, int64_t* out) {
    if (c->len <= 0) return 0;
    *out = c->buf[c->head];
    c->head = (c->head + 1) % c->cap;
    c->len -= 1;
    return 1;
}

/*
 * 销毁原生通道：先持锁关闭并摘出未读载荷，再放锁撤根。
 * （禁止持 chan 锁调 hao_gc_*：send 路径是 root→chan_lock，会 ABBA 死锁）
 */
static void chan_free_native(HaoChan* c) {
    if (!c) return;
    int64_t* pending = NULL;
    int32_t npend = 0;
    chan_lock(c);
    c->closed = 1;
    chan_wake_all(c);
    if (c->cap == 0) {
        if (c->has_slot) {
            pending = (int64_t*)malloc(sizeof(int64_t));
            if (pending) { pending[0] = c->slot; npend = 1; }
            else chan_unroot(c->slot); /* 极端：锁外撤不了则就地撤（仅 OOM） */
            c->slot = 0;
            c->has_slot = 0;
        }
    } else if (c->len > 0) {
        pending = (int64_t*)malloc((size_t)c->len * sizeof(int64_t));
        if (pending) {
            while (c->len > 0) {
                int64_t v = 0;
                if (!chan_pop_buf(c, &v)) break;
                pending[npend++] = v;
            }
        } else {
            while (c->len > 0) {
                int64_t v = 0;
                if (!chan_pop_buf(c, &v)) break;
                chan_unroot(v);
            }
        }
    }
    chan_unlock(c);
    for (int32_t i = 0; i < npend; ++i) chan_unroot(pending[i]);
    free(pending);
#ifdef _WIN32
    /* Win CRITICAL_SECTION / CONDITION_VARIABLE 无强制 destroy API */
#else
    pthread_mutex_destroy(&c->mtx);
    pthread_cond_destroy(&c->cv_send);
    pthread_cond_destroy(&c->cv_recv);
#endif
    free(c->buf);
    free(c);
}

/* 清扫回调：块已摘链；撤未读根并释放原生通道 */
static void hao_chan_unit_finalize(void* user) {
    int64_t* unit = (int64_t*)user;
    if (!unit) return;
    HaoChan* c = chan_load(unit);
    *unit = 0;
    chan_free_native(c);
}

/* 成功 1；unit 为空、已有通道或分配失败 0。
 * 禁止对同一 Long? 盒 remake：并发 send/recv 持有旧 HaoChan* 时 free → UAF。
 * 需要新通道请 new Channel / channel.make（新盒）。 */
int8_t hao_chan_make(int64_t* unit, int32_t capacity) {
    if (!unit) return 0;
    if (chan_load(unit)) return 0;
    if (capacity < 0) capacity = 0;
    if (capacity > 1024 * 1024) capacity = 1024 * 1024;
    HaoChan* c = (HaoChan*)calloc(1, sizeof(HaoChan));
    if (!c) return 0;
    c->cap = capacity;
    if (capacity > 0) {
        c->buf = (int64_t*)calloc((size_t)capacity, sizeof(int64_t));
        if (!c->buf) { free(c); return 0; }
    }
#ifdef _WIN32
    hao_win_crit_init(c->mtx);
    hao_win_cond_init(c->cv_send);
    hao_win_cond_init(c->cv_recv);
#else
    pthread_mutex_init(&c->mtx, NULL);
    pthread_cond_init(&c->cv_send, NULL);
    pthread_cond_init(&c->cv_recv, NULL);
#endif
    chan_store(unit, c);
    hao_gc_set_finalizer(unit, hao_chan_unit_finalize);
    return 1;
}

/* 成功 0；关闭后发送失败 1 */
int32_t hao_chan_send(int64_t* unit, int64_t bits) {
    HaoChan* c = chan_load(unit);
    if (!c) return 1;
    chan_root(bits);
    chan_lock(c);
    if (c->closed) {
        chan_unlock(c);
        chan_unroot(bits);
        return 1;
    }
    if (c->cap == 0) {
        while (c->has_slot && !c->closed)
            chan_wait_send(c);
        if (c->closed) {
            chan_unlock(c);
            chan_unroot(bits);
            return 1;
        }
        c->slot = bits;
        c->has_slot = 1;
        chan_wake_recv(c);
        while (c->has_slot && !c->closed)
            chan_wait_send(c);
        chan_unlock(c);
        return 0;
    }
    while (c->len >= c->cap && !c->closed)
        chan_wait_send(c);
    if (c->closed) {
        chan_unlock(c);
        chan_unroot(bits);
        return 1;
    }
    chan_push_buf(c, bits);
    chan_wake_recv(c);
    chan_unlock(c);
    return 0;
}

int64_t hao_chan_recv(int64_t* unit) {
    HaoChan* c = chan_load(unit);
    if (!c) return 0;
    chan_lock(c);
    if (c->cap == 0) {
        while (!c->has_slot && !c->closed)
            chan_wait_recv(c);
        if (!c->has_slot) {
            chan_unlock(c);
            return 0;
        }
        int64_t v = c->slot;
        c->has_slot = 0;
        chan_wake_send(c);
        chan_unlock(c);
        chan_unroot(v);
        return v;
    }
    while (c->len <= 0 && !c->closed)
        chan_wait_recv(c);
    int64_t v = 0;
    if (!chan_pop_buf(c, &v)) {
        chan_unlock(c);
        return 0;
    }
    chan_wake_send(c);
    chan_unlock(c);
    chan_unroot(v);
    return v;
}

int32_t hao_chan_try_send(int64_t* unit, int64_t bits) {
    HaoChan* c = chan_load(unit);
    if (!c) return 1;
    chan_lock(c);
    if (c->closed) { chan_unlock(c); return 1; }
    if (c->cap == 0) {
        /* 无缓冲：槽空则可占槽成功（供 select try_* 轮询；真多路 wait 见路线图） */
        if (c->has_slot) { chan_unlock(c); return 1; }
        chan_root(bits);
        c->slot = bits;
        c->has_slot = 1;
        chan_wake_recv(c);
        chan_unlock(c);
        return 0;
    }
    if (c->len >= c->cap) { chan_unlock(c); return 1; }
    chan_root(bits);
    chan_push_buf(c, bits);
    chan_wake_recv(c);
    chan_unlock(c);
    return 0;
}

/* 写出 *out；返回 1 成功，0 空/关闭 */
int32_t hao_chan_try_recv(int64_t* unit, int64_t* out) {
    HaoChan* c = chan_load(unit);
    if (!c || !out) return 0;
    chan_lock(c);
    if (c->cap == 0) {
        if (!c->has_slot) { chan_unlock(c); return 0; }
        *out = c->slot;
        c->has_slot = 0;
        chan_wake_send(c);
        chan_unlock(c);
        chan_unroot(*out);
        return 1;
    }
    int64_t v = 0;
    if (!chan_pop_buf(c, &v)) { chan_unlock(c); return 0; }
    *out = v;
    chan_wake_send(c);
    chan_unlock(c);
    chan_unroot(v);
    return 1;
}

void hao_chan_close(int64_t* unit) {
    HaoChan* c = chan_load(unit);
    if (!c) return;
    chan_lock(c);
    if (!c->closed) {
        c->closed = 1;
        chan_wake_all(c);
    }
    chan_unlock(c);
}
