/*
 * HaoLang 运行时 —— 线程（v0.19.0；v0.21.1 Windows 改 dynload）
 * ------------------------------------------------------------
 *  线程：创建/join/睡眠。线程运行一个 HaoLang 闭包（env 对象，
 *  槽 0 为实现函数指针，签名 ret(ptr env)），线程入口取出 fnptr 调用。
 *  线程池：固定 N 个 worker + 任务队列（互斥锁 + 条件变量），
 *  submit 把任务闭包入队，worker 出队执行。
 *
 *  GC：排队/待启动的闭包须 hao_gc_add_root；worker 在两次任务之间
 *  保持线程注册（勿每任务 unregister，否则 STW 漏扫）。
 *
 *  跨平台：
 *    - Windows：hao_win_*（kernel32.dll 惰性解析，5.12）
 *    - Linux：  pthread_create / pthread_join / nanosleep /
 *                pthread_mutex / pthread_cond
 *  线程句柄统一以 int64_t 存（HANDLE 或 pthread_t 都是 8 字节）。
 */
#include "runtime_internal.h"

#include <time.h>

#ifndef _WIN32
#include <pthread.h>
#include <sched.h>
#endif

/* ============================================================
 *  睡眠
 * ============================================================ */
void hao_thread_sleep_ms(int32_t ms) {
    if (ms <= 0) return;
#ifdef _WIN32
    hao_win_sleep_ms((uint32_t)ms);
#else
    struct timespec req;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&req, NULL);
#endif
}

void hao_thread_sleep_ns(int64_t ns) {
    if (ns <= 0) return;
#ifdef _WIN32
    hao_win_sleep_ms((uint32_t)(ns / 1000000));
#else
    struct timespec req;
    req.tv_sec = ns / 1000000000L;
    req.tv_nsec = ns % 1000000000L;
    nanosleep(&req, NULL);
#endif
}

/* ============================================================
 *  线程
 * ============================================================ */
/* 仅调用闭包；注册/根由调用方管理 */
static void invoke_closure(void* env) {
    if (!env) return;
    void** e = (void**)env;
    void (*fn)(void*) = (void(*)(void*))e[0];
    if (fn) fn(env);
}

/* 独立线程入口：已由 start 加根；此处注册线程、执行、去根并注销 */
static void run_closure_thread(void* env) {
    gc_register_thread();
    invoke_closure(env);
    if (env) hao_gc_remove_root(env);
    gc_unregister_thread();
}

#ifdef _WIN32
static uint32_t thread_entry(void* arg) { run_closure_thread(arg); return 0; }
#else
static void* thread_entry(void* arg) { run_closure_thread(arg); return NULL; }
#endif

int64_t hao_thread_start(void* env) {
    /* 在子线程跑起来之前，闭包只活在 Task/参数里，须挂外部根 */
    if (env) hao_gc_add_root(env);
#ifdef _WIN32
    void* h = hao_win_create_thread(thread_entry, env);
    if (!h) {
        if (env) hao_gc_remove_root(env);
        return 0;
    }
    return (int64_t)(intptr_t)h;
#else
    pthread_t t;
    if (pthread_create(&t, NULL, thread_entry, env) != 0) {
        if (env) hao_gc_remove_root(env);
        return 0;
    }
    return (int64_t)t;
#endif
}

void hao_thread_join(int64_t handle) {
    if (!handle) return;
#ifdef _WIN32
    hao_win_join_close((void*)(intptr_t)handle);
#else
    pthread_t t = (pthread_t)handle;
    pthread_join(t, NULL);
#endif
}

void hao_thread_yield(void) {
#ifdef _WIN32
    hao_win_switch_to_thread();
#else
    sched_yield();
#endif
}

/* ============================================================
 *  线程池
 * ============================================================ */
typedef struct Task {
    void* env;
    struct Task* next;
} Task;

typedef struct {
    Task* head;
    Task* tail;
    int shutdown;
    int nworkers;          /* 已成功创建的 worker 数 */
    int joined;            /* shutdown 已 join 过，防二次 join */
#ifdef _WIN32
    unsigned char mtx[HAO_WIN_CRITSEC_BYTES];
    unsigned char cv[HAO_WIN_CONDVAR_BYTES];
    void** threads;        /* HANDLE 数组，供 join；创建失败勿 free 池直至 join */
#else
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
    pthread_t* threads;
#endif
} Pool;

static void pool_drain_tasks(Pool* p) {
    Task* t = p->head;
    p->head = p->tail = NULL;
    while (t) {
        Task* n = t->next;
        if (t->env) hao_gc_remove_root(t->env);
        free(t);
        t = n;
    }
}

static void pool_join_workers(Pool* p) {
    if (!p || p->joined) return;
    for (int i = 0; i < p->nworkers; ++i) {
#ifdef _WIN32
        if (p->threads && p->threads[i])
            hao_win_join_close(p->threads[i]);
#else
        if (p->threads)
            pthread_join(p->threads[i], NULL);
#endif
    }
    p->joined = 1;
}

