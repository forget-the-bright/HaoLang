/*
 * HaoLang 运行时 —— 垃圾回收器（GC v3 / v0.55.2+）
 * ------------------------------------------------------------
 *  权威契约：docs/IR与GC契约.md
 *  精确堆 + 分代 remset（仅 minor seed）+ 混合屏障 + 色纪元；
 *  诚实双轨：shadow 始终扫；os_block/纯 C 另扫 GPR+有界 C 叶；
 *  major 禁止把 remset 当根；mark worker 不注册。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "runtime_internal.h"

#ifndef _WIN32
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <time.h>
#include <ucontext.h>
#endif

static void hao_gc_lock(void);
static void hao_gc_unlock(void);
static void gc_scan_range(char* lo, char* hi);
static void gc_mark_ptr(uintptr_t v);
static int64_t gc_mono_ms(void);
static void gc_stw_leave(void);

/* 并发 drain：密分配+屏障可持续入灰；不设上限会占着 gc_collecting 拖死 HTTP */
#define GC_CONCURRENT_DRAIN_MAX  (1u << 16)
#define GC_CONCURRENT_DRAIN_MS   48
/* 远超 STW 预算仍不 release → 收集者楔死；强行放行保进程可服务 */
#define GC_PARK_WATCHDOG_MS      2000
/* 时间源异常时 mono 差可能永不达标；自旋上限作第二道 watchdog */
#define GC_PARK_WATCHDOG_SPINS   4000000u

#define GC_ALIGN 16
#define GC_GEN_YOUNG 0
#define GC_GEN_OLD   1
#define GC_PROMOTE_AGE 4 /* 略抬高：密分配下 age=2 过早进 old，minor 收不回 */
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
static int64_t  gc_num_blocks = 0; /* O(1); matches heap-list length */
static size_t   gc_allocated = 0;
static size_t   gc_nursery_alloc = 0;
static size_t   gc_threshold = 1 << 20;
static size_t   gc_live = 0;
static int      gc_minors_since_major = 0;
static int64_t  gc_main_tid = 0;
/* v0.50.4：STW 未齐时不再全标活；incomplete + pacing 避免死亡螺旋 */
static int64_t  g_stw_incomplete = 0;
static int64_t  g_stw_mark_all_fallbacks = 0; /* 兼容旧 API；本版恒 0（已废除全标活） */
static int      gc_pacing_level = 0;          /* 连续 incomplete 退避档位 */
static size_t   gc_nursery_gate = GC_NURSERY_THRESHOLD; /* 可被 pacing 放大 */

/* v0.51+：并发标记相位（mutator 可见；屏障据此 shade） */
#define GC_PHASE_IDLE 0
#define GC_PHASE_MARK 1
static volatile int gc_phase = GC_PHASE_IDLE;
static int64_t      g_concurrent_mark_cycles = 0;
static int64_t      g_mark_assist_steps = 0;
static int64_t      g_mark_abort_cycles = 0; /* 终止失败 abort MARK 次数（v0.53.3） */
static int64_t      g_mark_worker_steps = 0; /* mark worker 推进灰块数（v0.54） */
static int64_t      g_park_watchdog_trips = 0; /* park_wait 超时强行放行（防 Web 永久楔死） */
static size_t       gc_heap_bytes = 0; /* 用户区合计，alloc/sweep 维护 */
/* v0.52：marked==gc_mark_epoch 为本轮已标；setup 只 ++epoch，勿扫百万清零 */
static uint8_t      gc_mark_epoch = 1;
static int          gc_mark_major = 1;       /* 扫/入队用：本轮是否 major */
static int          gc_cycle_is_major = 1;   /* 本轮 cycle 固定，续跑不改 */
static int          g_collect_want_major = 1;
static int64_t      gc_last_term_attempt_ms = 0;

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
static __declspec(thread) void*** g_shadow = NULL;
static __declspec(thread) size_t  g_shadow_n = 0;
static __declspec(thread) size_t  g_shadow_cap = 0;
/* gc_collect 入口 SP：seed 保守扫上限，避免扫进调用方 Hao 帧 */
static __declspec(thread) char* g_collect_c_hi = NULL;
/* os_block/arm 期间：STW 须加扫 GPR+C 叶（C 帧活对象） */
static __declspec(thread) int g_in_os_block = 0;
/* C API 形参钉住：协作 safepoint 在 shadow-only 下会漏标仅活在 C 帧的指针 */
#define GC_SCAN_PINS 4
static __declspec(thread) void* g_scan_pins[GC_SCAN_PINS];
static __declspec(thread) int   g_scan_pin_n = 0;
/* i64 藏指针（invoke 返回值等）跨 Hao 分配窗口 */
#define GC_REFL_I64_PINS 64
static __declspec(thread) void* g_refl_i64_pins[GC_REFL_I64_PINS];
static __declspec(thread) int   g_refl_i64_pin_n = 0;
#else
static __thread char* g_stk_top = NULL;
static __thread int   g_stk_reg  = 0;
static __thread char  g_gpr_spill[128];
static __thread char* g_park_sp = NULL;
static __thread int   g_parked = 0;
static __thread void*** g_shadow = NULL;
static __thread size_t  g_shadow_n = 0;
static __thread size_t  g_shadow_cap = 0;
static __thread char* g_collect_c_hi = NULL;
static __thread int g_in_os_block = 0;
#define GC_SCAN_PINS 4
static __thread void* g_scan_pins[GC_SCAN_PINS];
static __thread int   g_scan_pin_n = 0;
#define GC_REFL_I64_PINS 64
static __thread void* g_refl_i64_pins[GC_REFL_I64_PINS];
static __thread int   g_refl_i64_pin_n = 0;
#endif

static void gc_pin_clear(void) { g_scan_pin_n = 0; }
static void gc_pin_add(void* p) {
    if (!p || g_scan_pin_n >= GC_SCAN_PINS) return;
    g_scan_pins[g_scan_pin_n++] = p;
}

#define GC_MAX_THREADS 256
#define GC_GPR_SPILL_BYTES 128
#define GC_MARK_WORKERS 2
/*
 * park 叶保守窗口：仅 park_wait / os_block C 帧。
 * 过大（数十 KiB）会在浅调用栈上扫回 Hao 死局部，抵消 shadow。
 */
#define GC_CONSERVATIVE_LEAF_BYTES ((size_t)4 * 1024)
typedef struct {
    int64_t id;
    char*   stack_top;
    char**  park_sp_slot;
    char*   gpr_spill;
    int*    parked_flag;
    void**** shadow_slot; /* &g_shadow */
    size_t*  shadow_n;    /* &g_shadow_n */
    int*     in_os_block_flag; /* &g_in_os_block */
    void**   scan_pins;        /* g_scan_pins */
    int*     scan_pin_n;       /* &g_scan_pin_n */
    void**   refl_i64_pins;    /* g_refl_i64_pins */
    int*     refl_i64_pin_n;   /* &g_refl_i64_pin_n */
} GcThread;
static GcThread gc_threads[GC_MAX_THREADS];
static int   gc_thread_count = 0;
/* 本轮标记已扫过栈的线程（终止齐扫才 sweep） */
static int64_t gc_scanned_tids[GC_MAX_THREADS];
static int     gc_scanned_tid_n = 0;

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
    {
        int64_t t0 = gc_mono_ms();
        uint32_t spins = 0;
        while (!__atomic_load_n(&gc_stw_release, __ATOMIC_ACQUIRE)) {
            spins++;
            if (spins >= GC_PARK_WATCHDOG_SPINS ||
                gc_mono_ms() - t0 >= GC_PARK_WATCHDOG_MS) {
                /*
                 * 收集者应在数十 ms 内 leave。超时仍 request=1 则 Web/分配永久楔死。
                 * 强行 leave：可能与正在扫栈的收集者竞态，但优于进程假活。
                 * 下一轮 abort/setup 会 bump epoch 收口。
                 */
                g_park_watchdog_trips += 1;
                g_mark_abort_cycles += 1;
                gc_stw_leave();
                break;
            }
            gc_yield_brief();
        }
    }
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
    gc_threads[gc_thread_count].shadow_slot = &g_shadow;
    gc_threads[gc_thread_count].shadow_n = &g_shadow_n;
    gc_threads[gc_thread_count].in_os_block_flag = &g_in_os_block;
    gc_threads[gc_thread_count].scan_pins = g_scan_pins;
    gc_threads[gc_thread_count].scan_pin_n = &g_scan_pin_n;
    gc_threads[gc_thread_count].refl_i64_pins = g_refl_i64_pins;
    gc_threads[gc_thread_count].refl_i64_pin_n = &g_refl_i64_pin_n;
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

