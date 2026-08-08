/*
 * HaoLang 运行时 —— 垃圾回收器（GC v3 / v0.37）
 * ------------------------------------------------------------
 *  精确堆扫描（kind + 位图）+ 非移动分代 + 写屏障；
 *  栈/寄存器仍保守扫描；协作式 safepoint 握手 STW + Suspend 硬兜底；
 *  finalizer 解锁后执行（防持锁回调死锁）；
 *  多线程下阈值亦可自动 minor/major（无 gc_thread_count 门闩）。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "runtime_internal.h"

#ifndef _WIN32
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <ucontext.h>
#endif

static void hao_gc_lock(void);
static void hao_gc_unlock(void);
static void gc_scan_range(char* lo, char* hi);

#define GC_ALIGN 16
#define GC_GEN_YOUNG 0
#define GC_GEN_OLD   1
#define GC_PROMOTE_AGE 2
#define GC_NURSERY_THRESHOLD ((size_t)1 << 18)
#define GC_MINORS_BEFORE_MAJOR 8

typedef void (*HaoFinalizerFn)(void* user);

/* 32 字节块头（与 v2 同宽）：用户指针 = b+1，16 对齐。
 * scan_meta 为 32 位：SLOTS 位图低 32 槽；ARRAY 的 is_ptr；>32 槽退回 FULL。 */
typedef struct GCBlock {
    size_t user_size;
    struct GCBlock* next;
    HaoFinalizerFn finalizer;
    uint32_t scan_meta;
    uint8_t marked;
    uint8_t scan_kind;
    uint8_t gen;
    uint8_t age;
} GCBlock;

typedef char gc_block_size_ok[sizeof(GCBlock) == 32 ? 1 : -1];

#define GC_HEADER sizeof(GCBlock)

static int64_t gc_finalizer_runs = 0;
static int64_t gc_finalizer_sets = 0;
static int64_t gc_collect_count = 0;
static int64_t gc_minor_count = 0; /* young / nursery 回收次数（对标 .NET Gen0） */
static int64_t gc_major_count = 0; /* 全堆回收次数（对标 .NET Gen2） */
static int     gc_in_collect = 0;

/* 清扫阶段只摘链入队；解锁后再回调并 free，避免持锁死锁。 */
typedef struct {
    HaoFinalizerFn fn;
    void*          user;
    GCBlock*       block;
} GCPendingFinalizer;
static GCPendingFinalizer* gc_pend_fin = NULL;
static size_t gc_pend_fin_n = 0;
static size_t gc_pend_fin_cap = 0;

static GCBlock* gc_heap = NULL;
static size_t   gc_allocated = 0;
static size_t   gc_nursery_alloc = 0;
static size_t   gc_threshold = 1 << 20;
static size_t   gc_live = 0;
static int      gc_minors_since_major = 0;
static int64_t  gc_main_tid = 0;

static void**  gc_roots = NULL;
static size_t  gc_root_count = 0;
static size_t  gc_root_cap = 0;

/* 根槽：指向全局/静态 ptr 变量的地址；标记时解引用。 */
static void**  gc_root_slots = NULL;
static size_t  gc_root_slot_count = 0;
static size_t  gc_root_slot_cap = 0;

static void**  gc_remset = NULL;
static size_t  gc_remset_count = 0;
static size_t  gc_remset_cap = 0;

#ifdef _WIN32
static __declspec(thread) char* g_stk_top = NULL;
static __declspec(thread) int   g_stk_reg  = 0;
static __declspec(thread) char  g_gpr_spill[128];
static __declspec(thread) char* g_park_sp = NULL;
static __declspec(thread) int   g_parked = 0;
#else
static __thread char* g_stk_top = NULL;
static __thread int   g_stk_reg  = 0;
static __thread char  g_gpr_spill[128];
static __thread char* g_park_sp = NULL;
static __thread int   g_parked = 0;
#endif

#define GC_MAX_THREADS 256
#define GC_GPR_SPILL_BYTES 128
typedef struct {
    int64_t id;
    char*   stack_top;
    char**  park_sp_slot;
    char*   gpr_spill;
    int*    parked_flag;
} GcThread;
static GcThread gc_threads[GC_MAX_THREADS];
static int   gc_thread_count = 0;

static volatile int gc_stw_request = 0;
static volatile int gc_stw_release = 1;

#ifndef _WIN32
#define GC_PARK_SIG SIGUSR2
#endif

static int64_t gc_os_tid(void) {
#ifdef _WIN32
    return (int64_t)hao_win_get_current_thread_id();
#else
    return (int64_t)pthread_self();
#endif
}

static void gc_yield_brief(void) {
#ifdef _WIN32
    hao_win_switch_to_thread();
#else
    sched_yield();
#endif
}