#ifdef _WIN32
static uint32_t pool_worker(void* arg) {
#else
static void* pool_worker(void* arg) {
#endif
    Pool* p = (Pool*)arg;
    gc_register_thread(); /* 整个 worker 生命周期保持注册，勿每任务注销 */
    for (;;) {
        Task* t = NULL;
        int shutdown = 0;
#ifdef _WIN32
        hao_win_crit_enter(p->mtx);
        while (!p->head && !p->shutdown)
            hao_win_cond_wait(p->cv, p->mtx);
        if (p->head) { t = p->head; p->head = t->next; if (!p->head) p->tail = NULL; }
        shutdown = p->shutdown;
        hao_win_crit_leave(p->mtx);
#else
        pthread_mutex_lock(&p->mtx);
        while (!p->head && !p->shutdown)
            pthread_cond_wait(&p->cv, &p->mtx);
        if (p->head) { t = p->head; p->head = t->next; if (!p->head) p->tail = NULL; }
        shutdown = p->shutdown;
        pthread_mutex_unlock(&p->mtx);
#endif
        if (!t) { if (shutdown) break; else continue; }
        void* env = t->env;
        free(t);
        invoke_closure(env);
        if (env) hao_gc_remove_root(env); /* submit 时加的根 */
    }
    gc_unregister_thread();
    return 0;
}

int64_t hao_pool_new(int32_t n) {
    if (n < 1) n = 1;
    if (n > 256) n = 256; /* 防异常巨大 n 拖垮进程 */
    Pool* p = (Pool*)calloc(1, sizeof(Pool));
    if (!p) return 0;
#ifdef _WIN32
    p->threads = (void**)calloc((size_t)n, sizeof(void*));
#else
    p->threads = (pthread_t*)calloc((size_t)n, sizeof(pthread_t));
#endif
    if (!p->threads) { free(p); return 0; }
#ifdef _WIN32
    hao_win_crit_init(p->mtx);
    hao_win_cond_init(p->cv);
#else
    pthread_mutex_init(&p->mtx, NULL);
    pthread_cond_init(&p->cv, NULL);
#endif
    int ok = 1;
    for (int i = 0; i < n; ++i) {
#ifdef _WIN32
        void* h = hao_win_create_thread(pool_worker, p);
        if (!h) { ok = 0; break; }
        p->threads[i] = h;
#else
        if (pthread_create(&p->threads[i], NULL, pool_worker, p) != 0) {
            ok = 0;
            break;
        }
#endif
        p->nworkers = i + 1;
    }
    if (!ok) {
        /* 已跑的 worker 仍持 Pool*：须 shutdown+join 后再 free，否则 UAF */
        p->shutdown = 1;
#ifdef _WIN32
        hao_win_cond_wake_all(p->cv);
#else
        pthread_cond_broadcast(&p->cv);
#endif
        pool_join_workers(p);
        pool_drain_tasks(p);
#ifdef _WIN32
        /* mtx/cv 无显式 destroy（dynload CRITICAL_SECTION 由进程回收） */
#else
        pthread_mutex_destroy(&p->mtx);
        pthread_cond_destroy(&p->cv);
#endif
        free(p->threads);
        free(p);
        return 0;
    }
    return (int64_t)(intptr_t)p;
}

int32_t hao_pool_submit(int64_t pool, void* env) {
    Pool* p = (Pool*)(intptr_t)pool;
    if (!p) return 1;
    Task* t = (Task*)calloc(1, sizeof(Task));
    if (!t) return 1;
    t->env = env;
    /* 先挂根再入队，避免 worker 抢跑后 remove 早于 add */
    if (env) hao_gc_add_root(env);
    int failed = 0;
#ifdef _WIN32
    hao_win_crit_enter(p->mtx);
    failed = p->shutdown;
    if (!failed) {
        if (p->tail) p->tail->next = t; else p->head = t;
        p->tail = t;
    }
    hao_win_crit_leave(p->mtx);
    if (!failed) hao_win_cond_wake(p->cv);
#else
    pthread_mutex_lock(&p->mtx);
    failed = p->shutdown;
    if (!failed) {
        if (p->tail) p->tail->next = t; else p->head = t;
        p->tail = t;
    }
    pthread_mutex_unlock(&p->mtx);
    if (!failed) pthread_cond_signal(&p->cv);
#endif
    if (failed) {
        if (env) hao_gc_remove_root(env);
        free(t);
        return 1;
    }
    return 0;
}

void hao_pool_shutdown(int64_t pool) {
    Pool* p = (Pool*)(intptr_t)pool;
    if (!p) return;
#ifdef _WIN32
    hao_win_crit_enter(p->mtx);
    p->shutdown = 1;
    hao_win_crit_leave(p->mtx);
    hao_win_cond_wake_all(p->cv);
#else
    pthread_mutex_lock(&p->mtx);
    p->shutdown = 1;
    pthread_mutex_unlock(&p->mtx);
    pthread_cond_broadcast(&p->cv);
#endif
    /* 等待全部 worker 退出，保证已提交任务跑完后再返回（正测 pool 计数可靠） */
    pool_join_workers(p);
    pool_drain_tasks(p);
}