void hao_gc_os_block_enter(void) {
    if (!g_stk_reg) return;
    hao_gc_safepoint();
    gc_spill_gprs_to(g_gpr_spill);
    char* sp;
    __asm__ __volatile__("movq %%rsp, %0" : "=r"(sp));
    g_park_sp = sp;
    __atomic_store_n(&g_in_os_block, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_parked, 1, __ATOMIC_RELEASE);
}

void hao_gc_os_block_leave(void) {
    if (!g_stk_reg) return;
    /*
     * 必须先等 STW 放行再撤 parked：若先 parked=0 再 safepoint，
     * 收集者可能已按「已 park」扫完并进入并发 mark，本线程开跑造成漏根 → 回收活对象 → 进程崩溃。
     */
    if (__atomic_load_n(&gc_stw_request, __ATOMIC_ACQUIRE)) {
        int64_t t0 = gc_mono_ms();
        uint32_t spins = 0;
        while (!__atomic_load_n(&gc_stw_release, __ATOMIC_ACQUIRE)) {
            spins++;
            if (spins >= GC_PARK_WATCHDOG_SPINS ||
                gc_mono_ms() - t0 >= GC_PARK_WATCHDOG_MS) {
                g_park_watchdog_trips += 1;
                g_mark_abort_cycles += 1;
                gc_stw_leave();
                break;
            }
            gc_yield_brief();
        }
    }
    __atomic_store_n(&g_parked, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_in_os_block, 0, __ATOMIC_RELEASE);
    g_park_sp = NULL;
}

/*
 * 持业务锁、即将 cond_wait（会原子放锁）时用：只挂 park，不在此 safepoint。
 * 若用 os_block_enter，safepoint 会持着池/channel 锁 park，其它线程堵锁无法 park → STW 永不全。
 */
void hao_gc_os_block_arm(void) {
    if (!g_stk_reg) return;
    gc_spill_gprs_to(g_gpr_spill);
    char* sp;
    __asm__ __volatile__("movq %%rsp, %0" : "=r"(sp));
    g_park_sp = sp;
    __atomic_store_n(&g_in_os_block, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_parked, 1, __ATOMIC_RELEASE);
}

void hao_gc_os_block_disarm(void) {
    if (!g_stk_reg) return;
    /* 同 leave：持业务锁的 wait 返回后先等 STW，再撤 parked */
    if (__atomic_load_n(&gc_stw_request, __ATOMIC_ACQUIRE)) {
        int64_t t0 = gc_mono_ms();
        uint32_t spins = 0;
        while (!__atomic_load_n(&gc_stw_release, __ATOMIC_ACQUIRE)) {
            spins++;
            if (spins >= GC_PARK_WATCHDOG_SPINS ||
                gc_mono_ms() - t0 >= GC_PARK_WATCHDOG_MS) {
                g_park_watchdog_trips += 1;
                g_mark_abort_cycles += 1;
                gc_stw_leave();
                break;
            }
            gc_yield_brief();
        }
    }
    __atomic_store_n(&g_parked, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_in_os_block, 0, __ATOMIC_RELEASE);
    g_park_sp = NULL;
}

static void gc_scan_shadow(const GcThread* t) {
    if (!t->shadow_slot || !t->shadow_n) return;
    void*** slots = *t->shadow_slot;
    size_t n = *t->shadow_n;
    if (!slots || n == 0) return;
    for (size_t i = 0; i < n; ++i) {
        void** slot = slots[i];
        if (slot && *slot) gc_mark_ptr((uintptr_t)*slot);
    }
}

static void gc_scan_conservative_leaf(char* sp, char* stack_top) {
    if (!sp || !stack_top || sp >= stack_top) return;
    size_t span = (size_t)(stack_top - sp);
    if (span > (size_t)8 * 1024 * 1024) return;
    char* hi = sp + GC_CONSERVATIVE_LEAF_BYTES;
    if (hi > stack_top) hi = stack_top;
    gc_scan_range(sp, hi);
}

static void gc_scan_parked_thread(const GcThread* t) {
    /*
     * 诚实双轨（v0.55.18）：
     * 1) 始终扫 shadow / pins / gpr_spill
     * 2) 始终扫有界 C 叶：LLVM 可能把 GC 指针溅到非 shadow 栈槽；
     *    旧「有 shadow 则跳过叶」在分代修后仍作皮带（假活可接受，UAF 不可）。
     */
    gc_scan_shadow(t);
    if (t->scan_pins && t->scan_pin_n) {
        int n = *t->scan_pin_n;
        if (n > GC_SCAN_PINS) n = GC_SCAN_PINS;
        for (int i = 0; i < n; ++i)
            if (t->scan_pins[i]) gc_mark_ptr((uintptr_t)t->scan_pins[i]);
    }
    if (t->refl_i64_pins && t->refl_i64_pin_n) {
        int n = *t->refl_i64_pin_n;
        if (n > GC_REFL_I64_PINS) n = GC_REFL_I64_PINS;
        for (int i = 0; i < n; ++i)
            if (t->refl_i64_pins[i]) gc_mark_ptr((uintptr_t)t->refl_i64_pins[i]);
    }
    if (t->gpr_spill)
        gc_scan_range(t->gpr_spill, t->gpr_spill + GC_GPR_SPILL_BYTES);
    char* sp = (t->park_sp_slot && *t->park_sp_slot) ? *t->park_sp_slot : NULL;
    gc_scan_conservative_leaf(sp, t->stack_top);
}

size_t hao_gc_root_watermark(void) {
    return g_shadow_n;
}

void hao_gc_root_push(void** slot) {
    if (!slot) return;
    if (!g_stk_reg) gc_register_thread();
    if (g_shadow_n >= g_shadow_cap) {
        size_t nc = g_shadow_cap ? g_shadow_cap * 2 : 64;
        void*** nb = (void***)realloc(g_shadow, nc * sizeof(void**));
        if (!nb) {
            fputs("panic: GC shadow stack 扩容失败\n", stderr);
            exit(1);
        }
        g_shadow = nb;
        g_shadow_cap = nc;
    }
    g_shadow[g_shadow_n++] = slot;
}

void hao_gc_root_unwind(size_t wm) {
    if (wm < g_shadow_n) g_shadow_n = wm;
}

/*
 * 协作 STW（v0.50.4）：时间上限等待 mutator park；禁止 Win SuspendThread。
 * 未齐 → 返回 0（incomplete），由调用方跳过 sweep + pacing；**废除全标活**。
 * 调用方必须已持有 GC 锁；返回时仍持锁。返回 1=齐 park 并可扫，0=未齐。
 */
#define GC_STW_ROUNDS          3
#define GC_STW_ROUND_MS        8     /* 每轮最多等待（对齐低延迟） */
#define GC_STW_TOTAL_MS        64    /* 根 STW 上限（多线程 HTTP 略放宽） */
#define GC_STW_TERM_TOTAL_MS   48    /* 终止 STW 单次上限 */
#define GC_STW_TERM_RETRIES    5     /* 终止握手重试（STW 内不再长 drain） */
#define GC_TERM_COOLDOWN_MS    500   /* alloc 路径触发终止的最小间隔 */
#define GC_NURSERY_GATE_MAX    ((size_t)4 << 20) /* pacing 放大 nursery 上限 4MiB */
#define GC_THRESHOLD_PACE_MAX  ((size_t)64 << 20)