#if defined(__x86_64__) || defined(_M_X64)
static void gc_spill_gprs_to(char* dst) {
    uintptr_t regs[16];
    __asm__ __volatile__(
        "movq %%rax, %0\n\t"
        "movq %%rbx, %1\n\t"
        "movq %%rcx, %2\n\t"
        "movq %%rdx, %3\n\t"
        "movq %%rsi, %4\n\t"
        "movq %%rdi, %5\n\t"
        "movq %%rbp, %6\n\t"
        "movq %%rsp, %7\n\t"
        "movq %%r8,  %8\n\t"
        "movq %%r9,  %9\n\t"
        "movq %%r10, %10\n\t"
        "movq %%r11, %11\n\t"
        "movq %%r12, %12\n\t"
        "movq %%r13, %13\n\t"
        "movq %%r14, %14\n\t"
        "movq %%r15, %15\n\t"
        : "=m"(regs[0]), "=m"(regs[1]), "=m"(regs[2]), "=m"(regs[3]),
          "=m"(regs[4]), "=m"(regs[5]), "=m"(regs[6]), "=m"(regs[7]),
          "=m"(regs[8]), "=m"(regs[9]), "=m"(regs[10]), "=m"(regs[11]),
          "=m"(regs[12]), "=m"(regs[13]), "=m"(regs[14]), "=m"(regs[15])
        :
        : "memory");
    memcpy(dst, regs, GC_GPR_SPILL_BYTES);
}
#else
static void gc_spill_gprs_to(char* dst) {
    memset(dst, 0, GC_GPR_SPILL_BYTES);
}
#endif

static void gc_park_wait(void) {
    char local;
    g_park_sp = &local;
    __atomic_store_n(&g_parked, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&gc_stw_release, __ATOMIC_ACQUIRE))
        gc_yield_brief();
    __atomic_store_n(&g_parked, 0, __ATOMIC_RELEASE);
    g_park_sp = NULL;
}

void hao_gc_safepoint(void) {
    if (!__atomic_load_n(&gc_stw_request, __ATOMIC_ACQUIRE)) return;
    gc_spill_gprs_to(g_gpr_spill);
    gc_park_wait();
}

#ifndef _WIN32
static void gc_park_handler(int sig, siginfo_t* info, void* uctx) {
    (void)sig;
    (void)info;
    ucontext_t* uc = (ucontext_t*)uctx;
    if (uc) {
#if defined(__x86_64__)
        greg_t* g = uc->uc_mcontext.gregs;
        uintptr_t regs[16];
        regs[0] = (uintptr_t)g[REG_RAX];
        regs[1] = (uintptr_t)g[REG_RBX];
        regs[2] = (uintptr_t)g[REG_RCX];
        regs[3] = (uintptr_t)g[REG_RDX];
        regs[4] = (uintptr_t)g[REG_RSI];
        regs[5] = (uintptr_t)g[REG_RDI];
        regs[6] = (uintptr_t)g[REG_RBP];
        regs[7] = (uintptr_t)g[REG_RSP];
        regs[8] = (uintptr_t)g[REG_R8];
        regs[9] = (uintptr_t)g[REG_R9];
        regs[10] = (uintptr_t)g[REG_R10];
        regs[11] = (uintptr_t)g[REG_R11];
        regs[12] = (uintptr_t)g[REG_R12];
        regs[13] = (uintptr_t)g[REG_R13];
        regs[14] = (uintptr_t)g[REG_R14];
        regs[15] = (uintptr_t)g[REG_R15];
        memcpy(g_gpr_spill, regs, GC_GPR_SPILL_BYTES);
#else
        gc_spill_gprs_to(g_gpr_spill);
#endif
    } else {
        gc_spill_gprs_to(g_gpr_spill);
    }
    gc_park_wait();
}
#endif

static char* gc_current_stack_top(void) {
#ifdef _WIN32
    return hao_win_stack_base();
#else
    pthread_attr_t at; void* base = NULL; size_t sz = 0;
    if (pthread_getattr_np(pthread_self(), &at) == 0) {
        pthread_attr_getstack(&at, &base, &sz);
        pthread_attr_destroy(&at);
    }
    if (base && sz) return (char*)base + sz;
    char local; return &local + (8 << 20);
#endif
}

void gc_register_thread(void) {
    if (g_stk_reg) return;
    char* top = gc_current_stack_top();
    if (!top) return;
    hao_gc_lock();
    if (gc_thread_count >= GC_MAX_THREADS) {
        hao_gc_unlock();
        fputs("panic: GC 线程注册表已满\n", stderr);
        exit(1);
    }
    g_stk_top = top;
    g_stk_reg = 1;
    gc_threads[gc_thread_count].id = gc_os_tid();
    gc_threads[gc_thread_count].stack_top = top;
    gc_threads[gc_thread_count].park_sp_slot = &g_park_sp;
    gc_threads[gc_thread_count].gpr_spill = g_gpr_spill;
    gc_threads[gc_thread_count].parked_flag = &g_parked;
    gc_thread_count++;
    hao_gc_unlock();
}

