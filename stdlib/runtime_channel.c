/*
 * HaoLang 运行时 —— channel（缓冲 / 无缓冲会合）
 * ------------------------------------------------------------
 *  载荷统一为 i64（指针或整型位型）。队列中的堆指针挂 GC 根。
 *  句柄：NativeHandle 代理 HaoChan*（C calloc；Handle 代理释放一次）。
 *  capacity=0：无缓冲会合；>0：环形缓冲。
 *  零 SDK：Windows 用 hao_win_crit/cond；Linux 用 pthread。
 *  select：跨 chan 登记 waiter + park（hao_gc_os_block_*）；禁忙等主路径。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "runtime_internal.h"
#ifndef _WIN32
#include <pthread.h>
#include <time.h>
#endif

typedef struct HaoSelectWaiter HaoSelectWaiter;
typedef struct HaoSelectEnroll HaoSelectEnroll;

struct HaoSelectWaiter {
    volatile int32_t woken;
#ifdef _WIN32
    unsigned char mtx[HAO_WIN_CRITSEC_BYTES];
    unsigned char cv[HAO_WIN_CONDVAR_BYTES];
#else
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
#endif
};

struct HaoSelectEnroll {
    HaoSelectWaiter* w;
    HaoSelectEnroll* next;
};

typedef struct {
    int64_t* buf;
    int32_t  cap;       /* 0 = 无缓冲会合 */
    int32_t  len;
    int32_t  head;
    int32_t  closed;
    int32_t  has_slot;  /* 无缓冲：单槽有值 */
    int64_t  slot;
    HaoSelectEnroll* sel_q; /* select waiter 登记链（持 chan 锁访问） */
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

static HaoChan* chan_from(HaoNativeHandle* h) {
    return (HaoChan*)hao_handle_raw(h);
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

static void sel_signal(HaoSelectWaiter* w) {
    if (!w) return;
#ifdef _WIN32
    hao_win_crit_enter(w->mtx);
    w->woken = 1;
    hao_win_cond_wake(w->cv);
    hao_win_crit_leave(w->mtx);
#else
    pthread_mutex_lock(&w->mtx);
    w->woken = 1;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mtx);
#endif
}

/* 持 chan 锁：唤醒所有登记的 select waiter */
static void chan_wake_select(HaoChan* c) {
    for (HaoSelectEnroll* e = c->sel_q; e; e = e->next)
        sel_signal(e->w);
}

static void chan_wait_send(HaoChan* c) {
    hao_gc_os_block_arm();
#ifdef _WIN32
    hao_win_cond_wait(c->cv_send, c->mtx);
#else
    pthread_cond_wait(&c->cv_send, &c->mtx);
#endif
    hao_gc_os_block_disarm();
}

static void chan_wait_recv(HaoChan* c) {
    hao_gc_os_block_arm();
#ifdef _WIN32
    hao_win_cond_wait(c->cv_recv, c->mtx);
#else
    pthread_cond_wait(&c->cv_recv, &c->mtx);
#endif
    hao_gc_os_block_disarm();
}

static void chan_wake_send(HaoChan* c) {
#ifdef _WIN32
    hao_win_cond_wake(c->cv_send);
#else
    pthread_cond_signal(&c->cv_send);
#endif
    chan_wake_select(c);
}

static void chan_wake_recv(HaoChan* c) {
#ifdef _WIN32
    hao_win_cond_wake(c->cv_recv);
#else
    pthread_cond_signal(&c->cv_recv);
#endif
    chan_wake_select(c);
}

static void chan_wake_all(HaoChan* c) {
#ifdef _WIN32
    hao_win_cond_wake_all(c->cv_send);
    hao_win_cond_wake_all(c->cv_recv);
#else
    pthread_cond_broadcast(&c->cv_send);
    pthread_cond_broadcast(&c->cv_recv);
#endif
    chan_wake_select(c);
}

/* 仅堆指针挂根；整型位型（sendInt）不得污染根表。禁 is_heap_ptr（内含 safepoint） */
static void chan_root(int64_t bits) {
    void* p = (void*)(uintptr_t)bits;
    if (p) hao_gc_add_root_if_heap(p);
}