static int64_t gc_mono_ms(void) {
#ifdef _WIN32
    return hao_win_now_ns() / 1000000;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static void gc_sleep_ms(int ms) {
    if (ms <= 0) {
        gc_yield_brief();
        return;
    }
#ifdef _WIN32
    hao_win_sleep_ms((uint32_t)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

static int gc_count_parked(const GcThread* snap, int nsnap, int64_t self,
                           int* out_targets) {
    int targets = 0, parked = 0;
    for (int i = 0; i < nsnap; ++i) {
        if (snap[i].id == self) continue;
        int live = 0;
        for (int j = 0; j < gc_thread_count; ++j)
            if (gc_threads[j].id == snap[i].id) { live = 1; break; }
        if (!live) continue;
        targets++;
        if (snap[i].parked_flag &&
            __atomic_load_n(snap[i].parked_flag, __ATOMIC_ACQUIRE))
            parked++;
    }
    if (out_targets) *out_targets = targets;
    return parked;
}

/* 放行 STW（调用方持 GC 锁） */
static void gc_stw_leave(void) {
    __atomic_store_n(&gc_stw_request, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&gc_stw_release, 1, __ATOMIC_RELEASE);
    gc_yield_brief();
}

static void gc_scanned_tid_add(int64_t id) {
    for (int i = 0; i < gc_scanned_tid_n; ++i)
        if (gc_scanned_tids[i] == id) return;
    if (gc_scanned_tid_n < GC_MAX_THREADS)
        gc_scanned_tids[gc_scanned_tid_n++] = id;
}

static void gc_scanned_tid_clear(void) { gc_scanned_tid_n = 0; }

/* 当前注册线程（除 self）是否都已在本轮扫过栈 */
static int gc_all_threads_scanned(int64_t self) {
    for (int j = 0; j < gc_thread_count; ++j) {
        int64_t id = gc_threads[j].id;
        if (id == self) continue;
        int ok = 0;
        for (int i = 0; i < gc_scanned_tid_n; ++i)
            if (gc_scanned_tids[i] == id) { ok = 1; break; }
        if (!ok) return 0;
    }
    return 1;
}

/*
 * v0.52 软 STW：等到齐或超时；**无论是否齐都扫已 park 者**，保持 request=1。
 * 返回 1=本轮目标已齐 park；0=未齐（计 incomplete，但不 leave、不放弃标记）。
 * total_ms：本轮等待预算。
 */
static int gc_stw_enter_and_scan_soft(int total_ms) {
    int64_t self = gc_os_tid();
    int round;
    int missing = 0;
    int64_t t0 = gc_mono_ms();
    if (total_ms <= 0) total_ms = GC_STW_TOTAL_MS;

    GcThread snap[GC_MAX_THREADS];
    int nsnap = gc_thread_count;
    if (nsnap > GC_MAX_THREADS) nsnap = GC_MAX_THREADS;
    for (int i = 0; i < nsnap; ++i) snap[i] = gc_threads[i];

    int targets = 0;
    for (int i = 0; i < nsnap; ++i)
        if (snap[i].id != self) targets++;
    if (targets == 0) return 1;

    for (round = 0; round < GC_STW_ROUNDS; ++round) {
        if (gc_mono_ms() - t0 >= total_ms) break;

        __atomic_store_n(&gc_stw_release, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&gc_stw_request, 1, __ATOMIC_RELEASE);

        hao_gc_unlock();

#ifndef _WIN32
        for (int i = 0; i < nsnap; ++i) {
            if (snap[i].id == self) continue;
            pthread_kill((pthread_t)snap[i].id, GC_PARK_SIG);
        }
#endif

        {
            int64_t round_start = gc_mono_ms();
            int spins = 0;
            for (;;) {
                int tgt = 0;
                int parked = gc_count_parked(snap, nsnap, self, &tgt);
                if (tgt > 0 && parked >= tgt) break;
                if (gc_mono_ms() - round_start >= GC_STW_ROUND_MS) break;
                if (gc_mono_ms() - t0 >= total_ms) break;
                gc_yield_brief();
                spins++;
                if ((spins & 1023) == 0) gc_sleep_ms(1);
            }
        }

        hao_gc_lock();

        nsnap = gc_thread_count;
        if (nsnap > GC_MAX_THREADS) nsnap = GC_MAX_THREADS;
        for (int i = 0; i < nsnap; ++i) snap[i] = gc_threads[i];

        {
            int tgt = 0;
            int parked = gc_count_parked(snap, nsnap, self, &tgt);
            missing = (tgt > parked) ? (tgt - parked) : 0;
            targets = tgt;
        }

        if (missing == 0 || targets == 0)
            break;

        if (round + 1 >= GC_STW_ROUNDS) break;
        __atomic_store_n(&gc_stw_request, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&gc_stw_release, 1, __ATOMIC_RELEASE);
        hao_gc_unlock();
        gc_sleep_ms(1);
        hao_gc_lock();
        nsnap = gc_thread_count;
        if (nsnap > GC_MAX_THREADS) nsnap = GC_MAX_THREADS;
        for (int i = 0; i < nsnap; ++i) snap[i] = gc_threads[i];
    }

    /* 无论齐否：扫已 park 者并记入 scanned */
    for (int i = 0; i < nsnap; ++i) {
        if (snap[i].id == self) continue;
        int live = 0;
        for (int j = 0; j < gc_thread_count; ++j)
            if (gc_threads[j].id == snap[i].id) { live = 1; break; }
        if (!live) continue;
        if (snap[i].parked_flag &&
            __atomic_load_n(snap[i].parked_flag, __ATOMIC_ACQUIRE)) {
            gc_scan_parked_thread(&snap[i]);
            gc_scanned_tid_add(snap[i].id);
        }
    }

    if (missing != 0 && targets > 0) {
        g_stw_incomplete++;
        return 0;
    }
    return 1;
}

static void gc_apply_incomplete_pacing(void) {
    if (gc_pacing_level < 8) gc_pacing_level += 1;
    size_t mul = (size_t)1 << (gc_pacing_level > 4 ? 4 : gc_pacing_level);
    gc_nursery_gate = GC_NURSERY_THRESHOLD * mul;
    if (gc_nursery_gate > GC_NURSERY_GATE_MAX)
        gc_nursery_gate = GC_NURSERY_GATE_MAX;
    /* 暂缓立刻再进 STW：抬高阈值并清零 nursery 压力计数 */
    if (gc_threshold < (size_t)1 << 20)
        gc_threshold = (size_t)1 << 20;
    gc_threshold *= 2;
    if (gc_threshold > GC_THRESHOLD_PACE_MAX)
        gc_threshold = GC_THRESHOLD_PACE_MAX;
    gc_nursery_alloc = 0;
    /* 保留 gc_allocated，major 仍受阈值约束 */
}

static void gc_worklist_reset(void);
static void gc_bump_mark_epoch(void);

/* v0.53：终止失败则 abort —— 禁止无限 MARK + 黑分配囤不可达对象 */
static void gc_abort_mark_cycle(void) {
    if (__atomic_load_n(&gc_stw_request, __ATOMIC_ACQUIRE))
        gc_stw_leave();
    __atomic_store_n(&gc_phase, GC_PHASE_IDLE, __ATOMIC_RELEASE);
    gc_worklist_reset();
    gc_scanned_tid_clear();
    /* bump epoch：上一轮 marked/黑分配对本轮失效，下次 setup 再 bump 后全白可标 */
    gc_bump_mark_epoch();
    gc_mark_major = 1;
    g_collect_want_major = 1;
    g_mark_abort_cycles += 1;
    gc_apply_incomplete_pacing();
}

static void gc_on_successful_collect(void) {
    if (gc_pacing_level > 0) gc_pacing_level -= 1;
    size_t mul = (size_t)1 << (gc_pacing_level > 4 ? 4 : gc_pacing_level);
    if (mul < 1) mul = 1;
    gc_nursery_gate = GC_NURSERY_THRESHOLD * mul;
    if (gc_nursery_gate < GC_NURSERY_THRESHOLD)
        gc_nursery_gate = GC_NURSERY_THRESHOLD;
    if (gc_nursery_gate > GC_NURSERY_GATE_MAX)
        gc_nursery_gate = GC_NURSERY_GATE_MAX;
}

static char* gc_heap_lo = NULL;
static char* gc_heap_hi = NULL;

/*
 * 页索引：find_block 从 O(堆块数) → O(页内块数)。
 * 二十万块时线性扫是 /api/gc 数百 ms、STW 永远 incomplete 的主因。
 */
#define GC_PAGE_SHIFT 12
#define GC_PAGE_HASH  4096
typedef struct GcPageBucket {
    uintptr_t page;
    GCBlock** blocks;
    size_t n, cap;
    struct GcPageBucket* next;
} GcPageBucket;
static GcPageBucket* gc_page_tab[GC_PAGE_HASH];

static unsigned gc_page_h(uintptr_t page) {
    return (unsigned)((page * 11400714819323198485ull) >> 52) & (GC_PAGE_HASH - 1);
}

static void gc_page_bucket_add_block(GcPageBucket* buck, GCBlock* b) {
    for (size_t i = 0; i < buck->n; ++i)
        if (buck->blocks[i] == b) return;
    if (buck->n >= buck->cap) {
        size_t nc = buck->cap ? buck->cap * 2 : 4;
        GCBlock** nb = (GCBlock**)realloc(buck->blocks, nc * sizeof(GCBlock*));
        if (!nb) { fputs("panic: GC 页索引扩容失败\n", stderr); exit(1); }
        buck->blocks = nb;
        buck->cap = nc;
    }
    buck->blocks[buck->n++] = b;
}

static void gc_page_bucket_del_block(GcPageBucket* buck, GCBlock* b) {
    for (size_t i = 0; i < buck->n; ++i) {
        if (buck->blocks[i] == b) {
            buck->blocks[i] = buck->blocks[--buck->n];
            return;
        }
    }
}

static void gc_page_index_add(GCBlock* b) {
    if (!b) return;
    char* u = (char*)(b + 1);
    if (b->user_size == 0) return;
    uintptr_t p0 = ((uintptr_t)u) >> GC_PAGE_SHIFT;
    uintptr_t p1 = ((uintptr_t)(u + b->user_size - 1)) >> GC_PAGE_SHIFT;
    for (uintptr_t pg = p0; pg <= p1; ++pg) {
        unsigned h = gc_page_h(pg);
        GcPageBucket* buck = gc_page_tab[h];
        while (buck && buck->page != pg) buck = buck->next;
        if (!buck) {
            buck = (GcPageBucket*)calloc(1, sizeof(GcPageBucket));
            if (!buck) { fputs("panic: GC 页桶分配失败\n", stderr); exit(1); }
            buck->page = pg;
            buck->next = gc_page_tab[h];
            gc_page_tab[h] = buck;
        }
        gc_page_bucket_add_block(buck, b);
    }
}

static void gc_page_index_remove(GCBlock* b) {
    if (!b || b->user_size == 0) return;
    char* u = (char*)(b + 1);
    uintptr_t p0 = ((uintptr_t)u) >> GC_PAGE_SHIFT;
    uintptr_t p1 = ((uintptr_t)(u + b->user_size - 1)) >> GC_PAGE_SHIFT;
    for (uintptr_t pg = p0; pg <= p1; ++pg) {
        unsigned h = gc_page_h(pg);
        GcPageBucket* buck = gc_page_tab[h];
        while (buck && buck->page != pg) buck = buck->next;
        if (buck) gc_page_bucket_del_block(buck, b);
    }
}

static GCBlock* gc_find_block(void* p) {
    if (!p) return NULL;
    if ((char*)p < gc_heap_lo || (char*)p >= gc_heap_hi) return NULL;
    uintptr_t pg = ((uintptr_t)p) >> GC_PAGE_SHIFT;
    GcPageBucket* buck = gc_page_tab[gc_page_h(pg)];
    for (; buck; buck = buck->next) {
        if (buck->page != pg) continue;
        for (size_t i = 0; i < buck->n; ++i) {
            GCBlock* b = buck->blocks[i];
            char* u = (char*)(b + 1);
            if ((char*)p >= u && (char*)p <= u + b->user_size) return b;
        }
        return NULL;
    }
    return NULL;
}

/* 用户区起点精确匹配（拒绝 String 载荷内指针 / ASCII 脏值误判） */
static GCBlock* gc_find_block_exact(void* p) {
    GCBlock* b = gc_find_block(p);
    if (!b) return NULL;
    if ((void*)(b + 1) != p) return NULL;
    return b;
}

/* 指标/查询入口：先 safepoint，避免 /api/gc 忙读时永不 park → STW incomplete */
static void gc_lock_coop(void) {
    for (;;) {
        hao_gc_safepoint();
        hao_gc_lock();
        if (__atomic_load_n(&gc_stw_request, __ATOMIC_ACQUIRE)) {
            hao_gc_unlock();
            continue;
        }
        break;
    }
}

int8_t hao_gc_is_heap_ptr(void* p) {
    int8_t ok;
    if (!p) return 0;
    gc_lock_coop();
    ok = gc_find_block(p) ? 1 : 0;
    hao_gc_unlock();
    return ok;
}

int8_t hao_gc_expect_heap_ptr(void* p) {
    int8_t ok;
    if (!p) return 0;
    hao_gc_lock();
    ok = gc_find_block_exact(p) ? 1 : 0;
    hao_gc_unlock();
    return ok;
}

int8_t hao_gc_expect_heap_object(void* p) {
    int8_t ok = 0;
    GCBlock* b;
    if (!p) return 0;
    hao_gc_lock();
    b = gc_find_block_exact(p);
    if (b && (b->scan_kind == GC_KIND_SLOTS || b->scan_kind == GC_KIND_FULL))
        ok = 1;
    hao_gc_unlock();
    return ok;
}

void hao_gc_refl_i64_pin(void* p) {
    GCBlock* b;
    if (!p) return;
    if (!g_stk_reg) gc_register_thread();
    hao_gc_lock();
    b = gc_find_block_exact(p);
    if (!b) {
        hao_gc_unlock();
        return;
    }
    /* 满则退回全局根（极少；避免静默丢钉） */
    if (g_refl_i64_pin_n >= GC_REFL_I64_PINS) {
        hao_gc_unlock();
        hao_gc_add_root(p);
        return;
    }
    g_refl_i64_pins[g_refl_i64_pin_n++] = p;
    hao_gc_unlock();
}

void hao_gc_refl_i64_unpin(void* p) {
    int i;
    if (!p || g_refl_i64_pin_n <= 0) return;
    for (i = g_refl_i64_pin_n - 1; i >= 0; --i) {
        if (g_refl_i64_pins[i] != p) continue;
        g_refl_i64_pins[i] = g_refl_i64_pins[--g_refl_i64_pin_n];
        return;
    }
    /* 溢出路径曾走 add_root */
    hao_gc_remove_root(p);
}

int64_t hao_gc_stw_mark_all_fallbacks(void) {
    /* v0.50.4 起废除全标活；保留符号兼容，恒为 0。请用 hao_gc_stw_incomplete。 */
    (void)g_stw_mark_all_fallbacks;
    return 0;
}

int64_t hao_gc_stw_incomplete(void) {
    int64_t n;
    gc_lock_coop();
    n = g_stw_incomplete;
    hao_gc_unlock();
    return n;
}

int64_t hao_gc_concurrent_mark_cycles(void) {
    int64_t n;
    gc_lock_coop();
    n = g_concurrent_mark_cycles;
    hao_gc_unlock();
    return n;
}

int64_t hao_gc_heap_bytes(void) {
    int64_t n;
    gc_lock_coop();
    n = (int64_t)gc_heap_bytes;
    hao_gc_unlock();
    return n;
}

int64_t hao_gc_mark_assist_steps(void) {
    int64_t n;
    gc_lock_coop();
    n = g_mark_assist_steps;
    hao_gc_unlock();
    return n;
}

void gc_collect_inner(char* regs, size_t regs_size);

#define GC_WORKLIST_CAP 4096
static GCBlock** gc_worklist = NULL;
static size_t gc_wl_count = 0, gc_wl_cap = 0, gc_wl_head = 0;
static void gc_scan_block_precise(GCBlock* b);

static int gc_block_is_marked(const GCBlock* b) {
    return b && b->marked == gc_mark_epoch;
}

static void gc_enqueue(GCBlock* b) {
    if (!b || gc_block_is_marked(b)) return;
    /*
     * minor：old 本轮不回收，但栈根/字段可能指向 old，其 young 子必须入灰。
     * 旧逻辑直接 return → 漏扫 → 下一轮 minor 回收仍被 old 引用的 String → str_len UAF。
     * 用 marked 防 old↔old 环重复扫；不入 worklist。
     */
    if (!gc_mark_major && b->gen != GC_GEN_YOUNG) {
        b->marked = gc_mark_epoch;
        gc_scan_block_precise(b);
        return;
    }
    b->marked = gc_mark_epoch;
    if (gc_wl_count >= gc_wl_cap) {
        gc_wl_cap = gc_wl_cap ? gc_wl_cap * 2 : GC_WORKLIST_CAP;
        gc_worklist = (GCBlock**)realloc(gc_worklist, gc_wl_cap * sizeof(GCBlock*));
        if (!gc_worklist) { fputs("panic: GC 工作集分配失败\n", stderr); exit(1); }
    }
    gc_worklist[gc_wl_count++] = b;
}

static void gc_bump_mark_epoch(void) {
    uint8_t e = (uint8_t)(gc_mark_epoch + 1);
    if (e == 0) e = 1; /* 跳过 0：calloc 初值 0 = 永白于任一 epoch */
    gc_mark_epoch = e;
}

/* 调用方持锁；推进最多 max_steps 个 grey */
static void gc_mark_assist_locked(size_t max_steps) {
    size_t n = 0;
    while (n < max_steps && gc_wl_head < gc_wl_count) {
        GCBlock* b = gc_worklist[gc_wl_head++];
        gc_scan_block_precise(b);
        n++;
        g_mark_assist_steps++;
    }
    if (gc_wl_head >= gc_wl_count) {
        gc_wl_count = 0;
        gc_wl_head = 0;
    }
}

static void gc_mark_ptr(uintptr_t v) {
    if (v == 0) return;
    /* 对象基址 16 对齐，但栈上可能暂存字段内指针（+8）；按指针宽对齐即可。 */
    if ((v & (sizeof(uintptr_t) - 1)) != 0) return;
    gc_enqueue(gc_find_block((void*)v));
}

static void gc_worklist_reset(void) {
    gc_wl_count = 0;
    gc_wl_head = 0;
    free(gc_worklist);
    gc_worklist = NULL;
    gc_wl_cap = 0;
}

/* 调用方持 GC 锁。并发 drain：步数/时间上限 + 周期性放锁（防屏障入灰活锁）。
 * 终止路径请用握手（STW 只 seed/判空，放行后再 drain），勿在 STW 下长抽灰。 */
static void gc_drain_worklist(int concurrent) {
    int64_t t0 = concurrent ? gc_mono_ms() : 0;
    size_t steps = 0;
    while (gc_wl_head < gc_wl_count) {
        GCBlock* b = gc_worklist[gc_wl_head++];
        gc_scan_block_precise(b);
        if (!concurrent) continue;
        steps++;
        if (steps >= GC_CONCURRENT_DRAIN_MAX) break;
        /* 每 16 步放锁，给 GC 私有 worker 抢灰（仅 yield 时同核易饿死） */
        if ((gc_wl_head & 15u) == 0) {
            hao_gc_unlock();
#ifdef _WIN32
            hao_win_sleep_ms(0);
#else
            gc_yield_brief();
#endif
            hao_gc_lock();
            if (gc_mono_ms() - t0 >= GC_CONCURRENT_DRAIN_MS) break;
        }
    }
    if (gc_wl_head >= gc_wl_count) {
        gc_wl_count = 0;
        gc_wl_head = 0;
    }
}

static void gc_seed_roots_and_remset(char* regs, size_t regs_size) {
    for (size_t i = 0; i < gc_root_count; ++i)
        if (gc_roots[i]) gc_mark_ptr((uintptr_t)gc_roots[i]);
    for (size_t i = 0; i < gc_root_slot_count; ++i) {
        void** slot = (void**)gc_root_slots[i];
        if (slot && *slot) gc_mark_ptr((uintptr_t)*slot);
    }
    /* 收集者自身 Hao 精确根 */
    for (size_t i = 0; i < g_shadow_n; ++i) {
        void** slot = g_shadow ? g_shadow[i] : NULL;
        if (slot && *slot) gc_mark_ptr((uintptr_t)*slot);
    }
    for (int i = 0; i < g_scan_pin_n && i < GC_SCAN_PINS; ++i)
        if (g_scan_pins[i]) gc_mark_ptr((uintptr_t)g_scan_pins[i]);
    for (int i = 0; i < g_refl_i64_pin_n && i < GC_REFL_I64_PINS; ++i)
        if (g_refl_i64_pins[i]) gc_mark_ptr((uintptr_t)g_refl_i64_pins[i]);
    /*
     * remset 仅服务 minor：扫 old 容器上的 young 子指针。
     * major 禁止 seed/enqueue remset——否则把不可达 old 当真根，子图永假活
     * （08-gc-monitor blockCount 只涨的主因，v0.55.2）。
     */
    if (!gc_mark_major) {
        for (size_t i = 0; i < gc_remset_count; ++i) {
            GCBlock* b = gc_find_block(gc_remset[i]);
            if (b) gc_scan_block_precise(b);
        }
    }
    /*
     * 收集者：始终扫 collect 入口以下 C 帧；永不扫 trampoline GPR
     * （Hao callee-saved 死指针是 finalizer 假活主因）。
     * 无 shadow 时退回有界叶（纯 C 触发的 collect）。
     */
    (void)regs;
    (void)regs_size;
    {
        char* sp;
#ifdef _WIN32
        sp = (char*)_AddressOfReturnAddress();
#else
        char local; sp = &local;
#endif
        char* hi = g_collect_c_hi;
        if (hi && sp < hi)
            gc_scan_range(sp, hi);
        else if (g_shadow_n == 0)
            gc_scan_conservative_leaf(sp, g_stk_top);
    }
}

/* ---- GC 私有 mark worker（勿复用用户 hao_pool）---- */
static volatile int g_mark_workers_run = 1;
static int g_mark_workers_started = 0;
#ifdef _WIN32
static void* g_mark_worker_handles[GC_MARK_WORKERS];
#else
static pthread_t g_mark_worker_threads[GC_MARK_WORKERS];
#endif

#ifdef _WIN32
static uint32_t mark_worker_main(void* arg) {
#else
static void* mark_worker_main(void* arg) {
#endif
    (void)arg;
    /*
     * GC 私有线程：不 gc_register_thread。
     * 若注册为 mutator，STW 会保守扫其栈/GPR；扫描残留指针会把已死对象假活，
     * 导致 finalizer/回收冒烟偶发失败，并增加 STW 握手目标。
     * 与堆交互一律持 GC 锁；STW 期间仅 park，不碰 worklist。
     */
    while (__atomic_load_n(&g_mark_workers_run, __ATOMIC_ACQUIRE)) {
        if (__atomic_load_n(&gc_stw_request, __ATOMIC_ACQUIRE)) {
            hao_gc_safepoint();
            continue;
        }
        hao_gc_lock();
        if (__atomic_load_n(&gc_stw_request, __ATOMIC_ACQUIRE)) {
            hao_gc_unlock();
            hao_gc_safepoint();
            continue;
        }
        if (__atomic_load_n(&gc_phase, __ATOMIC_ACQUIRE) == GC_PHASE_MARK &&
            gc_wl_head < gc_wl_count) {
            size_t n = 0;
            while (n < 64 && gc_wl_head < gc_wl_count) {
                GCBlock* b = gc_worklist[gc_wl_head++];
                gc_scan_block_precise(b);
                n++;
                g_mark_worker_steps++;
            }
            if (gc_wl_head >= gc_wl_count) {
                gc_wl_count = 0;
                gc_wl_head = 0;
            }
            hao_gc_unlock();
        } else {
            int marking =
                __atomic_load_n(&gc_phase, __ATOMIC_ACQUIRE) == GC_PHASE_MARK;
            hao_gc_unlock();
            /* MARK 期短让出；IDLE 才睡，避免 1ms 睡眠错过协助窗口 */
            if (marking)
                gc_yield_brief();
            else {
#ifdef _WIN32
                hao_win_sleep_ms(1);
#else
                {
                    struct timespec ts;
                    ts.tv_sec = 0;
                    ts.tv_nsec = 1000 * 1000;
                    nanosleep(&ts, NULL);
                }
#endif
            }
        }
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void gc_ensure_mark_workers(void) {
    if (g_mark_workers_started) return;
    g_mark_workers_started = 1;
    for (int i = 0; i < GC_MARK_WORKERS; ++i) {
#ifdef _WIN32
        g_mark_worker_handles[i] = hao_win_create_thread(mark_worker_main, NULL);
#else
        if (pthread_create(&g_mark_worker_threads[i], NULL, mark_worker_main,
                           NULL) != 0)
            g_mark_worker_threads[i] = (pthread_t)0;
#endif
    }
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

/* 持锁：活堆上是否仍有指针指向 user（精确 kind 扫描）。 */
static int gc_block_refers_to(GCBlock* b, void* user) {
    if (!b || !user) return 0;
    char* u = (char*)(b + 1);
    uintptr_t want = (uintptr_t)user;
    switch (b->scan_kind) {
    case GC_KIND_OPAQUE:
        return 0;
    case GC_KIND_SLOTS: {
        uint32_t bm = b->scan_meta;
        size_t nslots = b->user_size / sizeof(uintptr_t);
        if (nslots > 32) nslots = 32;
        for (size_t i = 0; i < nslots; ++i) {
            if ((bm & (1u << i)) && ((uintptr_t*)u)[i] == want) return 1;
        }
        return 0;
    }
    case GC_KIND_ARRAY: {
        if ((b->scan_meta & 1u) == 0) return 0;
        if (b->user_size < (size_t)HAO_ARR_HEADER) return 0;
        char* elems = u + HAO_ARR_HEADER;
        int64_t len = *(int64_t*)(u + HAO_ARR_LEN_OFF_BASE);
        int64_t esz = *(int64_t*)(u + HAO_ARR_ESZ_OFF_BASE);
        if (esz != 8 || len <= 0) return 0;
        size_t max = (b->user_size - (size_t)HAO_ARR_HEADER) / 8;
        if ((size_t)len > max) len = (int64_t)max;
        for (int64_t i = 0; i < len; ++i)
            if (((uintptr_t*)elems)[i] == want) return 1;
        return 0;
    }
    case GC_KIND_FULL:
    default: {
        size_t n = b->user_size / sizeof(uintptr_t);
        for (size_t i = 0; i < n; ++i)
            if (((uintptr_t*)u)[i] == want) return 1;
        return 0;
    }
    }
}

/* 持锁：外部根 / root_slot / 活堆是否仍引用 user（复活判定）。 */
static int gc_user_still_reachable_locked(void* user) {
    if (!user) return 0;
    for (size_t i = 0; i < gc_root_count; ++i)
        if (gc_roots[i] == user) return 1;
    for (size_t i = 0; i < gc_root_slot_count; ++i) {
        void** slot = (void**)gc_root_slots[i];
        if (slot && *slot == user) return 1;
    }
    for (GCBlock* b = gc_heap; b; b = b->next) {
        if (gc_block_refers_to(b, user)) return 1;
    }
    return 0;
}

/* 持锁：把已摘链块挂回堆（finalizer 复活）。 */
static void gc_relink_block_locked(GCBlock* b) {
    if (!b) return;
    b->finalizer = NULL;
    b->gen = GC_GEN_YOUNG;
    b->age = 0;
    b->marked = 0;
    b->next = gc_heap;
    gc_heap = b;
    gc_num_blocks += 1;
    gc_heap_bytes += b->user_size;
    {
        char* u = (char*)(b + 1);
        if (!gc_heap_lo || u < gc_heap_lo) gc_heap_lo = u;
        if (!gc_heap_hi || u + b->user_size > gc_heap_hi)
            gc_heap_hi = u + b->user_size;
    }
    gc_page_index_add(b);
}

/* 回调在锁外执行；队列摘取需短暂加锁防并发 drain 互抢。
 * 回调后若仍可达则挂回堆（完整复活）；否则 free。禁止「挂根仍必 free」。 */
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
            /* 回调期间挂根：防并发 STW 与「栈上仍握 user」窗口踩 free */
            hao_gc_add_root(list[i].user);
            list[i].fn(list[i].user);
            hao_gc_remove_root(list[i].user);
        }
        hao_gc_lock();
        if (gc_user_still_reachable_locked(list[i].user))
            gc_relink_block_locked(list[i].block);
        else
            free(list[i].block);
        hao_gc_unlock();
    }
    free(list);
}

/* ---- 测试辅助：finalizer 复活冒烟（写入 C 静态根槽）---- */
static void* g_fin_rescue = NULL;
static int   g_fin_rescue_slot_reg = 0;

static void gc_fin_rescue_cb(void* user) {
    g_fin_rescue = user;
}

static void gc_fin_nop_cb(void* user) {
    (void)user;
}

void hao_gc_test_arm_rescue_finalizer(void* obj) {
    if (!g_fin_rescue_slot_reg) {
        hao_gc_add_root_slot(&g_fin_rescue);
        g_fin_rescue_slot_reg = 1;
    }
    g_fin_rescue = NULL;
    hao_gc_set_finalizer(obj, gc_fin_rescue_cb);
}

void hao_gc_test_arm_nop_finalizer(void* obj) {
    hao_gc_set_finalizer(obj, gc_fin_nop_cb);
}

void* hao_gc_test_get_rescue(void) {
    return g_fin_rescue;
}

void hao_gc_test_clear_rescue(void) {
    g_fin_rescue = NULL;
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

/* 调用方持 GC 锁：块是否仍握 young 子指针（晋升挂 remset 用） */
static int gc_block_has_young_ptr(GCBlock* b) {
    char* u;
    if (!b || b->scan_kind == GC_KIND_OPAQUE) return 0;
    u = (char*)(b + 1);
    switch (b->scan_kind) {
    case GC_KIND_SLOTS: {
        uint32_t bm = b->scan_meta;
        size_t nslots = b->user_size / sizeof(uintptr_t);
        if (nslots > 32) nslots = 32;
        for (size_t i = 0; i < nslots; ++i) {
            GCBlock* c;
            if (!(bm & (1u << i))) continue;
            c = gc_find_block((void*)((uintptr_t*)u)[i]);
            if (c && c->gen == GC_GEN_YOUNG) return 1;
        }
        return 0;
    }
    case GC_KIND_ARRAY: {
        char* elems;
        int64_t len, esz;
        size_t max;
        if ((b->scan_meta & 1u) == 0) return 0;
        if (b->user_size < (size_t)HAO_ARR_HEADER) return 0;
        elems = u + HAO_ARR_HEADER;
        len = *(int64_t*)(u + HAO_ARR_LEN_OFF_BASE);
        esz = *(int64_t*)(u + HAO_ARR_ESZ_OFF_BASE);
        if (esz != 8 || len <= 0) return 0;
        max = (b->user_size - (size_t)HAO_ARR_HEADER) / 8;
        if ((size_t)len > max) len = (int64_t)max;
        for (int64_t i = 0; i < len; ++i) {
            GCBlock* c = gc_find_block((void*)((uintptr_t*)elems)[i]);
            if (c && c->gen == GC_GEN_YOUNG) return 1;
        }
        return 0;
    }
    case GC_KIND_FULL:
    default: {
        size_t n = b->user_size / sizeof(uintptr_t);
        for (size_t i = 0; i < n; ++i) {
            GCBlock* c = gc_find_block((void*)((uintptr_t*)u)[i]);
            if (c && c->gen == GC_GEN_YOUNG) return 1;
        }
        return 0;
    }
    }
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

/* 丢掉已回收/已无 young 子的边；跨 minor 只保留真 old→young。 */
static void gc_remset_filter_live(void) {
    size_t w = 0;
    for (size_t i = 0; i < gc_remset_count; ++i) {
        GCBlock* b = gc_find_block_exact(gc_remset[i]);
        if (b && b->gen == GC_GEN_OLD && gc_block_has_young_ptr(b))
            gc_remset[w++] = gc_remset[i];
    }
    gc_remset_count = w;
}

void hao_gc_barrier(void* dst, void* new_val) {
    if (!dst) return;
    /*
     * v0.54 混合屏障：dst 须为**槽地址**（先 barrier 再 store）。
     * MARK 期 Yuasa shade(old) + Dijkstra shade(new)；IDLE 仅 remset。
     * STW 重试放锁前须 pin old/new，否则 shadow-only 漏标 → 字符串等 UAF。
     */
    hao_gc_lock();
    for (;;) {
        while (__atomic_load_n(&gc_stw_request, __ATOMIC_ACQUIRE)) {
            void* old_pin = *(void**)dst;
            gc_pin_clear();
            gc_pin_add(new_val);
            gc_pin_add(old_pin);
            hao_gc_unlock();
            hao_gc_safepoint();
            hao_gc_lock();
            gc_pin_clear();
        }
        void* old_val = *(void**)dst;
        GCBlock* db = gc_find_block(dst);
        GCBlock* ob = old_val ? gc_find_block(old_val) : NULL;
        GCBlock* nb = new_val ? gc_find_block(new_val) : NULL;
        /* 分代 remset：old 容器 → young 新值 */
        if (db && nb && db->gen == GC_GEN_OLD && nb->gen == GC_GEN_YOUNG)
            gc_remset_add((void*)(db + 1));
        if (__atomic_load_n(&gc_phase, __ATOMIC_ACQUIRE) == GC_PHASE_MARK) {
            /* 重试窗口可能夹 STW；确认仍 MARK 再 shade */
            if (ob) gc_enqueue(ob);
            if (nb) gc_enqueue(nb);
        }
        /* 若 shade 前又来了 STW 请求，重跑（重新 load old） */
        if (__atomic_load_n(&gc_stw_request, __ATOMIC_ACQUIRE))
            continue;
        break;
    }
    hao_gc_unlock();
}

void hao_gc_shade(void* p) {
    if (!p) return;
    if (__atomic_load_n(&gc_phase, __ATOMIC_ACQUIRE) != GC_PHASE_MARK) return;
    hao_gc_lock();
    while (__atomic_load_n(&gc_stw_request, __ATOMIC_ACQUIRE)) {
        gc_pin_clear();
        gc_pin_add(p);
        hao_gc_unlock();
        hao_gc_safepoint();
        hao_gc_lock();
        gc_pin_clear();
    }
    if (__atomic_load_n(&gc_phase, __ATOMIC_ACQUIRE) == GC_PHASE_MARK) {
        GCBlock* b = gc_find_block(p);
        if (b) gc_enqueue(b);
    }
    hao_gc_unlock();
}

void hao_gc_array_copy_and_shade(char* newbase, const char* oldbase, size_t nbytes,
                                 int64_t len, int is_ptr) {
    if (!newbase || !oldbase) return;
    hao_gc_lock();
    while (__atomic_load_n(&gc_stw_request, __ATOMIC_ACQUIRE)) {
        hao_gc_unlock();
        hao_gc_safepoint();
        hao_gc_lock();
    }
    memcpy(newbase, oldbase, nbytes);
    *(int64_t*)(newbase + 0) = is_ptr ? 2 : 0;
    /* 持锁补 shade：与 memcpy 之间无 safepoint，黑数组子指针不会被漏扫 */
    if (is_ptr && len > 0 &&
        __atomic_load_n(&gc_phase, __ATOMIC_ACQUIRE) == GC_PHASE_MARK) {
        uintptr_t* elems = (uintptr_t*)(newbase + HAO_ARR_HEADER);
        for (int64_t i = 0; i < len; ++i) {
            if (!elems[i]) continue;
            GCBlock* b = gc_find_block((void*)elems[i]);
            if (b) gc_enqueue(b);
        }
    }
    hao_gc_unlock();
}

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
    /*
     * 保守扫上限必须落在 trampoline 之下：naked 垫片把 Hao GPR 溅到栈上，
     * 若 hi 取 gc_collect 帧会扫回死指针 → finalizer 假活。
     */
    char c_hi_frame;
    char* prev_c_hi = g_collect_c_hi;
    g_collect_c_hi = &c_hi_frame;

    int64_t self = gc_os_tid();
    int continuing =
        (__atomic_load_n(&gc_phase, __ATOMIC_ACQUIRE) == GC_PHASE_MARK);
    int major;

    if (!continuing) {
        /* ---- Mark Setup：色纪元 O(1) + 先开屏障 ---- */
        major = g_collect_want_major ? 1 : 0;
        gc_cycle_is_major = major;
        gc_mark_major = major;
        gc_bump_mark_epoch();
        gc_worklist_reset();
        gc_scanned_tid_clear();

        __atomic_store_n(&gc_phase, GC_PHASE_MARK, __ATOMIC_RELEASE);
        g_concurrent_mark_cycles += 1;
        /* mark worker 须在持锁外启动（见 gc_collect / alloc）；此处仅依赖已启动 */

        gc_seed_roots_and_remset(regs, regs_size);
        /*
         * 软根 STW：未齐则 abort，禁止带着漏根进并发 mark。
         * 旧行为「未齐仍 mark」在 accept/recv 的 os_block 竞态下会漏标 → 回收活对象 → 进程直接退出。
         */
        {
            int root_ok = gc_stw_enter_and_scan_soft(GC_STW_TOTAL_MS);
            gc_scanned_tid_add(self);
            if (!root_ok) {
                gc_abort_mark_cycle();
                g_collect_c_hi = prev_c_hi;
                return;
            }
        }
        gc_stw_leave();
        gc_drain_worklist(1);
    } else {
        /* 续跑：仅当仍 MARK（未 abort）；不 bump epoch */
        major = gc_cycle_is_major;
        gc_mark_major = major;
        gc_drain_worklist(1);
    }

    /* ---- Mark Termination：握手 seed/判空；未空则放行后并发 drain 再试 ---- */
    {
        int term_ok = 0;
        for (int attempt = 0; attempt < GC_STW_TERM_RETRIES; ++attempt) {
            gc_scanned_tid_clear();
            int soft_ok = gc_stw_enter_and_scan_soft(GC_STW_TERM_TOTAL_MS);
            gc_scanned_tid_add(self);
            gc_seed_roots_and_remset(regs, regs_size);
            /* 二次 seed：屏障窗口漏灰时第一轮可能假空 */
            if (soft_ok && gc_wl_head >= gc_wl_count)
                gc_seed_roots_and_remset(regs, regs_size);
            if (soft_ok && gc_all_threads_scanned(self) &&
                gc_wl_head >= gc_wl_count) {
                term_ok = 1;
                break;
            }
            gc_stw_leave();
            gc_drain_worklist(1);
            hao_gc_unlock();
            gc_sleep_ms(2);
            hao_gc_lock();
        }
        if (!term_ok) {
            /* 禁止无限 MARK 黑囤：作废本轮色，下一轮从根重来 */
            gc_abort_mark_cycle();
            g_collect_c_hi = prev_c_hi;
            return;
        }
    }
    major = gc_cycle_is_major;

    __atomic_store_n(&gc_phase, GC_PHASE_IDLE, __ATOMIC_RELEASE);
    /*
     * v0.55.15：sweep 期间保持 STW（不中途放锁/不先 leave）。
     * 旧「先 leave 再边扫边放锁」在漏标时与 mutator 并发 free → str_len 等处 0xC0000005。
     * 堆块数通常有限，整段 sweep 持锁停顿可接受。
     */
    void** promoted_buf = NULL;
    size_t promoted_n = 0, promoted_cap = 0;

    gc_in_collect = 1;
    GCBlock** prev = &gc_heap;
    size_t live = 0;
    while (*prev) {
        GCBlock* b = *prev;
        int marked = gc_block_is_marked(b);
        int collect_this = major ? !marked
                                 : (b->gen == GC_GEN_YOUNG && !marked);
        if (!collect_this) {
            if (!major && b->gen == GC_GEN_YOUNG && marked) {
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
            if (gc_num_blocks > 0) gc_num_blocks -= 1;
            if (gc_heap_bytes >= b->user_size)
                gc_heap_bytes -= b->user_size;
            else
                gc_heap_bytes = 0;
            gc_page_index_remove(b);
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
    gc_stw_leave();
    gc_worklist_reset();
    gc_scanned_tid_clear();

    if (major) {
        /* major 已扫全堆：remset 整表丢弃，避免陈旧 old→young 边拖活 */
        gc_remset_count = 0;
    } else {
        /* 只保留仍在堆上的屏障边 */
        gc_remset_filter_live();
        /*
         * 晋升瞬间可能产生 old→young；仅当块内仍握 young 子时挂 remset。
         * 盲目挂全部晋升（含 OPAQUE String）会把 remset 撑到数千（monitor remset≈8007）。
         */
        for (size_t i = 0; i < promoted_n; ++i) {
            GCBlock* pb = gc_find_block(promoted_buf[i]);
            if (pb && gc_block_has_young_ptr(pb))
                gc_remset_add(promoted_buf[i]);
        }
    }
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
    gc_on_successful_collect();
    gc_mark_major = 1;
    g_collect_want_major = 1;
    g_collect_c_hi = prev_c_hi;
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
    char frame;
    char* prev_c_hi = g_collect_c_hi;
    /* 取更靠近 stack_top 的 C 边界（栈向下长） */
    if (!prev_c_hi || &frame > prev_c_hi) g_collect_c_hi = &frame;
    gc_collecting = 1;
    g_collect_want_major = major ? 1 : 0;
    gc_collect_trampoline();
    gc_collecting = 0;
    g_collect_c_hi = prev_c_hi;
}

void gc_collect(void) {
    char collect_frame;
    char* prev_c_hi = g_collect_c_hi;
    g_collect_c_hi = &collect_frame;
    /* 持锁外启动 worker，避免 create_thread→register 与持锁 collect 死锁 */
    gc_ensure_mark_workers();
    for (;;) {
        hao_gc_safepoint();
        hao_gc_lock();
        if (__atomic_load_n(&gc_stw_request, __ATOMIC_ACQUIRE)) {
            hao_gc_unlock();
            continue;
        }
        break;
    }
    /* STW 放锁窗口内禁止重入 collect_inner（否则与协助线程交叉死锁） */
    if (gc_collecting) {
        g_collect_want_major = 1;
        hao_gc_unlock();
        g_collect_c_hi = prev_c_hi;
        return;
    }
    gc_collecting = 1;
    g_collect_want_major = 1;
    gc_collect_trampoline();
    gc_collecting = 0;
    hao_gc_unlock();
    gc_drain_finalizers();
    g_collect_c_hi = prev_c_hi;
}

void hao_gc_add_root(void* p) {
    /*
     * 协作锁 + pin：STW 时须 park（免堵锁），但 shadow-only 扫不到 C 形参 p，
     * 不 pin 会在入 gc_roots 前被 sweep → UAF（v0.55.13 压测 0xC0000005）。
     */
    if (!p) return;
    gc_pin_clear();
    gc_pin_add(p);
    gc_lock_coop();
    if (gc_root_count >= gc_root_cap) {
        gc_root_cap = gc_root_cap ? gc_root_cap * 2 : 16;
        gc_roots = (void**)realloc(gc_roots, gc_root_cap * sizeof(void*));
        if (!gc_roots) {
            gc_pin_clear();
            fputs("panic: GC 根数组分配失败\n", stderr);
            exit(1);
        }
    }
    gc_roots[gc_root_count++] = p;
    gc_pin_clear();
    hao_gc_unlock();
}

void hao_gc_add_root_if_heap(void* p) {
    /* 与 add_root 同锁路径，但先 gc_find_block；禁止走 is_heap_ptr（其内 safepoint） */
    if (!p) return;
    gc_pin_clear();
    gc_pin_add(p);
    gc_lock_coop();
    if (!gc_find_block(p)) {
        gc_pin_clear();
        hao_gc_unlock();
        return;
    }
    if (gc_root_count >= gc_root_cap) {
        gc_root_cap = gc_root_cap ? gc_root_cap * 2 : 16;
        gc_roots = (void**)realloc(gc_roots, gc_root_cap * sizeof(void*));
        if (!gc_roots) {
            gc_pin_clear();
            fputs("panic: GC 根数组分配失败\n", stderr);
            exit(1);
        }
    }
    gc_roots[gc_root_count++] = p;
    gc_pin_clear();
    hao_gc_unlock();
}

void hao_gc_add_root_slot(void* slot) {
    /* 注册前 pin 槽内指针，避免协作 park 窗口漏标 */
    void* pinned = (slot && *(void**)slot) ? *(void**)slot : NULL;
    gc_pin_clear();
    gc_pin_add(pinned);
    gc_lock_coop();
    if (!slot) {
        gc_pin_clear();
        hao_gc_unlock();
        return;
    }
    for (size_t i = 0; i < gc_root_slot_count; ++i) {
        if (gc_root_slots[i] == slot) {
            gc_pin_clear();
            hao_gc_unlock();
            return;
        }
    }
    if (gc_root_slot_count >= gc_root_slot_cap) {
        gc_root_slot_cap = gc_root_slot_cap ? gc_root_slot_cap * 2 : 16;
        gc_root_slots = (void**)realloc(gc_root_slots,
                                        gc_root_slot_cap * sizeof(void*));
        if (!gc_root_slots) {
            gc_pin_clear();
            fputs("panic: GC 根槽数组分配失败\n", stderr);
            exit(1);
        }
    }
    gc_root_slots[gc_root_slot_count++] = slot;
    gc_pin_clear();
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

/* 写入 gc.GcStats：槽 0 vtable；1..19 与 GC.hao 字段声明序一致 */
void hao_gc_stats(void* obj) {
    if (!obj) return;
    int64_t* s = (int64_t*)obj;
    gc_lock_coop();
    s[1] = (int64_t)gc_live;
    s[2] = (int64_t)gc_heap_bytes;
    s[3] = (int64_t)gc_nursery_alloc;
    s[4] = (int64_t)gc_threshold;
    s[5] = (int64_t)gc_allocated;
    s[6] = gc_num_blocks;
    s[7] = gc_collect_count;
    s[8] = gc_minor_count;
    s[9] = gc_major_count;
    s[10] = gc_finalizer_runs;
    s[11] = gc_finalizer_sets;
    s[12] = g_stw_incomplete;
    s[13] = 0; /* stwMarkAllFallbacks 已废除 */
    s[14] = g_concurrent_mark_cycles;
    s[15] = g_mark_assist_steps;
    s[16] = (int64_t)gc_thread_count;
    s[17] = g_mark_abort_cycles;
    s[18] = g_mark_worker_steps;
    s[19] = (int64_t)gc_remset_count; /* v0.55.2 */
    s[20] = g_park_watchdog_trips;    /* v0.55.11：park 超时强行放行 */
    hao_gc_unlock();
}

int64_t hao_gc_remset_count(void) {
    gc_lock_coop();
    int64_t n = (int64_t)gc_remset_count;
    hao_gc_unlock();
    return n;
}

int64_t hao_gc_mark_abort_cycles(void) {
    gc_lock_coop();
    int64_t n = g_mark_abort_cycles;
    hao_gc_unlock();
    return n;
}

int64_t hao_gc_mark_worker_steps(void) {
    gc_lock_coop();
    int64_t n = g_mark_worker_steps;
    hao_gc_unlock();
    return n;
}

int64_t hao_gc_live_bytes(void) {
    gc_lock_coop();
    int64_t v = (int64_t)gc_live;
    hao_gc_unlock();
    return v;
}

int64_t hao_gc_threshold(void) {
    gc_lock_coop();
    int64_t v = (int64_t)gc_threshold;
    hao_gc_unlock();
    return v;
}

int64_t hao_gc_allocated_since(void) {
    gc_lock_coop();
    int64_t v = (int64_t)gc_allocated;
    hao_gc_unlock();
    return v;
}

int64_t hao_gc_block_count(void) {
    gc_lock_coop();
    int64_t n = gc_num_blocks;
    hao_gc_unlock();
    return n;
}

int64_t hao_gc_collect_count(void) {
    gc_lock_coop();
    int64_t v = gc_collect_count;
    hao_gc_unlock();
    return v;
}

int64_t hao_gc_minor_count(void) {
    gc_lock_coop();
    int64_t v = gc_minor_count;
    hao_gc_unlock();
    return v;
}

int64_t hao_gc_major_count(void) {
    gc_lock_coop();
    int64_t v = gc_major_count;
    hao_gc_unlock();
    return v;
}

/* 自上次 minor/major 以来 nursery 累计分配（触发 minor 的压力指标） */
int64_t hao_gc_nursery_bytes(void) {
    gc_lock_coop();
    int64_t v = (int64_t)gc_nursery_alloc;
    hao_gc_unlock();
    return v;
}

/* 已向 GC 注册的线程数（参与 STW 栈扫描；≠ OS 线程总数） */
int64_t hao_gc_registered_threads(void) {
    gc_lock_coop();
    int64_t v = (int64_t)gc_thread_count;
    hao_gc_unlock();
    return v;
}

void hao_gc_remove_root(void* p) {
    /*
     * 禁止协作 safepoint：concat 等在「新串仅活在 C 返回值」窗口摘旧根时，
     * coop → shadow-only 扫不到新串 → sweep → 随后 hao_str_len UAF（v0.55.16）。
     * 摘根临界区极短，不 park 不会单独楔死 STW。
     */
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
#ifdef _WIN32
    /* 尽早挂未处理异常过滤器，避免 monitor 静默退出时无任何日志 */
    (void)hao_win_get_current_thread_id();
#endif
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
    gc_ensure_mark_workers(); /* 持锁外；nursery/阈值触发 collect 也要有 worker */
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

    /* v0.52：标记期 assist；worklist 空时按冷却尝试终止（禁止每 alloc 打满 STW） */
    if (__atomic_load_n(&gc_phase, __ATOMIC_ACQUIRE) == GC_PHASE_MARK) {
        size_t steps = need / 32 + 8;
        if (steps > 128) steps = 128;
        gc_mark_assist_locked(steps);
        if (gc_wl_head >= gc_wl_count) {
            int64_t now = gc_mono_ms();
            if (now - gc_last_term_attempt_ms >= GC_TERM_COOLDOWN_MS) {
                gc_last_term_attempt_ms = now;
                gc_run_collect_locked(gc_cycle_is_major);
            }
        }
    } else if (gc_allocated >= gc_threshold ||
               gc_minors_since_major >= GC_MINORS_BEFORE_MAJOR) {
        gc_run_collect_locked(1);
    } else if (gc_nursery_alloc >= gc_nursery_gate) {
        gc_run_collect_locked(0);
    }

    size_t total = GC_HEADER + need;
    /* calloc/free 不得持 GC 锁：否则其它 mutator 堵在锁上无法 park → STW 永远 incomplete */
    hao_gc_unlock();
    GCBlock* b = (GCBlock*)calloc(1, total);
    if (!b) {
        for (;;) {
            hao_gc_safepoint();
            hao_gc_lock();
            if (__atomic_load_n(&gc_stw_request, __ATOMIC_ACQUIRE)) {
                hao_gc_unlock();
                continue;
            }
            break;
        }
        gc_run_collect_locked(1);
        hao_gc_unlock();
        b = (GCBlock*)calloc(1, total);
        if (!b) { fputs("panic: 内存分配失败\n", stderr); exit(1); }
    }
    for (;;) {
        hao_gc_safepoint();
        hao_gc_lock();
        if (__atomic_load_n(&gc_stw_request, __ATOMIC_ACQUIRE)) {
            hao_gc_unlock();
            continue;
        }
        break;
    }
    b->user_size = need;
    /* 并发标记期黑分配：打上当前 epoch */
    b->marked = (__atomic_load_n(&gc_phase, __ATOMIC_ACQUIRE) == GC_PHASE_MARK)
                    ? gc_mark_epoch
                    : 0;
    b->scan_kind = kind;
    b->scan_meta = (uint32_t)meta;
    b->gen = GC_GEN_YOUNG;
    b->age = 0;
    b->next = gc_heap;
    gc_heap = b;
    gc_num_blocks += 1;
    gc_heap_bytes += need;

    char* u = (char*)(b + 1);
    if (!gc_heap_lo || u < gc_heap_lo) gc_heap_lo = u;
    if (!gc_heap_hi || u + need > gc_heap_hi) gc_heap_hi = u + need;
    gc_page_index_add(b);

    hao_gc_unlock();
    gc_drain_finalizers();
    return u;
}

void* gc_alloc(size_t n) {
    return gc_alloc_ex(n, GC_KIND_FULL, 0);
}