void gc_unregister_thread(void) {
    if (!g_stk_reg) return;
    /* 离表前先响应 STW，避免快照里留下将退出的线程。 */
    hao_gc_safepoint();
    int64_t id = gc_os_tid();
    for (;;) {
        hao_gc_lock();
        if (__atomic_load_n(&gc_stw_request, __ATOMIC_ACQUIRE)) {
            hao_gc_unlock();
            hao_gc_safepoint();
            continue;
        }
        break;
    }
    for (int i = 0; i < gc_thread_count; ++i)
        if (gc_threads[i].id == id) {
            gc_threads[i] = gc_threads[gc_thread_count - 1];
            gc_thread_count--;
            break;
        }
    hao_gc_unlock();
    g_stk_reg = 0;
    g_stk_top = NULL;
}

static void gc_scan_parked_thread(const GcThread* t) {
    if (t->gpr_spill)
        gc_scan_range(t->gpr_spill, t->gpr_spill + GC_GPR_SPILL_BYTES);
    char* sp = (t->park_sp_slot && *t->park_sp_slot) ? *t->park_sp_slot : NULL;
    if (sp && t->stack_top && sp < t->stack_top &&
        (size_t)(t->stack_top - sp) <= (size_t)8 * 1024 * 1024)
        gc_scan_range(sp, t->stack_top);
}

/* 多轮协作未齐 park 后走全标活的次数（诊断用） */
static int64_t g_stw_mark_all_fallbacks = 0;

/*
 * 协作 STW：多轮等待 mutator park；禁止 Win SuspendThread（易 AV）。
 * 轮间复位 request/release 再重试；仅最终仍 missing 才全标活保底。
 * 调用方必须已持有 GC 锁；返回时仍持锁。
 */
#define GC_STW_ROUNDS       3
#define GC_STW_SPIN_BASE    1000000

static void gc_stop_the_world_scan(void) {
    int64_t self = gc_os_tid();
    int round;
    int missing = 0;

    GcThread snap[GC_MAX_THREADS];
    int nsnap = gc_thread_count;
    if (nsnap > GC_MAX_THREADS) nsnap = GC_MAX_THREADS;
    for (int i = 0; i < nsnap; ++i) snap[i] = gc_threads[i];

    int targets = 0;
    for (int i = 0; i < nsnap; ++i)
        if (snap[i].id != self) targets++;
    if (targets == 0) return;

    for (round = 0; round < GC_STW_ROUNDS; ++round) {
        __atomic_store_n(&gc_stw_release, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&gc_stw_request, 1, __ATOMIC_RELEASE);

        /* 放锁：否则其它线程堵在 crit_enter，永远到不了 hao_gc_safepoint。 */
        hao_gc_unlock();

#ifndef _WIN32
        for (int i = 0; i < nsnap; ++i) {
            if (snap[i].id == self) continue;
            pthread_kill((pthread_t)snap[i].id, GC_PARK_SIG);
        }
#endif

        {
            int spin_lim = GC_STW_SPIN_BASE * (round + 1);
            for (int spin = 0; spin < spin_lim; ++spin) {
                int parked = 0;
                for (int i = 0; i < nsnap; ++i) {
                    if (snap[i].id == self) continue;
                    if (snap[i].parked_flag &&
                        __atomic_load_n(snap[i].parked_flag, __ATOMIC_ACQUIRE))
                        parked++;
                }
                if (parked >= targets) break;
                gc_yield_brief();
            }
        }

        hao_gc_lock();

        missing = 0;
        for (int i = 0; i < nsnap; ++i) {
            if (snap[i].id == self) continue;
            int live = 0;
            for (int j = 0; j < gc_thread_count; ++j)
                if (gc_threads[j].id == snap[i].id) { live = 1; break; }
            if (!live) continue;
            if (!(snap[i].parked_flag &&
                  __atomic_load_n(snap[i].parked_flag, __ATOMIC_ACQUIRE)))
                missing++;
        }

        if (missing == 0 || round + 1 >= GC_STW_ROUNDS)
            break;

        /* 未齐且还有轮次：放行后刷新快照再试 */
        __atomic_store_n(&gc_stw_request, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&gc_stw_release, 1, __ATOMIC_RELEASE);
        gc_yield_brief();
        nsnap = gc_thread_count;
        if (nsnap > GC_MAX_THREADS) nsnap = GC_MAX_THREADS;
        for (int i = 0; i < nsnap; ++i) snap[i] = gc_threads[i];
        targets = 0;
        for (int i = 0; i < nsnap; ++i)
            if (snap[i].id != self) targets++;
        if (targets == 0) {
            missing = 0;
            break;
        }
    }

    /* 此时仍持锁且 request=1：扫描已 park 者；仍 missing 则全标活保底 */
    for (int i = 0; i < nsnap; ++i) {
        if (snap[i].id == self) continue;
        int live = 0;
        for (int j = 0; j < gc_thread_count; ++j)
            if (gc_threads[j].id == snap[i].id) { live = 1; break; }
        if (!live) continue;
        if (snap[i].parked_flag &&
            __atomic_load_n(snap[i].parked_flag, __ATOMIC_ACQUIRE))
            gc_scan_parked_thread(&snap[i]);
    }

    if (missing) {
        g_stw_mark_all_fallbacks++;
        for (GCBlock* b = gc_heap; b; b = b->next) b->marked = 1;
    }

    __atomic_store_n(&gc_stw_request, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&gc_stw_release, 1, __ATOMIC_RELEASE);
    gc_yield_brief();
}