static void chan_unroot(int64_t bits) {
    void* p = (void*)(uintptr_t)bits;
    if (p) hao_gc_remove_root(p); /* 未挂过则无操作 */
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
    /* select 链节点属 waiter 栈/临时堆；销毁时只清空头指针（waiter 应已摘链） */
    c->sel_q = NULL;
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

/* Handle drop：代理释放 HaoChan（属 C） */
static void hao_chan_drop(void* raw) {
    chan_free_native((HaoChan*)raw);
}

/* 成功 1；h 为空、已有通道或分配失败 0。
 * 禁止对同一 Handle remake：并发 send/recv 持有旧 HaoChan* 时 free → UAF。
 * 需要新通道请 channel.make（新 Handle）。 */
int8_t hao_chan_make(HaoNativeHandle* h, int32_t capacity) {
    HaoChan* c;
    if (!h) return 0;
    if (hao_handle_is_open(h)) return 0;
    if (capacity < 0) capacity = 0;
    if (capacity > 1024 * 1024) capacity = 1024 * 1024;
    c = (HaoChan*)calloc(1, sizeof(HaoChan));
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
    hao_handle_attach(h, c, hao_chan_drop);
    return 1;
}

/* 成功 0；关闭后发送失败 1 */
int32_t hao_chan_send(HaoNativeHandle* h, int64_t bits) {
    HaoChan* c = chan_from(h);
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

int64_t hao_chan_recv(HaoNativeHandle* h) {
    HaoChan* c = chan_from(h);
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

int32_t hao_chan_try_send(HaoNativeHandle* h, int64_t bits) {
    HaoChan* c = chan_from(h);
    if (!c) return 1;
    /* 与 send 同序：先 root 再 chan_lock，禁止持锁调 hao_gc_*（ABBA） */
    chan_root(bits);
    chan_lock(c);
    if (c->closed) {
        chan_unlock(c);
        chan_unroot(bits);
        return 1;
    }
    if (c->cap == 0) {
        /* 无缓冲：槽空则可占槽成功 */
        if (c->has_slot) {
            chan_unlock(c);
            chan_unroot(bits);
            return 1;
        }
        c->slot = bits;
        c->has_slot = 1;
        chan_wake_recv(c);
        chan_unlock(c);
        return 0;
    }
    if (c->len >= c->cap) {
        chan_unlock(c);
        chan_unroot(bits);
        return 1;
    }
    chan_push_buf(c, bits);
    chan_wake_recv(c);
    chan_unlock(c);
    return 0;
}

/* 写出 *out；返回 1 成功，0 空/关闭 */
int32_t hao_chan_try_recv(HaoNativeHandle* h, int64_t* out) {
    HaoChan* c = chan_from(h);
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

void hao_chan_close(HaoNativeHandle* h) {
    HaoChan* c = chan_from(h);
    if (!c) return;
    chan_lock(c);
    if (!c->closed) {
        c->closed = 1;
        chan_wake_all(c);
    }
    chan_unlock(c);
}

/* ---- select 真等待 ---- */

static _Thread_local int32_t hao_sel_rot;

/* select recv：有数据或已关闭（零值）即就绪，避免因关闭死锁 */
static int32_t sel_try_recv(HaoNativeHandle* h, int64_t* out) {
    HaoChan* c = chan_from(h);
    if (!c || !out) return 0;
    chan_lock(c);
    if (c->cap == 0) {
        if (c->has_slot) {
            *out = c->slot;
            c->has_slot = 0;
            chan_wake_send(c);
            chan_unlock(c);
            chan_unroot(*out);
            return 1;
        }
        if (c->closed) {
            *out = 0;
            chan_unlock(c);
            return 1;
        }
        chan_unlock(c);
        return 0;
    }
    int64_t v = 0;
    if (chan_pop_buf(c, &v)) {
        *out = v;
        chan_wake_send(c);
        chan_unlock(c);
        chan_unroot(v);
        return 1;
    }
    if (c->closed) {
        *out = 0;
        chan_unlock(c);
        return 1;
    }
    chan_unlock(c);
    return 0;
}

static int sel_try_one(HaoChanSelectOp* op) {
    if (!op || !op->handle) return 0;
    if (op->mode == 0) {
        /* recv */
        if (!op->out) return 0;
        return sel_try_recv((HaoNativeHandle*)op->handle, op->out) != 0;
    }
    /* send：0 成功 */
    return hao_chan_try_send((HaoNativeHandle*)op->handle, op->bits) == 0;
}

static void sel_enroll(HaoChan* c, HaoSelectEnroll* node) {
    /* 持 c 锁 */
    node->next = c->sel_q;
    c->sel_q = node;
}

static void sel_unenroll(HaoChan* c, HaoSelectWaiter* w) {
    /* 持 c 锁 */
    HaoSelectEnroll** pp = &c->sel_q;
    while (*pp) {
        if ((*pp)->w == w) {
            HaoSelectEnroll* dead = *pp;
            *pp = dead->next;
            dead->next = NULL;
            /* node 属调用方栈，不 free */
            return;
        }
        pp = &(*pp)->next;
    }
}

static void sel_waiter_init(HaoSelectWaiter* w) {
    w->woken = 0;
#ifdef _WIN32
    hao_win_crit_init(w->mtx);
    hao_win_cond_init(w->cv);
#else
    pthread_mutex_init(&w->mtx, NULL);
    pthread_cond_init(&w->cv, NULL);
#endif
}

static void sel_waiter_destroy(HaoSelectWaiter* w) {
#ifdef _WIN32
    (void)w;
#else
    pthread_mutex_destroy(&w->mtx);
    pthread_cond_destroy(&w->cv);
#endif
}

static void sel_park(HaoSelectWaiter* w) {
#ifdef _WIN32
    hao_win_crit_enter(w->mtx);
    while (!w->woken) {
        hao_gc_os_block_arm();
        hao_win_cond_wait(w->cv, w->mtx);
        hao_gc_os_block_disarm();
    }
    w->woken = 0;
    hao_win_crit_leave(w->mtx);
#else
    pthread_mutex_lock(&w->mtx);
    while (!w->woken) {
        hao_gc_os_block_arm();
        pthread_cond_wait(&w->cv, &w->mtx);
        hao_gc_os_block_disarm();
    }
    w->woken = 0;
    pthread_mutex_unlock(&w->mtx);
#endif
}

/*
 * 多路 select：返回选中 case 下标；has_default 且无人就绪返回 -1。
 * 无 default：登记各 chan 后 park（os_block），对端 send/recv/close 唤醒。
 */
int32_t hao_chan_select(HaoChanSelectOp* ops, int32_t nop, int32_t has_default) {
    if (!ops || nop <= 0) return has_default ? -1 : 0;
    int32_t start = hao_sel_rot++;
    if (start < 0) start = 0;
    start %= nop;

    for (;;) {
        for (int32_t k = 0; k < nop; ++k) {
            int32_t i = (start + k) % nop;
            if (sel_try_one(&ops[i])) return i;
        }
        if (has_default) return -1;

        HaoSelectWaiter waiter;
        sel_waiter_init(&waiter);
        /* 栈上 enroll 节点：与 ops 一一对应 */
        HaoSelectEnroll* nodes =
            (HaoSelectEnroll*)calloc((size_t)nop, sizeof(HaoSelectEnroll));
        if (!nodes) {
            sel_waiter_destroy(&waiter);
            /* OOM：短暂让出后重试（非忙等主路径；极端降级） */
            hao_gc_os_block_arm();
#ifdef _WIN32
            hao_win_sleep_ms(1);
#else
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 1000000L;
            nanosleep(&ts, NULL);
#endif
            hao_gc_os_block_disarm();
            continue;
        }

        for (int32_t i = 0; i < nop; ++i) {
            HaoChan* c = chan_from((HaoNativeHandle*)ops[i].handle);
            if (!c) continue;
            nodes[i].w = &waiter;
            nodes[i].next = NULL;
            chan_lock(c);
            sel_enroll(c, &nodes[i]);
            chan_unlock(c);
        }

        /* 登记后再试一轮，防 try→enroll 窗口丢唤醒 */
        int32_t picked = -1;
        for (int32_t k = 0; k < nop; ++k) {
            int32_t i = (start + k) % nop;
            if (sel_try_one(&ops[i])) {
                picked = i;
                break;
            }
        }

        if (picked < 0 && !waiter.woken)
            sel_park(&waiter);

        for (int32_t i = 0; i < nop; ++i) {
            HaoChan* c = chan_from((HaoNativeHandle*)ops[i].handle);
            if (!c) continue;
            chan_lock(c);
            sel_unenroll(c, &waiter);
            chan_unlock(c);
        }
        free(nodes);
        sel_waiter_destroy(&waiter);

        if (picked >= 0) return picked;
        /* 假醒 / 竞态：重新 try → 再登记 */
    }
}