static char* gc_heap_lo = NULL;
static char* gc_heap_hi = NULL;

static GCBlock* gc_find_block(void* p) {
    if (!p) return NULL;
    if ((char*)p < gc_heap_lo || (char*)p >= gc_heap_hi) return NULL;
    for (GCBlock* b = gc_heap; b; b = b->next) {
        char* u = (char*)(b + 1);
        if ((char*)p >= u && (char*)p <= u + b->user_size) return b;
    }
    return NULL;
}

int8_t hao_gc_is_heap_ptr(void* p) {
    int8_t ok;
    if (!p) return 0;
    hao_gc_lock();
    ok = gc_find_block(p) ? 1 : 0;
    hao_gc_unlock();
    return ok;
}

int64_t hao_gc_stw_mark_all_fallbacks(void) {
    int64_t n;
    hao_gc_lock();
    n = g_stw_mark_all_fallbacks;
    hao_gc_unlock();
    return n;
}

void gc_collect_inner(char* regs, size_t regs_size);

#define GC_WORKLIST_CAP 4096
static GCBlock** gc_worklist = NULL;
static size_t gc_wl_count = 0, gc_wl_cap = 0;
static int gc_mark_major = 1;

static void gc_enqueue(GCBlock* b) {
    if (!b || b->marked) return;
    if (!gc_mark_major && b->gen != GC_GEN_YOUNG) return;
    b->marked = 1;
    if (gc_wl_count >= gc_wl_cap) {
        gc_wl_cap = gc_wl_cap ? gc_wl_cap * 2 : GC_WORKLIST_CAP;
        gc_worklist = (GCBlock**)realloc(gc_worklist, gc_wl_cap * sizeof(GCBlock*));
        if (!gc_worklist) { fputs("panic: GC 工作集分配失败\n", stderr); exit(1); }
    }
    gc_worklist[gc_wl_count++] = b;
}

static void gc_mark_ptr(uintptr_t v) {
    if (v == 0) return;
    /* 对象基址 16 对齐，但栈上可能暂存字段内指针（+8）；按指针宽对齐即可。 */
    if ((v & (sizeof(uintptr_t) - 1)) != 0) return;
    gc_enqueue(gc_find_block((void*)v));
}

static void gc_pend_fin_add(HaoFinalizerFn fn, void* user, GCBlock* block) {
    if (gc_pend_fin_n >= gc_pend_fin_cap) {
        gc_pend_fin_cap = gc_pend_fin_cap ? gc_pend_fin_cap * 2 : 32;
        gc_pend_fin = (GCPendingFinalizer*)realloc(
            gc_pend_fin, gc_pend_fin_cap * sizeof(GCPendingFinalizer));
        if (!gc_pend_fin) {
            fputs("panic: GC finalizer 队列分配失败\n", stderr);
            exit(1);
        }
    }
    gc_pend_fin[gc_pend_fin_n].fn = fn;
    gc_pend_fin[gc_pend_fin_n].user = user;
    gc_pend_fin[gc_pend_fin_n].block = block;
    gc_pend_fin_n++;
}

/* 回调在锁外执行；队列摘取需短暂加锁防并发 drain 互抢。 */
static void gc_drain_finalizers(void) {
    hao_gc_lock();
    size_t n = gc_pend_fin_n;
    GCPendingFinalizer* list = gc_pend_fin;
    gc_pend_fin = NULL;
    gc_pend_fin_n = 0;
    gc_pend_fin_cap = 0;
    hao_gc_unlock();
    for (size_t i = 0; i < n; ++i) {
        if (list[i].fn) {
            __atomic_fetch_add(&gc_finalizer_runs, 1, __ATOMIC_RELAXED);
            list[i].fn(list[i].user);
        }
        free(list[i].block);
    }
    free(list);
}

static void gc_scan_range(char* lo, char* hi) {
    if (lo > hi) { char* t = lo; lo = hi; hi = t; }
    uintptr_t cur = ((uintptr_t)lo + sizeof(uintptr_t) - 1) & ~(uintptr_t)(sizeof(uintptr_t) - 1);
    uintptr_t end = (uintptr_t)hi;
    for (; cur + sizeof(uintptr_t) <= end; cur += sizeof(uintptr_t))
        gc_mark_ptr(*(uintptr_t*)cur);
}

static void gc_scan_block_precise(GCBlock* b) {
    char* u = (char*)(b + 1);
    switch (b->scan_kind) {
    case GC_KIND_OPAQUE:
        return;
    case GC_KIND_SLOTS: {
        uint32_t bm = b->scan_meta;
        size_t nslots = b->user_size / sizeof(uintptr_t);
        if (nslots > 32) nslots = 32;
        for (size_t i = 0; i < nslots; ++i) {
            if (bm & (1u << i))
                gc_mark_ptr(((uintptr_t*)u)[i]);
        }
        return;
    }
    case GC_KIND_ARRAY: {
        if ((b->scan_meta & 1u) == 0) return;
        if (b->user_size < (size_t)HAO_ARR_HEADER) return;
        char* elems = u + HAO_ARR_HEADER;
        int64_t len = *(int64_t*)(u + HAO_ARR_LEN_OFF_BASE);
        int64_t esz = *(int64_t*)(u + HAO_ARR_ESZ_OFF_BASE);
        if (esz != 8 || len <= 0) return;
        size_t max = (b->user_size - (size_t)HAO_ARR_HEADER) / 8;
        if ((size_t)len > max) len = (int64_t)max;
        for (int64_t i = 0; i < len; ++i)
            gc_mark_ptr(((uintptr_t*)elems)[i]);
        return;
    }
    case GC_KIND_FULL:
    default: {
        size_t n = b->user_size / sizeof(uintptr_t);
        for (size_t i = 0; i < n; ++i)
            gc_mark_ptr(((uintptr_t*)u)[i]);
        return;
    }
    }
}

static void gc_process_worklist(void) {
    for (size_t k = 0; k < gc_wl_count; ++k)
        gc_scan_block_precise(gc_worklist[k]);
    gc_wl_count = 0;
    free(gc_worklist);
    gc_worklist = NULL;
    gc_wl_cap = 0;
}

static void gc_remset_add(void* user) {
    if (!user) return;
    for (size_t i = 0; i < gc_remset_count; ++i)
        if (gc_remset[i] == user) return;
    if (gc_remset_count >= gc_remset_cap) {
        gc_remset_cap = gc_remset_cap ? gc_remset_cap * 2 : 64;
        gc_remset = (void**)realloc(gc_remset, gc_remset_cap * sizeof(void*));
        if (!gc_remset) { fputs("panic: GC remset 分配失败\n", stderr); exit(1); }
    }
    gc_remset[gc_remset_count++] = user;
}

/* 丢掉已回收对象；跨 minor 必须保留仍存活的 old→young 边。 */
static void gc_remset_filter_live(void) {
    size_t w = 0;
    for (size_t i = 0; i < gc_remset_count; ++i) {
        if (gc_find_block(gc_remset[i]))
            gc_remset[w++] = gc_remset[i];
    }
    gc_remset_count = w;
}

void hao_gc_barrier(void* dst, void* new_val) {
    if (!dst || !new_val) return;
    for (;;) {
        hao_gc_safepoint();
        hao_gc_lock();
        if (__atomic_load_n(&gc_stw_request, __ATOMIC_ACQUIRE)) {
            hao_gc_unlock();
            continue;
        }
        break;
    }
    GCBlock* db = gc_find_block(dst);
    GCBlock* nb = gc_find_block(new_val);
    if (db && nb && db->gen == GC_GEN_OLD && nb->gen == GC_GEN_YOUNG)
        gc_remset_add((void*)(db + 1));
    hao_gc_unlock();
}

static int g_collect_want_major = 1;

#if defined(__x86_64__) || defined(_M_X64)
__attribute__((naked))
static void gc_collect_trampoline(void) {
    __asm__(
        ".intel_syntax noprefix\n"
        "sub rsp, 192\n"
        "mov qword ptr [rsp +  32], rax\n"
        "mov qword ptr [rsp +  40], rbx\n"
        "mov qword ptr [rsp +  48], rcx\n"
        "mov qword ptr [rsp +  56], rdx\n"
        "mov qword ptr [rsp +  64], rsi\n"
        "mov qword ptr [rsp +  72], rdi\n"
        "mov qword ptr [rsp +  80], rbp\n"
        "mov qword ptr [rsp +  88], rsp\n"
        "mov qword ptr [rsp +  96], r8\n"
        "mov qword ptr [rsp + 104], r9\n"
        "mov qword ptr [rsp + 112], r10\n"
        "mov qword ptr [rsp + 120], r11\n"
        "mov qword ptr [rsp + 128], r12\n"
        "mov qword ptr [rsp + 136], r13\n"
        "mov qword ptr [rsp + 144], r14\n"
        "mov qword ptr [rsp + 152], r15\n"
        "lea rcx, [rsp + 32]\n"
        "mov rdx, 128\n"
        "call gc_collect_inner\n"
        "mov rbx, [rsp +  40]\n"
        "mov rsi, [rsp +  64]\n"
        "mov rdi, [rsp +  72]\n"
        "mov rbp, [rsp +  80]\n"
        "mov r12, [rsp + 128]\n"
        "mov r13, [rsp + 136]\n"
        "mov r14, [rsp + 144]\n"
        "mov r15, [rsp + 152]\n"
        "add rsp, 192\n"
        "ret\n"
        ".att_syntax prefix\n"
    );
}
#else
static void gc_collect_trampoline(void) { gc_collect_inner(0, 0); }
#endif

__attribute__((noinline))
void gc_collect_inner(char* regs, size_t regs_size) {
    int major = g_collect_want_major;
    gc_mark_major = major ? 1 : 0;

    if (major) {
        for (GCBlock* b = gc_heap; b; b = b->next) b->marked = 0;
    } else {
        for (GCBlock* b = gc_heap; b; b = b->next)
            if (b->gen == GC_GEN_YOUNG) b->marked = 0;
    }

    for (size_t i = 0; i < gc_root_count; ++i)
        if (gc_roots[i]) gc_mark_ptr((uintptr_t)gc_roots[i]);
    for (size_t i = 0; i < gc_root_slot_count; ++i) {
        void** slot = (void**)gc_root_slots[i];
        if (slot && *slot) gc_mark_ptr((uintptr_t)*slot);
    }

    for (size_t i = 0; i < gc_remset_count; ++i) {
        GCBlock* b = gc_find_block(gc_remset[i]);
        if (!b) continue;
        if (major) gc_enqueue(b);
        else gc_scan_block_precise(b);
    }

    if (regs && regs_size) gc_scan_range(regs, regs + regs_size);
    char* sp;
#ifdef _WIN32
    sp = (char*)_AddressOfReturnAddress();
#else
    char local; sp = &local;
#endif
    if (g_stk_top && sp < g_stk_top)
        gc_scan_range(sp, g_stk_top);

    gc_stop_the_world_scan();
    gc_process_worklist();

    void** promoted_buf = NULL;
    size_t promoted_n = 0, promoted_cap = 0;

    gc_in_collect = 1;
    GCBlock** prev = &gc_heap;
    size_t live = 0;
    while (*prev) {
        GCBlock* b = *prev;
        int collect_this = major ? !b->marked
                                 : (b->gen == GC_GEN_YOUNG && !b->marked);
        if (!collect_this) {
            if (!major && b->gen == GC_GEN_YOUNG && b->marked) {
                b->age += 1;
                if (b->age >= GC_PROMOTE_AGE) {
                    b->gen = GC_GEN_OLD;
                    if (promoted_n >= promoted_cap) {
                        promoted_cap = promoted_cap ? promoted_cap * 2 : 256;
                        promoted_buf = (void**)realloc(
                            promoted_buf, promoted_cap * sizeof(void*));
                        if (!promoted_buf) {
                            fputs("panic: GC 晋升缓冲分配失败\n", stderr);
                            exit(1);
                        }
                    }
                    promoted_buf[promoted_n++] = (void*)(b + 1);
                }
            }
            live += b->user_size;
            prev = &b->next;
        } else {
            *prev = b->next;
            void* user = (void*)(b + 1);
            HaoFinalizerFn fn = b->finalizer;
            b->finalizer = NULL;
            if (fn)
                gc_pend_fin_add(fn, user, b);
            else
                free(b);
        }
    }
    gc_in_collect = 0;

    /* 旧实现 clear 后只挂晋升对象：已存在的 old→young 边跨 minor 会漏标。 */
    gc_remset_filter_live();
    for (size_t i = 0; i < promoted_n; ++i)
        gc_remset_add(promoted_buf[i]);
    free(promoted_buf);
    gc_nursery_alloc = 0;

    if (major) {
        gc_live = live;
        gc_threshold = live * 2;
        if (gc_threshold < (size_t)1 << 18) gc_threshold = (size_t)1 << 18;
        if (gc_threshold > (size_t)256 << 20) gc_threshold = (size_t)256 << 20;
        gc_allocated = 0;
        gc_minors_since_major = 0;
        gc_major_count += 1;
    } else {
        gc_minors_since_major += 1;
        gc_minor_count += 1;
    }
    gc_collect_count += 1;
    gc_mark_major = 1;
    g_collect_want_major = 1;
}

#ifdef _WIN32
static unsigned char gc_mutex_obj[HAO_WIN_CRITSEC_BYTES];
static int gc_mutex_state = 0;
static void hao_gc_lock(void) {
    int s = __atomic_load_n(&gc_mutex_state, __ATOMIC_ACQUIRE);
    if (s != 2) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&gc_mutex_state, &expected, 1, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            hao_win_crit_init(gc_mutex_obj);
            __atomic_store_n(&gc_mutex_state, 2, __ATOMIC_RELEASE);
        } else {
            while (__atomic_load_n(&gc_mutex_state, __ATOMIC_ACQUIRE) != 2)
                hao_win_switch_to_thread();
        }
    }
    hao_win_crit_enter(gc_mutex_obj);
}
static void hao_gc_unlock(void) { hao_win_crit_leave(gc_mutex_obj); }
#else
static pthread_mutex_t gc_mutex_obj = PTHREAD_MUTEX_INITIALIZER;
static void hao_gc_lock(void) { pthread_mutex_lock(&gc_mutex_obj); }
static void hao_gc_unlock(void) { pthread_mutex_unlock(&gc_mutex_obj); }
#endif

static int gc_collecting = 0;

static void gc_run_collect_locked(int major) {
    if (gc_collecting) return; /* 防 STW 放锁窗口内嵌套收集 */
    gc_collecting = 1;
    g_collect_want_major = major ? 1 : 0;
    gc_collect_trampoline();
    gc_collecting = 0;
}

void gc_collect(void) {
    hao_gc_lock();
    g_collect_want_major = 1;
    gc_collect_trampoline();
    hao_gc_unlock();
    gc_drain_finalizers();
}

void hao_gc_add_root(void* p) {
    hao_gc_lock();
    if (!p) { hao_gc_unlock(); return; }
    if (gc_root_count >= gc_root_cap) {
        gc_root_cap = gc_root_cap ? gc_root_cap * 2 : 16;
        gc_roots = (void**)realloc(gc_roots, gc_root_cap * sizeof(void*));
        if (!gc_roots) { fputs("panic: GC 根数组分配失败\n", stderr); exit(1); }
    }
    gc_roots[gc_root_count++] = p;
    hao_gc_unlock();
}

void hao_gc_add_root_slot(void* slot) {
    hao_gc_lock();
    if (!slot) { hao_gc_unlock(); return; }
    for (size_t i = 0; i < gc_root_slot_count; ++i) {
        if (gc_root_slots[i] == slot) { hao_gc_unlock(); return; }
    }
    if (gc_root_slot_count >= gc_root_slot_cap) {
        gc_root_slot_cap = gc_root_slot_cap ? gc_root_slot_cap * 2 : 16;
        gc_root_slots = (void**)realloc(gc_root_slots,
                                        gc_root_slot_cap * sizeof(void*));
        if (!gc_root_slots) {
            fputs("panic: GC 根槽数组分配失败\n", stderr);
            exit(1);
        }
    }
    gc_root_slots[gc_root_slot_count++] = slot;
    hao_gc_unlock();
}

void hao_gc_set_finalizer(void* obj, HaoFinalizer fn) {
    if (!obj) return;
    if (!gc_in_collect) hao_gc_lock();
    GCBlock* b = gc_find_block(obj);
    if (b) {
        b->finalizer = (HaoFinalizerFn)fn;
        if (fn) gc_finalizer_sets += 1;
    }
    if (!gc_in_collect) hao_gc_unlock();
}

int64_t hao_gc_finalizer_sets(void) { return gc_finalizer_sets; }

void hao_gc_clear_finalizer(void* obj) {
    if (!obj) return;
    if (!gc_in_collect) hao_gc_lock();
    GCBlock* b = gc_find_block(obj);
    if (b) b->finalizer = NULL;
    if (!gc_in_collect) hao_gc_unlock();
}

int64_t hao_gc_finalizer_runs(void) { return gc_finalizer_runs; }

int64_t hao_gc_live_bytes(void) {
    hao_gc_lock();
    int64_t v = (int64_t)gc_live;
    hao_gc_unlock();
    return v;
}

int64_t hao_gc_threshold(void) {
    hao_gc_lock();
    int64_t v = (int64_t)gc_threshold;
    hao_gc_unlock();
    return v;
}

int64_t hao_gc_allocated_since(void) {
    hao_gc_lock();
    int64_t v = (int64_t)gc_allocated;
    hao_gc_unlock();
    return v;
}

int64_t hao_gc_block_count(void) {
    hao_gc_lock();
    int64_t n = 0;
    for (GCBlock* b = gc_heap; b; b = b->next) n += 1;
    hao_gc_unlock();
    return n;
}

int64_t hao_gc_collect_count(void) {
    hao_gc_lock();
    int64_t v = gc_collect_count;
    hao_gc_unlock();
    return v;
}

int64_t hao_gc_minor_count(void) {
    hao_gc_lock();
    int64_t v = gc_minor_count;
    hao_gc_unlock();
    return v;
}

int64_t hao_gc_major_count(void) {
    hao_gc_lock();
    int64_t v = gc_major_count;
    hao_gc_unlock();
    return v;
}

/* 自上次 minor/major 以来 nursery 累计分配（触发 minor 的压力指标） */
int64_t hao_gc_nursery_bytes(void) {
    hao_gc_lock();
    int64_t v = (int64_t)gc_nursery_alloc;
    hao_gc_unlock();
    return v;
}

/* 已向 GC 注册的线程数（参与 STW 栈扫描；≠ OS 线程总数） */
int64_t hao_gc_registered_threads(void) {
    hao_gc_lock();
    int64_t v = (int64_t)gc_thread_count;
    hao_gc_unlock();
    return v;
}

void hao_gc_remove_root(void* p) {
    hao_gc_lock();
    for (size_t i = 0; i < gc_root_count; ++i) {
        if (gc_roots[i] == p) {
            gc_roots[i] = gc_roots[--gc_root_count];
            break;
        }
    }
    hao_gc_unlock();
}

void gc_init(void) {
    gc_register_thread();
    if (gc_main_tid == 0) gc_main_tid = gc_os_tid();
#ifndef _WIN32
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = gc_park_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(GC_PARK_SIG, &sa, NULL);
#endif
}

#ifndef _WIN32
__attribute__((constructor))
static void hao_gc_init_ctor(void) { gc_init(); }
#endif

void* gc_alloc_ex(size_t n, uint8_t kind, uint64_t meta) {
    for (;;) {
        hao_gc_safepoint();
        gc_register_thread();
        hao_gc_lock();
        /* 收集器已放锁等 park：若见 request，让出并 park，勿在 STW 中分配。 */
        if (__atomic_load_n(&gc_stw_request, __ATOMIC_ACQUIRE)) {
            hao_gc_unlock();
            continue;
        }
        break;
    }
    if (!g_stk_top) gc_init();
    if (gc_main_tid == 0) gc_main_tid = gc_os_tid();
    if (n == 0) n = 1;
    if (n > SIZE_MAX - GC_HEADER - GC_ALIGN) {
        hao_gc_unlock();
        fputs("panic: 内存分配过大\n", stderr);
        exit(1);
    }
    size_t need = (n + GC_ALIGN - 1) & ~(size_t)(GC_ALIGN - 1);

    if (gc_allocated > SIZE_MAX - need)
        gc_allocated = SIZE_MAX;
    else
        gc_allocated += need;
    if (gc_nursery_alloc > SIZE_MAX - need)
        gc_nursery_alloc = SIZE_MAX;
    else
        gc_nursery_alloc += need;

    /* v0.37：多线程亦可阈值自动回收（safepoint 握手 + Suspend 兜底）。 */
    if (gc_allocated >= gc_threshold ||
        gc_minors_since_major >= GC_MINORS_BEFORE_MAJOR)
        gc_run_collect_locked(1);
    else if (gc_nursery_alloc >= GC_NURSERY_THRESHOLD)
        gc_run_collect_locked(0);

    size_t total = GC_HEADER + need;
    GCBlock* b = (GCBlock*)calloc(1, total);
    if (!b) {
        gc_run_collect_locked(1);
        b = (GCBlock*)calloc(1, total);
        if (!b) { fputs("panic: 内存分配失败\n", stderr); exit(1); }
    }
    b->user_size = need;
    b->marked = 0;
    b->scan_kind = kind;
    b->scan_meta = (uint32_t)meta;
    b->gen = GC_GEN_YOUNG;
    b->age = 0;
    b->next = gc_heap;
    gc_heap = b;

    char* u = (char*)(b + 1);
    if (!gc_heap_lo || u < gc_heap_lo) gc_heap_lo = u;
    if (!gc_heap_hi || u + need > gc_heap_hi) gc_heap_hi = u + need;

    hao_gc_unlock();
    gc_drain_finalizers();
    return u;
}

void* gc_alloc(size_t n) {
    return gc_alloc_ex(n, GC_KIND_FULL, 0);
}
