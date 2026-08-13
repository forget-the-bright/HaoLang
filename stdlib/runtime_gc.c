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
#include <string.h>

#ifdef _WIN32
#include <excpt.h> /* finalizer SEH：__try/__except */
#endif

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
static int gc_test_should_fail_calloc(void);
static void gc_stw_leave(void);
static void gc_sleep_ms(int ms);
static void gc_sweep_yield_locked(const char* tag);
static void gc_hold_slice_begin(void);
static void gc_hold_slice_end(void);
static void gc_stats_fill_into(int64_t* s);
static void gc_stats_publish_locked(void);

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
 * scan_meta：SLOTS=低 32 位图；BITMAP=nslots（位图在槽区尾）；ARRAY=is_ptr。 */
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
static int64_t g_finalizer_exceptions = 0; /* 回调 SEH/隔离吞异常 */
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
/* v0.73 mspan：不再用 CRT exact-size freelist 永囤 */
static GCBlock* g_span_doomed = NULL; /* trampoline 内只摘链；垫片外再 push/free */
static int g_span_doomed_concurrent = 0;
static int64_t  gc_num_blocks = 0; /* O(1); matches heap-list length */
static size_t   gc_allocated = 0;
static size_t   gc_nursery_alloc = 0;
static size_t   gc_threshold = 1 << 20;
static size_t   gc_live = 0;
static int      gc_minors_since_major = 0;
static int64_t  gc_main_tid = 0;
/* v0.50.4：STW 未齐时不再全标活；incomplete + pacing 避免死亡螺旋 */
static int64_t  g_stw_incomplete = 0;
static int64_t  g_stw_incomplete_root = 0; /* v0.55.52：根软 STW 未齐 */
static int64_t  g_stw_incomplete_term = 0; /* v0.55.52：终止软 STW 未齐 */
static int64_t  g_stw_mark_all_fallbacks = 0; /* 兼容旧 API；本版恒 0（已废除全标活） */
/* v0.72：热宽限已删；hao_gc_stw_grace_rescues 恒 0（ABI 兼容） */
static int      gc_pacing_level = 0;          /* 连续 incomplete 退避档位 */
static size_t   gc_nursery_gate = GC_NURSERY_THRESHOLD; /* 可被 pacing 放大 */

/* v0.51+：并发标记相位（mutator 可见；屏障据此 shade） */
#define GC_PHASE_IDLE 0
#define GC_PHASE_MARK 1
static volatile int gc_phase = GC_PHASE_IDLE;
static int64_t      g_concurrent_mark_cycles = 0;
static int64_t      g_concurrent_sweep_cycles = 0; /* 成功 concurrent sweep 轮次（可关） */
static int64_t      g_span_sweep_chunks = 0; /* span 槽回收入队次数 */
static int64_t      g_span_freelist_hits = 0; /* span 槽复用命中 */
static int64_t      g_span_commit_bytes = 0;  /* VirtualAlloc 未 Free 合计 */
static int64_t      g_freelist_bytes = 0;    /* 空闲槽用户区合计 */
static int64_t      g_scavenge_bytes = 0;
static int64_t      g_scavenge_cycles = 0;
static int64_t      g_span_count = 0;
static int64_t      g_last_idle_scavenge_ms = 0;
/* v0.74 GC-LAT-2：分段持锁/发布式 memstats */
#define GC_STATS_SLOTS 64
#define GC_SWEEP_SLICE_MS 2
static int64_t      g_last_collect_hold_ms = 0;
static int64_t      g_last_unlink_ms = 0;
static int64_t      g_last_drain_ms = 0;
static int64_t      g_last_scavenge_ms = 0;
static int64_t      g_last_stats_lock_wait_ms = 0;
static int64_t      g_last_stats_safepoint_ms = 0;
static int64_t      g_last_publish_ms = 0;
static int64_t      g_stats_publish_count = 0;
static int64_t      g_stats_snap[2][GC_STATS_SLOTS];
static volatile int g_stats_snap_i = 0;
static int64_t      g_hold_acc_ms = 0; /* 单次 collect 持大锁累计 */
static int64_t      g_hold_slice_t0 = 0;
static int64_t      g_alloc_bytes_total = 0;
static int64_t      g_freed_bytes_total = 0;
static int64_t      g_stw_wait_ms_total = 0;
static int64_t      g_promote_count = 0;
static int64_t      g_proc_start_ms = 0;
static const char*  g_last_miss_file = NULL;
static int32_t      g_last_miss_line = 0;
static int32_t      g_last_miss_col = 0;
static void*        g_oom_exc = NULL;
/* 测试钩：粘滞至 gc_oom_fail，使 calloc 路径连续失败（含 soft 清后重试） */
static int          g_test_force_oom = 0;
typedef struct {
    void* wr;
    void* referent;
    int   soft;
    int   used;
} GcWeakEnt;
static GcWeakEnt* g_weaks = NULL;
static int        g_weak_n = 0;
static int        g_weak_cap = 0;

static int64_t      g_mark_assist_steps = 0;
static int64_t      g_mark_abort_cycles = 0; /* 终止失败 abort MARK 次数（v0.53.3） */
static int64_t      g_mark_abort_root = 0;   /* v0.55.52 */
static int64_t      g_mark_abort_term = 0;
static int64_t      g_mark_abort_park_wd = 0;
static int64_t      g_mark_worker_steps = 0; /* mark worker 推进灰块数（v0.54） */
static int64_t      g_park_watchdog_trips = 0; /* park 超时 trip 次数（v0.71：不再 mutator leave） */
static volatile int g_stw_watchdog_trip = 0;   /* mutator 置位；仅收集者 leave/abort */
static size_t       gc_heap_bytes = 0; /* 用户区合计，alloc/sweep 维护 */
/* v0.52：marked==gc_mark_epoch 为本轮已标；setup 只 ++epoch，勿扫百万清零 */
static uint8_t      gc_mark_epoch = 1;
static int          gc_mark_major = 1;       /* 扫/入队用：本轮是否 major */
static int          gc_cycle_is_major = 1;   /* 本轮 cycle 固定，续跑不改 */
static int          g_collect_want_major = 1;
static int64_t      gc_last_term_attempt_ms = 0;

/* v0.55.52：软 STW 调用上下文 + 末次未齐快照（sticky） */
#define GC_STW_PHASE_NONE 0
#define GC_STW_PHASE_ROOT 1
#define GC_STW_PHASE_TERM 2
#define GC_ABORT_ROOT     1
#define GC_ABORT_TERM     2
#define GC_ABORT_PARK_WD  3
static int g_stw_soft_phase = GC_STW_PHASE_NONE;
static int g_stw_soft_attempt = 0;
static int g_last_stw_phase = 0;
static int g_last_stw_attempt = 0;
static int g_last_stw_targets = 0;
static int g_last_stw_parked = 0;
static int g_last_stw_missing = 0;
static int g_last_stw_os_block_missing = 0;
/* v0.55.53：末次缺 park 身份 + safepoint 龄 */
#define GC_LAST_MISS_TIDS 8
static int64_t g_last_miss_tids[GC_LAST_MISS_TIDS];
static int     g_last_miss_tid_n = 0;
static int64_t g_last_miss_max_age_ms = -1; /* -1=无样本/从未 SP */
/* v0.55.53：finalizer 发现面 */
static int64_t g_finalizer_skip_abort = 0;   /* abort 跳过 sweep/drain 次数 */
static int64_t g_finalizer_live_at_sweep = 0; /* 上次成功 sweep 时仍存活且带 finalizer 的块数 */
static int     g_last_finalizer_diag = 0;    /* 0=无 1=live 主导 2=abort 主导 */

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
/* v0.55.53：上次到达协作点（safepoint 轮询 / park / os_block）的 mono ms；0=从未 */
static __declspec(thread) int64_t g_last_safepoint_mono_ms = 0;
static __declspec(thread) const char* g_last_hao_file = NULL;
static __declspec(thread) int32_t g_last_hao_line = 0;
static __declspec(thread) int32_t g_last_hao_col = 0;
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
static __thread int64_t g_last_safepoint_mono_ms = 0;
static __thread const char* g_last_hao_file = NULL;
static __thread int32_t g_last_hao_line = 0;
static __thread int32_t g_last_hao_col = 0;
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
    int64_t* last_sp_ms_slot;  /* &g_last_safepoint_mono_ms（v0.55.53） */
    const char** last_src_file; /* 末次 safepoint Hao 源文件 */
    int32_t*     last_src_line;
    int32_t*     last_src_col;
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

static void gc_stw_trip_clear(void) {
    __atomic_store_n(&g_stw_watchdog_trip, 0, __ATOMIC_RELEASE);
}

static int gc_stw_trip_armed(void) {
    return __atomic_load_n(&g_stw_watchdog_trip, __ATOMIC_ACQUIRE) != 0;
}

/* mutator：仅置 trip，禁止 gc_stw_leave（收口归收集者） */
static void gc_stw_trip_set(void) {
    int was = __atomic_exchange_n(&g_stw_watchdog_trip, 1, __ATOMIC_ACQ_REL);
    if (!was) {
        g_park_watchdog_trips += 1;
        hao_trace("gc", "park_wd_trip");
    }
}

/*
 * 等 STW release：首超时 trip；第二超时 fatal（禁静默 leave→UAF）。
 * 保持 parked，直到收集者 leave。
 */
static void gc_mutator_wait_stw_release(void) {
    int64_t t0 = gc_mono_ms();
    uint32_t spins = 0;
    int tripped = 0;
    while (!__atomic_load_n(&gc_stw_release, __ATOMIC_ACQUIRE)) {
        spins++;
        if (spins >= GC_PARK_WATCHDOG_SPINS ||
            gc_mono_ms() - t0 >= GC_PARK_WATCHDOG_MS) {
            if (!tripped) {
                gc_stw_trip_set();
                tripped = 1;
                t0 = gc_mono_ms();
                spins = 0;
                continue;
            }
            hao_report_fatal(
                "stw_wedge",
                "park watchdog 2nd timeout; collector did not leave");
            return;
        }
        gc_yield_brief();
    }
}

static void gc_park_wait(void) {
    char local;
    g_park_sp = &local;
    __atomic_store_n(&g_last_safepoint_mono_ms, gc_mono_ms(), __ATOMIC_RELEASE);
    {
        const char* f = NULL;
        int32_t ln = 0, col = 0;
        hao_dbg_peek_src_loc(&f, &ln, &col);
        g_last_hao_file = f;
        g_last_hao_line = ln;
        g_last_hao_col = col;
    }
    __atomic_store_n(&g_parked, 1, __ATOMIC_RELEASE);
    {
        int64_t t0 = gc_mono_ms();
        gc_mutator_wait_stw_release();
        {
            int64_t w = gc_mono_ms() - t0;
            if (w > 0) g_stw_wait_ms_total += w;
        }
    }
    __atomic_store_n(&g_parked, 0, __ATOMIC_RELEASE);
    g_park_sp = NULL;
}

void hao_gc_safepoint(void) {
    /* v0.55.53：轮询即记协作点龄（即使无 STW request） */
    __atomic_store_n(&g_last_safepoint_mono_ms, gc_mono_ms(), __ATOMIC_RELEASE);
    {
        const char* f = NULL;
        int32_t ln = 0, col = 0;
        hao_dbg_peek_src_loc(&f, &ln, &col);
        g_last_hao_file = f;
        g_last_hao_line = ln;
        g_last_hao_col = col;
    }
    if (!__atomic_load_n(&gc_stw_request, __ATOMIC_ACQUIRE)) return;
    hao_trace("gc", "safepoint park");
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
    gc_threads[gc_thread_count].last_sp_ms_slot = &g_last_safepoint_mono_ms;
    gc_threads[gc_thread_count].last_src_file = &g_last_hao_file;
    gc_threads[gc_thread_count].last_src_line = &g_last_hao_line;
    gc_threads[gc_thread_count].last_src_col = &g_last_hao_col;
    __atomic_store_n(&g_last_safepoint_mono_ms, gc_mono_ms(), __ATOMIC_RELEASE);
    if (g_proc_start_ms == 0) g_proc_start_ms = gc_mono_ms();
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
        gc_mutator_wait_stw_release();
        {
            int64_t w = gc_mono_ms() - t0;
            if (w > 0) g_stw_wait_ms_total += w;
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
    __atomic_store_n(&g_last_safepoint_mono_ms, gc_mono_ms(), __ATOMIC_RELEASE);
    __atomic_store_n(&g_in_os_block, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_parked, 1, __ATOMIC_RELEASE);
}

void hao_gc_os_block_disarm(void) {
    if (!g_stk_reg) return;
    /* 同 leave：持业务锁的 wait 返回后先等 STW，再撤 parked */
    if (__atomic_load_n(&gc_stw_request, __ATOMIC_ACQUIRE)) {
        int64_t t0 = gc_mono_ms();
        gc_mutator_wait_stw_release();
        {
            int64_t w = gc_mono_ms() - t0;
            if (w > 0) g_stw_wait_ms_total += w;
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
 * 协作 STW（v0.72 GC-LAT-1）：fail-fast 短预算；**禁止**再加长（见 gc_stw_budget_gate）。
 * 未齐 → 返回 0（incomplete）；trip → 返回 2（park_wd，由收集者 abort）。
 * v0.71：mutator watchdog 仅置 trip；v0.72：删热宽限，stall 无进展即结束等待。
 * 调用方必须已持有 GC 锁；返回时仍持锁。返回 1=齐，0=未齐，2=watchdog trip。
 */
#define GC_STW_ROUNDS          2
#define GC_STW_ROUND_MS        6     /* 每轮等待上限 */
#define GC_STW_TOTAL_MS        16    /* 根 STW 上限 */
#define GC_STW_TERM_TOTAL_MS   12    /* 终止 STW 单次上限 */
#define GC_STW_TERM_RETRIES    2     /* 终止握手重试 */
#define GC_STW_STALL_MS        4     /* parked 不增连续 stall → 早 abort 等待 */
#define GC_TERM_COOLDOWN_MS    500   /* alloc 路径触发终止的最小间隔 */
#define GC_NURSERY_GATE_MAX    ((size_t)4 << 20) /* pacing 放大 nursery 上限 4MiB */
#define GC_THRESHOLD_PACE_MAX  ((size_t)64 << 20)

static int64_t g_stw_hold_start_ms = 0; /* request=1 起算，leave 时 TRACE */

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
    if (g_stw_hold_start_ms > 0) {
        hao_trace("gc", "stw_hold_ms");
        g_stw_hold_start_ms = 0;
    }
    hao_trace("gc", "stw_leave");
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
 * v0.52/v0.72 软 STW：等到齐、stall 或超时；**无论是否齐都扫已 park 者**，保持 request=1。
 * 返回 1=本轮目标已齐 park；0=未齐；2=park_wd trip。
 * total_ms：本轮等待预算（v0.72 短预算 fail-fast）。
 * 调用前设 g_stw_soft_phase / g_stw_soft_attempt（v0.55.52 分相定位）。
 */
/* Win64：收集线程栈可能未 16B 齐；tid 列表勿走 snprintf/movdqa（v0.55.55） */
static int hao_gc_append_i64(char* buf, int cap, int len, int64_t v) {
    char tmp[24];
    int n = 0;
    uint64_t u;
    if (len < 0) len = 0;
    if (len >= cap) return cap;
    if (v < 0) {
        if (len + 1 >= cap) return len;
        buf[len++] = '-';
        u = (uint64_t)(-(v + 1)) + 1ull;
    } else {
        u = (uint64_t)v;
    }
    do {
        tmp[n++] = (char)('0' + (u % 10));
        u /= 10;
    } while (u && n < (int)sizeof(tmp));
    if (len + n >= cap) n = cap - len - 1;
    if (n < 0) n = 0;
    for (int i = 0; i < n; ++i) buf[len + i] = tmp[n - 1 - i];
    len += n;
    buf[len] = '\0';
    return len;
}


static int hao_gc_append_str(char* buf, int cap, int len, const char* s) {
    if (len < 0) len = 0;
    if (!s) return len;
    while (*s && len + 1 < cap) buf[len++] = *s++;
    if (len < cap) buf[len] = '\0';
    return len;
}

static int hao_gc_append_ptr(char* buf, int cap, int len, const void* p) {
    static const char* hex = "0123456789abcdef";
    uintptr_t u = (uintptr_t)p;
    char tmp[2 * sizeof(uintptr_t)];
    int n = 0;
    len = hao_gc_append_str(buf, cap, len, "0x");
    if (u == 0) return hao_gc_append_str(buf, cap, len, "0");
    while (u && n < (int)sizeof(tmp)) {
        tmp[n++] = hex[u & 15u];
        u >>= 4;
    }
    if (len + n >= cap) n = cap - len - 1;
    if (n < 0) n = 0;
    for (int i = 0; i < n; ++i) buf[len + i] = tmp[n - 1 - i];
    len += n;
    if (len < cap) buf[len] = '\0';
    return len;
}

/* VERIFY fatal：禁 snprintf（对齐 tid 列表手写格式化） */
static void gc_verify_fatal_ip(const char* head, const char* ikey, int64_t idx, void* ptr) {
    char detail[160];
    int len = 0;
    len = hao_gc_append_str(detail, 160, len, head);
    len = hao_gc_append_str(detail, 160, len, " ");
    len = hao_gc_append_str(detail, 160, len, ikey);
    len = hao_gc_append_str(detail, 160, len, "=");
    len = hao_gc_append_i64(detail, 160, len, idx);
    len = hao_gc_append_str(detail, 160, len, " ptr=");
    (void)hao_gc_append_ptr(detail, 160, len, ptr);
    hao_report_fatal("gc_verify", detail);
}

static void gc_record_stw_incomplete(const GcThread* snap, int nsnap, int64_t self,
                                     int targets, int parked, int missing) {
    int os_miss = 0;
    int miss_n = 0;
    int any_sp = 0;
    int64_t miss_tids[GC_LAST_MISS_TIDS];
    int64_t max_age = -1;
    int64_t now = gc_mono_ms();
    char tid_buf[160];
    int tid_len = 0;
    tid_buf[0] = '\0';
    for (int i = 0; i < nsnap; ++i) {
        if (snap[i].id == self) continue;
        int live = 0;
        for (int j = 0; j < gc_thread_count; ++j)
            if (gc_threads[j].id == snap[i].id) { live = 1; break; }
        if (!live) continue;
        int is_parked = snap[i].parked_flag &&
            __atomic_load_n(snap[i].parked_flag, __ATOMIC_ACQUIRE);
        if (is_parked) continue;
        if (snap[i].in_os_block_flag &&
            __atomic_load_n(snap[i].in_os_block_flag, __ATOMIC_ACQUIRE))
            os_miss++;
        if (miss_n < GC_LAST_MISS_TIDS)
            miss_tids[miss_n++] = snap[i].id;
        if (miss_n == 1 && snap[i].last_src_file && *snap[i].last_src_file) {
            g_last_miss_file = *snap[i].last_src_file;
            g_last_miss_line = snap[i].last_src_line ? *snap[i].last_src_line : 0;
            g_last_miss_col = snap[i].last_src_col ? *snap[i].last_src_col : 0;
        }
        if (snap[i].last_sp_ms_slot) {
            int64_t last = __atomic_load_n(snap[i].last_sp_ms_slot, __ATOMIC_ACQUIRE);
            if (last > 0) {
                int64_t age = now - last;
                if (age < 0) age = 0;
                if (!any_sp || age > max_age) max_age = age;
                any_sp = 1;
            }
        }
        if (tid_len < (int)sizeof(tid_buf) - 24) {
            if (tid_len > 0 && tid_len + 1 < (int)sizeof(tid_buf)) {
                tid_buf[tid_len++] = ',';
                tid_buf[tid_len] = '\0';
            }
            tid_len = hao_gc_append_i64(tid_buf, (int)sizeof(tid_buf), tid_len,
                                          snap[i].id);
        }
    }
    if (!any_sp) max_age = -1;
    g_last_stw_phase = g_stw_soft_phase;
    g_last_stw_attempt = g_stw_soft_attempt;
    g_last_stw_targets = targets;
    g_last_stw_parked = parked;
    g_last_stw_missing = missing;
    g_last_stw_os_block_missing = os_miss;
    g_last_miss_tid_n = miss_n;
    for (int k = 0; k < miss_n; ++k)
        g_last_miss_tids[k] = miss_tids[k];
    for (int k = miss_n; k < GC_LAST_MISS_TIDS; ++k)
        g_last_miss_tids[k] = 0;
    g_last_miss_max_age_ms = max_age;
    hao_trace("gc",
              "stw_incomplete phase=%s attempt=%d missing=%d targets=%d parked=%d "
              "os_block=%d miss_tids=%s miss_age_ms=%lld",
              g_stw_soft_phase == GC_STW_PHASE_ROOT ? "root"
                  : (g_stw_soft_phase == GC_STW_PHASE_TERM ? "term" : "?"),
              g_stw_soft_attempt, missing, targets, parked, os_miss,
              tid_buf[0] ? tid_buf : "-",
              (long long)max_age);
}

/* 返回：1=齐；0=incomplete；2=park_wd trip（勿计 incomplete） */
static int gc_stw_enter_and_scan_soft(int total_ms) {
    int64_t self = gc_os_tid();
    int round;
    int missing = 0;
    int parked_now = 0;
    int64_t t0 = gc_mono_ms();
    if (total_ms <= 0) total_ms = GC_STW_TOTAL_MS;

    hao_trace("gc", "stw_enter");
    hao_trace("gc", "stw_budget_cap");

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
        if (gc_stw_trip_armed()) {
            hao_trace("gc", "stw_miss reason=park_wd_trip");
            return 2;
        }

        __atomic_store_n(&gc_stw_release, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&gc_stw_request, 1, __ATOMIC_RELEASE);
        if (g_stw_hold_start_ms == 0)
            g_stw_hold_start_ms = gc_mono_ms();

        gc_hold_slice_end();
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
            int last_parked = -1;
            int64_t stall_since = round_start;
            for (;;) {
                if (gc_stw_trip_armed()) {
                    hao_gc_lock();
                    gc_hold_slice_begin();
                    hao_trace("gc", "stw_miss reason=park_wd_trip");
                    return 2;
                }
                int tgt = 0;
                int parked = gc_count_parked(snap, nsnap, self, &tgt);
                if (tgt > 0 && parked >= tgt) break;
                if (parked != last_parked) {
                    last_parked = parked;
                    stall_since = gc_mono_ms();
                } else if (gc_mono_ms() - stall_since >= GC_STW_STALL_MS) {
                    hao_trace("gc", "stw_stall_abort");
                    break;
                }
                if (gc_mono_ms() - round_start >= GC_STW_ROUND_MS) break;
                if (gc_mono_ms() - t0 >= total_ms) break;
                gc_yield_brief();
                spins++;
                if ((spins & 1023) == 0) gc_sleep_ms(1);
            }
        }

        hao_gc_lock();
        gc_hold_slice_begin();

        nsnap = gc_thread_count;
        if (nsnap > GC_MAX_THREADS) nsnap = GC_MAX_THREADS;
        for (int i = 0; i < nsnap; ++i) snap[i] = gc_threads[i];

        {
            int tgt = 0;
            parked_now = gc_count_parked(snap, nsnap, self, &tgt);
            missing = (tgt > parked_now) ? (tgt - parked_now) : 0;
            targets = tgt;
        }

        if (missing == 0 || targets == 0)
            break;

        if (round + 1 >= GC_STW_ROUNDS) break;
        /* 跨轮保持 stw_request，避免放行后已 park 线程跑飞再 miss */
        gc_hold_slice_end();
        hao_gc_unlock();
        gc_yield_brief();
        hao_gc_lock();
        gc_hold_slice_begin();
        nsnap = gc_thread_count;
        if (nsnap > GC_MAX_THREADS) nsnap = GC_MAX_THREADS;
        for (int i = 0; i < nsnap; ++i) snap[i] = gc_threads[i];
    }

    if (gc_stw_trip_armed()) {
        hao_trace("gc", "stw_miss reason=park_wd_trip");
        return 2;
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
        /* v0.72：无热宽限；未齐即 incomplete（fail-fast） */
        g_stw_incomplete++;
        if (g_stw_soft_phase == GC_STW_PHASE_ROOT)
            g_stw_incomplete_root++;
        else if (g_stw_soft_phase == GC_STW_PHASE_TERM)
            g_stw_incomplete_term++;
        gc_record_stw_incomplete(snap, nsnap, self, targets, parked_now, missing);
        hao_trace("gc", "stw_miss reason=incomplete");
        return 0;
    }
    return 1;
}

static int gc_consecutive_aborts = 0; /* v0.71：连续 abort 才抬 pacing */

static void gc_apply_incomplete_pacing(int reason) {
    /* park_wd：收口 STW，不叠阈值倍增（避免握手税打成堆胀） */
    if (reason == GC_ABORT_PARK_WD) {
        gc_nursery_alloc = 0;
        return;
    }
    gc_consecutive_aborts += 1;
    /* 单次握手税不抬档；连续 abort（≥2）才升级 */
    if (gc_consecutive_aborts < 2) {
        gc_nursery_alloc = 0;
        return;
    }
    if (gc_pacing_level < 8) gc_pacing_level += 1;
    size_t mul = (size_t)1 << (gc_pacing_level > 4 ? 4 : gc_pacing_level);
    gc_nursery_gate = GC_NURSERY_THRESHOLD * mul;
    if (gc_nursery_gate > GC_NURSERY_GATE_MAX)
        gc_nursery_gate = GC_NURSERY_GATE_MAX;
    if (gc_threshold < (size_t)1 << 20)
        gc_threshold = (size_t)1 << 20;
    gc_threshold *= 2;
    if (gc_threshold > GC_THRESHOLD_PACE_MAX)
        gc_threshold = GC_THRESHOLD_PACE_MAX;
    gc_nursery_alloc = 0;
}

static void gc_worklist_reset(void);
static void gc_bump_mark_epoch(void);

/* v0.53：终止失败则 abort —— 禁止无限 MARK + 黑分配囤不可达对象 */
static void gc_abort_mark_cycle(int reason) {
    if (__atomic_load_n(&gc_stw_request, __ATOMIC_ACQUIRE))
        gc_stw_leave();
    gc_stw_trip_clear();
    __atomic_store_n(&gc_phase, GC_PHASE_IDLE, __ATOMIC_RELEASE);
    gc_worklist_reset();
    gc_scanned_tid_clear();
    /* bump epoch：上一轮 marked/黑分配对本轮失效，下次 setup 再 bump 后全白可标 */
    gc_bump_mark_epoch();
    gc_mark_major = 1;
    g_collect_want_major = 1;
    g_mark_abort_cycles += 1;
    if (reason == GC_ABORT_ROOT)
        g_mark_abort_root += 1;
    else if (reason == GC_ABORT_TERM)
        g_mark_abort_term += 1;
    else if (reason == GC_ABORT_PARK_WD)
        g_mark_abort_park_wd += 1;
    hao_trace("gc", "mark_abort reason=%s",
              reason == GC_ABORT_ROOT ? "root"
                  : (reason == GC_ABORT_TERM ? "term"
                         : (reason == GC_ABORT_PARK_WD ? "park_wd" : "?")));
    g_finalizer_skip_abort += 1;
    if (g_finalizer_skip_abort >= g_finalizer_live_at_sweep)
        g_last_finalizer_diag = 2; /* abort 主导可见 */
    hao_trace("gc", "finalizer_skip reason=abort count=%lld",
              (long long)g_finalizer_skip_abort);
    gc_apply_incomplete_pacing(reason);
}

static void gc_on_successful_collect(void) {
    /* 成功 major：清零连续 abort 与 pacing 档 */
    gc_consecutive_aborts = 0;
    gc_pacing_level = 0;
    gc_nursery_gate = GC_NURSERY_THRESHOLD;
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

#define GC_SPAN_NCLASS 8
#define GC_SPAN_LOS ((size_t)1 << 16)
#define GC_MSPAGE_SIZE ((size_t)8192)
#define GC_MSPAN_BYTES ((size_t)256 * 1024) /* 32 页；门禁常量 */
#define GC_FREELIST_SOFT_MAX ((size_t)2 << 20)
#define GC_SPAN_PAGE_HASH 4096

typedef struct MSpan {
    char* start;
    size_t nbytes;
    size_t elemsize;
    size_t nelems;
    int sizeclass; /* -1 = LOS */
    uint8_t* allocBits;
    uint8_t* markBits;
    size_t free_count;
    size_t alloc_count;
    struct MSpan* next; /* per-class 或 LOS 链 */
} MSpan;

static MSpan* g_mspan_class[GC_SPAN_NCLASS];
static MSpan* g_mspan_los;

typedef struct SpanPageEnt {
    uintptr_t page;
    MSpan* span;
    struct SpanPageEnt* next;
} SpanPageEnt;
static SpanPageEnt* g_span_page_tab[GC_SPAN_PAGE_HASH];

static size_t gc_span_class_size(int sc) {
    static const size_t k[GC_SPAN_NCLASS] = {
        64, 128, 256, 512, 1024, 2048, 4096, 65536
    };
    if (sc < 0 || sc >= GC_SPAN_NCLASS) return 0;
    return k[sc];
}

static int gc_span_class_of(size_t need) {
    if (need == 0 || need > GC_SPAN_LOS) return -1;
    if (need <= 64) return 0;
    if (need <= 128) return 1;
    if (need <= 256) return 2;
    if (need <= 512) return 3;
    if (need <= 1024) return 4;
    if (need <= 2048) return 5;
    if (need <= 4096) return 6;
    return 7;
}

static int gc_bit_test(const uint8_t* bits, size_t i) {
    return (bits[i >> 3] >> (i & 7)) & 1;
}
static void gc_bit_set(uint8_t* bits, size_t i) {
    bits[i >> 3] |= (uint8_t)(1u << (i & 7));
}
static void gc_bit_clear(uint8_t* bits, size_t i) {
    bits[i >> 3] &= (uint8_t)~(1u << (i & 7));
}

static void gc_span_page_register(MSpan* sp) {
    uintptr_t base, end, pg;
    if (!sp || !sp->start) return;
    base = (uintptr_t)sp->start / GC_MSPAGE_SIZE;
    end = ((uintptr_t)sp->start + sp->nbytes + GC_MSPAGE_SIZE - 1) / GC_MSPAGE_SIZE;
    for (pg = base; pg < end; ++pg) {
        SpanPageEnt* e = (SpanPageEnt*)calloc(1, sizeof(SpanPageEnt));
        size_t h;
        if (!e) {
            fputs("panic: GC span page map alloc failed\n", stderr);
            exit(1);
        }
        e->page = pg;
        e->span = sp;
        h = (size_t)(pg & (GC_SPAN_PAGE_HASH - 1));
        e->next = g_span_page_tab[h];
        g_span_page_tab[h] = e;
    }
}

static void gc_span_page_unregister(MSpan* sp) {
    uintptr_t base, end, pg;
    if (!sp || !sp->start) return;
    base = (uintptr_t)sp->start / GC_MSPAGE_SIZE;
    end = ((uintptr_t)sp->start + sp->nbytes + GC_MSPAGE_SIZE - 1) / GC_MSPAGE_SIZE;
    for (pg = base; pg < end; ++pg) {
        size_t h = (size_t)(pg & (GC_SPAN_PAGE_HASH - 1));
        SpanPageEnt** pp = &g_span_page_tab[h];
        while (*pp) {
            if ((*pp)->page == pg && (*pp)->span == sp) {
                SpanPageEnt* dead = *pp;
                *pp = dead->next;
                free(dead);
                break;
            }
            pp = &(*pp)->next;
        }
    }
}

static MSpan* gc_span_of_ptr(void* p) {
    uintptr_t pg;
    size_t h;
    SpanPageEnt* e;
    if (!p) return NULL;
    pg = (uintptr_t)p / GC_MSPAGE_SIZE;
    h = (size_t)(pg & (GC_SPAN_PAGE_HASH - 1));
    for (e = g_span_page_tab[h]; e; e = e->next)
        if (e->page == pg) return e->span;
    return NULL;
}

static void gc_mspan_destroy_locked(MSpan* sp) {
    size_t nbytes;
    if (!sp) return;
    hao_trace("gc", "span_free");
    gc_span_page_unregister(sp);
    nbytes = sp->nbytes;
    if (sp->start) {
        hao_os_vfree(sp->start, nbytes);
        sp->start = NULL;
    }
    if (g_span_commit_bytes >= (int64_t)nbytes)
        g_span_commit_bytes -= (int64_t)nbytes;
    else
        g_span_commit_bytes = 0;
    g_scavenge_bytes += (int64_t)nbytes;
    if (g_span_count > 0) g_span_count -= 1;
    free(sp->allocBits);
    free(sp->markBits);
    free(sp);
}

static void gc_mspan_unlink_locked(MSpan* sp) {
    MSpan** head;
    MSpan** pp;
    if (!sp) return;
    if (sp->sizeclass < 0)
        head = &g_mspan_los;
    else
        head = &g_mspan_class[sp->sizeclass];
    pp = head;
    while (*pp) {
        if (*pp == sp) {
            *pp = sp->next;
            sp->next = NULL;
            return;
        }
        pp = &(*pp)->next;
    }
}

/* 空 span 立刻还 OS；部分空闲计入 freelistBytes */
static void gc_mspan_maybe_release_locked(MSpan* sp) {
    size_t free_user;
    if (!sp) return;
    if (sp->free_count < sp->nelems) {
        free_user = sp->free_count * (sp->elemsize > GC_HEADER
                                          ? sp->elemsize - GC_HEADER
                                          : 0);
        /* freelist_bytes 在 push/pop 增量维护，这里不重算全表 */
        return;
    }
    free_user = sp->nelems * (sp->elemsize > GC_HEADER ? sp->elemsize - GC_HEADER : 0);
    if (g_freelist_bytes >= (int64_t)free_user)
        g_freelist_bytes -= (int64_t)free_user;
    else
        g_freelist_bytes = 0;
    gc_mspan_unlink_locked(sp);
    gc_mspan_destroy_locked(sp);
    g_scavenge_cycles += 1;
    hao_trace("gc", "scavenge");
}

static MSpan* gc_mspan_create_locked(int sc, size_t need) {
    MSpan* sp;
    size_t class_sz, elemsize, nbytes, nelems, bits_bytes;
    char* mem;
    size_t i;
    if (gc_test_should_fail_calloc()) return NULL;
    sp = (MSpan*)calloc(1, sizeof(MSpan));
    if (!sp) return NULL;
    if (sc < 0) {
        elemsize = GC_HEADER + need;
        nbytes = (elemsize + GC_MSPAGE_SIZE - 1) & ~(GC_MSPAGE_SIZE - 1);
        nelems = 1;
        sp->sizeclass = -1;
    } else {
        class_sz = gc_span_class_size(sc);
        elemsize = GC_HEADER + class_sz;
        nbytes = GC_MSPAN_BYTES;
        nelems = nbytes / elemsize;
        if (nelems < 1) {
            nbytes = (elemsize + GC_MSPAGE_SIZE - 1) & ~(GC_MSPAGE_SIZE - 1);
            nelems = 1;
        }
        sp->sizeclass = sc;
    }
    hao_gc_unlock();
    mem = (char*)hao_os_valloc(nbytes);
    hao_gc_lock();
    if (!mem) {
        free(sp);
        return NULL;
    }
    bits_bytes = (nelems + 7) / 8;
    sp->allocBits = (uint8_t*)calloc(1, bits_bytes);
    sp->markBits = (uint8_t*)calloc(1, bits_bytes);
    if (!sp->allocBits || !sp->markBits) {
        hao_os_vfree(mem, nbytes);
        free(sp->allocBits);
        free(sp->markBits);
        free(sp);
        return NULL;
    }
    sp->start = mem;
    sp->nbytes = nbytes;
    sp->elemsize = elemsize;
    sp->nelems = nelems;
    sp->free_count = nelems;
    sp->alloc_count = 0;
    for (i = 0; i < nelems; ++i) {
        GCBlock* b = (GCBlock*)(mem + i * elemsize);
        b->user_size = 0;
        b->next = NULL;
        b->finalizer = NULL;
        b->marked = 0;
        b->scan_kind = GC_KIND_OPAQUE;
        b->scan_meta = 0;
        b->gen = GC_GEN_YOUNG;
        b->age = 0;
    }
    g_span_commit_bytes += (int64_t)nbytes;
    g_freelist_bytes += (int64_t)(nelems * (elemsize - GC_HEADER));
    g_span_count += 1;
    gc_span_page_register(sp);
    if (sc < 0) {
        sp->next = g_mspan_los;
        g_mspan_los = sp;
    } else {
        sp->next = g_mspan_class[sc];
        g_mspan_class[sc] = sp;
    }
    hao_trace("gc", "span_new");
    return sp;
}

static void gc_span_ensure(void) {
    /* mspan 惰性：无全局 calloc freelist 头 */
}

static void gc_span_push_locked(GCBlock* b) {
    MSpan* sp;
    size_t idx;
    size_t user_part;
    if (!b) return;
    sp = gc_span_of_ptr(b);
    if (!sp) {
        /* 遗留 CRT 块（不应再出现）：直接 free */
        hao_trace("gc", "span_free");
        free(b);
        return;
    }
    if (sp->elemsize == 0) {
        hao_trace("gc", "bitmap_bad");
        hao_report_fatal("gc_verify", "span elemsize=0");
        return;
    }
    idx = (size_t)(((char*)b - sp->start) / sp->elemsize);
    if (idx >= sp->nelems || (char*)b != sp->start + idx * sp->elemsize) {
        hao_trace("gc", "bitmap_bad");
        hao_report_fatal("gc_verify", "span slot misaligned");
        return;
    }
    if (!gc_bit_test(sp->allocBits, idx)) {
        hao_trace("gc", "bitmap_bad");
        hao_report_fatal("gc_verify", "double free span slot");
        return;
    }
    {
        char* u = (char*)(b + 1);
        size_t i, lim = sp->elemsize - GC_HEADER;
        for (i = 0; i < lim; ++i) u[i] = 0;
    }
    b->finalizer = NULL;
    b->marked = 0;
    b->scan_kind = GC_KIND_OPAQUE;
    b->scan_meta = 0;
    b->gen = GC_GEN_YOUNG;
    b->age = 0;
    b->user_size = 0;
    b->next = NULL;
    gc_bit_clear(sp->allocBits, idx);
    gc_bit_clear(sp->markBits, idx);
    sp->free_count += 1;
    if (sp->alloc_count > 0) sp->alloc_count -= 1;
    user_part = sp->elemsize - GC_HEADER;
    g_freelist_bytes += (int64_t)user_part;
    g_span_sweep_chunks += 1;
    gc_mspan_maybe_release_locked(sp);
}

static GCBlock* gc_span_pop_locked(size_t need) {
    int sc = gc_span_class_of(need);
    MSpan* sp;
    size_t i;
    if (sc < 0) return NULL;
    for (sp = g_mspan_class[sc]; sp; sp = sp->next) {
        if (sp->free_count == 0) continue;
        for (i = 0; i < sp->nelems; ++i) {
            if (gc_bit_test(sp->allocBits, i)) continue;
            {
                GCBlock* b = (GCBlock*)(sp->start + i * sp->elemsize);
                size_t user_part = sp->elemsize - GC_HEADER;
                gc_bit_set(sp->allocBits, i);
                gc_bit_clear(sp->markBits, i);
                sp->free_count -= 1;
                sp->alloc_count += 1;
                if (g_freelist_bytes >= (int64_t)user_part)
                    g_freelist_bytes -= (int64_t)user_part;
                else
                    g_freelist_bytes = 0;
                g_span_freelist_hits += 1;
                hao_trace("gc", "span_alloc");
                return b;
            }
        }
    }
    sp = gc_mspan_create_locked(sc, need);
    if (!sp || sp->free_count == 0) return NULL;
    for (i = 0; i < sp->nelems; ++i) {
        if (gc_bit_test(sp->allocBits, i)) continue;
        {
            GCBlock* b = (GCBlock*)(sp->start + i * sp->elemsize);
            size_t user_part = sp->elemsize - GC_HEADER;
            gc_bit_set(sp->allocBits, i);
            sp->free_count -= 1;
            sp->alloc_count += 1;
            if (g_freelist_bytes >= (int64_t)user_part)
                g_freelist_bytes -= (int64_t)user_part;
            else
                g_freelist_bytes = 0;
            g_span_freelist_hits += 1;
            hao_trace("gc", "span_alloc");
            return b;
        }
    }
    return NULL;
}

static GCBlock* gc_mspan_alloc_los_locked(size_t need) {
    MSpan* sp = gc_mspan_create_locked(-1, need);
    GCBlock* b;
    size_t user_part;
    if (!sp) return NULL;
    b = (GCBlock*)sp->start;
    gc_bit_set(sp->allocBits, 0);
    sp->free_count = 0;
    sp->alloc_count = 1;
    user_part = sp->elemsize - GC_HEADER;
    if (g_freelist_bytes >= (int64_t)user_part)
        g_freelist_bytes -= (int64_t)user_part;
    else
        g_freelist_bytes = 0;
    hao_trace("gc", "span_alloc");
    return b;
}

/* reason: 0=major 1=idle 2=urgent — 拆空闲过多的 span（空板已在 push 时还） */
static void gc_mspan_scavenge_locked(int reason) {
    int sc;
    size_t soft = GC_FREELIST_SOFT_MAX;
    size_t target;
    int64_t t0 = gc_mono_ms();
    int64_t slice_t0 = t0;
    int freed = 0;
    (void)reason;
    if (gc_threshold * 2 > soft) soft = gc_threshold * 2;
    target = soft / 2;
    hao_trace("gc", "scavenge");
    g_scavenge_cycles += 1;
    /* 空 span 已在 push 释放；此处压掉「半空过多」：从各类摘 free_count 高的整板 */
    for (sc = 0; sc < GC_SPAN_NCLASS && (size_t)g_freelist_bytes > target; ++sc) {
        MSpan** pp = &g_mspan_class[sc];
        while (*pp && (size_t)g_freelist_bytes > target) {
            MSpan* sp = *pp;
            if (sp->alloc_count == 0 && sp->free_count == sp->nelems) {
                *pp = sp->next;
                sp->next = NULL;
                {
                    size_t free_user =
                        sp->nelems * (sp->elemsize > GC_HEADER ? sp->elemsize - GC_HEADER
                                                               : 0);
                    if (g_freelist_bytes >= (int64_t)free_user)
                        g_freelist_bytes -= (int64_t)free_user;
                    else
                        g_freelist_bytes = 0;
                }
                gc_mspan_destroy_locked(sp);
                freed += 1;
                if ((freed % 2) == 0 ||
                    (gc_mono_ms() - slice_t0) >= GC_SWEEP_SLICE_MS) {
                    gc_sweep_yield_locked("scavenge");
                    slice_t0 = gc_mono_ms();
                }
                continue;
            }
            /* 半空：若空闲比例高且超水位，仍保留（P2 可合并）；P1 只还全空 */
            pp = &(*pp)->next;
        }
    }
    g_last_idle_scavenge_ms = gc_mono_ms();
    g_last_scavenge_ms = g_last_idle_scavenge_ms - t0;
}

static void gc_mspan_mark_bit_for_block(GCBlock* b) {
    MSpan* sp;
    size_t idx;
    if (!b) return;
    sp = gc_span_of_ptr(b);
    if (!sp || !sp->markBits) return;
    idx = (size_t)(((char*)b - sp->start) / sp->elemsize);
    if (idx < sp->nelems) gc_bit_set(sp->markBits, idx);
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

/* v0.72：只读计数 — 一次 safepoint 后直接持锁拷贝，不再空等 stw_request leave */
static void gc_lock_stats_ro(void) {
    hao_gc_safepoint();
    hao_gc_lock();
}

/* LAT-2：持锁累计（unlock 窗口外计入 lastCollectHoldMs） */
static void gc_hold_slice_begin(void) { g_hold_slice_t0 = gc_mono_ms(); }
static void gc_hold_slice_end(void) {
    if (g_hold_slice_t0 > 0) {
        g_hold_acc_ms += gc_mono_ms() - g_hold_slice_t0;
        g_hold_slice_t0 = 0;
    }
}
static void gc_sweep_yield_locked(const char* tag) {
    hao_trace("gc", tag);
    gc_hold_slice_end();
    hao_gc_unlock();
    gc_sleep_ms(0);
    hao_gc_lock();
    gc_hold_slice_begin();
}

static void gc_stats_publish_locked(void) {
    int64_t t0 = gc_mono_ms();
    int next = 1 - __atomic_load_n(&g_stats_snap_i, __ATOMIC_RELAXED);
    if (next < 0 || next > 1) next = 0;
    memset(g_stats_snap[next], 0, sizeof(g_stats_snap[next]));
    gc_stats_fill_into(g_stats_snap[next]);
    __atomic_store_n(&g_stats_snap_i, next, __ATOMIC_RELEASE);
    g_stats_publish_count += 1;
    g_last_publish_ms = gc_mono_ms() - t0;
    hao_trace("gc", "stats_publish");
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
    if (b && (b->scan_kind == GC_KIND_SLOTS || b->scan_kind == GC_KIND_BITMAP))
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
    gc_lock_stats_ro();
    n = g_stw_incomplete;
    hao_gc_unlock();
    return n;
}

int64_t hao_gc_stw_grace_rescues(void) {
    /* v0.72：热宽限删除；ABI 保留，恒 0 */
    return 0;
}

int64_t hao_gc_concurrent_mark_cycles(void) {
    int64_t n;
    gc_lock_stats_ro();
    n = g_concurrent_mark_cycles;
    hao_gc_unlock();
    return n;
}

int64_t hao_gc_concurrent_sweep_cycles(void) {
    int64_t n;
    gc_lock_stats_ro();
    n = g_concurrent_sweep_cycles;
    hao_gc_unlock();
    return n;
}

int64_t hao_gc_span_sweep_chunks(void) {
    int64_t n;
    gc_lock_stats_ro();
    n = g_span_sweep_chunks;
    hao_gc_unlock();
    return n;
}

int64_t hao_gc_freelist_hits(void) {
    int64_t n;
    gc_lock_stats_ro();
    n = g_span_freelist_hits;
    hao_gc_unlock();
    return n;
}

int64_t hao_gc_heap_bytes(void) {
    int64_t n;
    gc_lock_stats_ro();
    n = (int64_t)gc_heap_bytes;
    hao_gc_unlock();
    return n;
}

/* fatal/崩溃路径：无锁尽力读；禁止再抢 GC 锁（防死锁） */
void hao_gc_fprint_debug_snapshot(FILE* f) {
    if (!f) return;
    int phase = __atomic_load_n(&gc_phase, __ATOMIC_ACQUIRE);
    const char* pname = (phase == GC_PHASE_MARK) ? "MARK"
                        : (phase == GC_PHASE_IDLE) ? "IDLE" : "?";
    fprintf(f,
            "gc_snapshot: phase=%s(%d) heapBytes=%lld live=%lld blocks=%lld "
            "collects=%lld minor=%lld major=%lld remset=%lld threads=%lld\n",
            pname, phase,
            (long long)gc_heap_bytes, (long long)gc_live, (long long)gc_num_blocks,
            (long long)gc_collect_count, (long long)gc_minor_count,
            (long long)gc_major_count, (long long)gc_remset_count,
            (long long)gc_thread_count);
    fflush(f);
}

int64_t hao_gc_mark_assist_steps(void) {
    int64_t n;
    gc_lock_stats_ro();
    n = g_mark_assist_steps;
    hao_gc_unlock();
    return n;
}

void gc_collect_inner(char* regs, size_t regs_size);
static void gc_process_weak_refs_locked(void);
static int gc_clear_soft_refs_locked(void);
static void gc_oom_fail(size_t need);

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
        gc_mspan_mark_bit_for_block(b);
        gc_scan_block_precise(b);
        return;
    }
    b->marked = gc_mark_epoch;
    gc_mspan_mark_bit_for_block(b);
    if (gc_wl_count >= gc_wl_cap) {
        gc_wl_cap = gc_wl_cap ? gc_wl_cap * 2 : GC_WORKLIST_CAP;
        gc_worklist = (GCBlock**)realloc(gc_worklist, gc_wl_cap * sizeof(GCBlock*));
        if (!gc_worklist) { fputs("panic: GC 工作集分配失败\n", stderr); exit(1); }
    }
    gc_worklist[gc_wl_count++] = b;
}

static void gc_bump_mark_epoch(void) {
    uint8_t e = (uint8_t)(gc_mark_epoch + 1);
    int sc;
    if (e == 0) e = 1; /* 跳过 0：calloc 初值 0 = 永白于任一 epoch */
    gc_mark_epoch = e;
    /* P2：新一轮 mark 清 markBits，与对象头 epoch 对齐 */
    for (sc = -1; sc < GC_SPAN_NCLASS; ++sc) {
        MSpan* sp = (sc < 0) ? g_mspan_los : g_mspan_class[sc];
        while (sp) {
            if (sp->markBits && sp->nelems) {
                size_t nb = (sp->nelems + 7) / 8;
                size_t i;
                for (i = 0; i < nb; ++i) sp->markBits[i] = 0;
            }
            sp = sp->next;
        }
    }
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

/* 持锁、trampoline 外：消化 g_span_doomed → freelist / pend_fin；时间片让路 */
static void gc_span_drain_doomed_locked(void) {
    int free_steps = 0;
    int concurrent = g_span_doomed_concurrent;
    GCBlock* doomed = g_span_doomed;
    int64_t t0 = gc_mono_ms();
    int64_t slice_t0 = t0;
    g_span_doomed = NULL;
    g_span_doomed_concurrent = 0;
    gc_span_ensure();
    while (doomed) {
        GCBlock* b = doomed;
        doomed = b->next;
        void* user = (void*)(b + 1);
        HaoFinalizerFn fn = b->finalizer;
        b->finalizer = NULL;
        if (fn)
            gc_pend_fin_add(fn, user, b);
        else
            gc_span_push_locked(b);
        free_steps += 1;
        if (concurrent &&
            ((free_steps % 32) == 0 ||
             (gc_mono_ms() - slice_t0) >= GC_SWEEP_SLICE_MS)) {
            gc_sweep_yield_locked("drain_batch");
            slice_t0 = gc_mono_ms();
        }
    }
    g_last_drain_ms = gc_mono_ms() - t0;
}

/* BITMAP：槽区后的 u32 位图；SLOTS：scan_meta 低位。 */
static int gc_slot_bit_is_ptr(GCBlock* b, size_t i) {
    if (!b) return 0;
    if (b->scan_kind == GC_KIND_SLOTS) {
        if (i >= 32) return 0;
        return (b->scan_meta >> i) & 1u;
    }
    if (b->scan_kind == GC_KIND_BITMAP) {
        size_t nslots = (size_t)b->scan_meta;
        uint32_t* bm;
        if (i >= nslots) return 0;
        if (b->user_size < nslots * sizeof(uintptr_t) + 4) return 0;
        bm = (uint32_t*)((char*)(b + 1) + nslots * sizeof(uintptr_t));
        return (bm[i >> 5] >> (i & 31)) & 1u;
    }
    return 0;
}

static size_t gc_slots_count(GCBlock* b) {
    if (!b) return 0;
    if (b->scan_kind == GC_KIND_SLOTS) {
        size_t n = b->user_size / sizeof(uintptr_t);
        return n > 32 ? 32 : n;
    }
    if (b->scan_kind == GC_KIND_BITMAP)
        return (size_t)b->scan_meta;
    return 0;
}

/* 持锁：活堆上是否仍有指针指向 user（精确 kind 扫描）。 */
static int gc_block_refers_to(GCBlock* b, void* user) {
    if (!b || !user) return 0;
    char* u = (char*)(b + 1);
    uintptr_t want = (uintptr_t)user;
    switch (b->scan_kind) {
    case GC_KIND_OPAQUE:
    case GC_KIND_FULL: /* 禁止保守：不认伪指针边 */
        return 0;
    case GC_KIND_SLOTS:
    case GC_KIND_BITMAP: {
        size_t nslots = gc_slots_count(b);
        size_t i;
        for (i = 0; i < nslots; ++i) {
            if (gc_slot_bit_is_ptr(b, i) && ((uintptr_t*)u)[i] == want) return 1;
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
    default:
        return 0;
    }
}

/* 持锁：与 mark 同口径——显式根/槽 + 全线程 shadow/pins + 活堆边。 */
static int gc_user_still_reachable_locked(void* user) {
    int ti;
    if (!user) return 0;
    for (size_t i = 0; i < gc_root_count; ++i)
        if (gc_roots[i] == user) return 1;
    for (size_t i = 0; i < gc_root_slot_count; ++i) {
        void** slot = (void**)gc_root_slots[i];
        if (slot && *slot == user) return 1;
    }
    for (ti = 0; ti < gc_thread_count; ++ti) {
        GcThread* t = &gc_threads[ti];
        size_t sn = (t->shadow_n && t->shadow_slot && *t->shadow_slot)
                        ? *t->shadow_n
                        : 0;
        void*** sh = (t->shadow_slot) ? *t->shadow_slot : NULL;
        size_t i;
        int pi;
        for (i = 0; i < sn; ++i) {
            void** slot = sh ? sh[i] : NULL;
            if (slot && *slot == user) return 1;
        }
        if (t->scan_pins && t->scan_pin_n) {
            int n = *t->scan_pin_n;
            if (n > GC_SCAN_PINS) n = GC_SCAN_PINS;
            for (pi = 0; pi < n; ++pi)
                if (t->scan_pins[pi] == user) return 1;
        }
        if (t->refl_i64_pins && t->refl_i64_pin_n) {
            int n = *t->refl_i64_pin_n;
            if (n > GC_REFL_I64_PINS) n = GC_REFL_I64_PINS;
            for (pi = 0; pi < n; ++pi)
                if (t->refl_i64_pins[pi] == user) return 1;
        }
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
 * 回调后若仍可达则挂回堆（完整复活）；否则 freelist。禁止「挂根仍必 free」。
 * Win：SEH 隔离回调异常，禁止拖垮整个进程。 */
static void gc_drain_finalizers(void) {
    hao_gc_lock();
    size_t n = gc_pend_fin_n;
    GCPendingFinalizer* list = gc_pend_fin;
    gc_pend_fin = NULL;
    gc_pend_fin_n = 0;
    gc_pend_fin_cap = 0;
    /* 空闲 scavenge：距上次 ≥400ms 且 freelist 偏大 */
    if ((size_t)g_freelist_bytes > GC_FREELIST_SOFT_MAX / 2) {
        int64_t now = gc_mono_ms();
        if (now - g_last_idle_scavenge_ms >= 400)
            gc_mspan_scavenge_locked(1);
    }
    hao_gc_unlock();
    for (size_t i = 0; i < n; ++i) {
        if (list[i].fn) {
            __atomic_fetch_add(&gc_finalizer_runs, 1, __ATOMIC_RELAXED);
            /* 回调期间挂根：防并发 STW 与「栈上仍握 user」窗口踩 free */
            hao_gc_add_root(list[i].user);
#ifdef _WIN32
            __try {
                list[i].fn(list[i].user);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                __atomic_fetch_add(&g_finalizer_exceptions, 1, __ATOMIC_RELAXED);
            }
#else
            list[i].fn(list[i].user);
#endif
            hao_gc_remove_root(list[i].user);
        }
        hao_gc_lock();
        if (gc_user_still_reachable_locked(list[i].user))
            gc_relink_block_locked(list[i].block);
        else
            gc_span_push_locked(list[i].block);
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
    case GC_KIND_FULL: /* 禁止保守扫：FULL 当叶 */
        return;
    case GC_KIND_SLOTS:
    case GC_KIND_BITMAP: {
        size_t nslots = gc_slots_count(b);
        size_t i;
        for (i = 0; i < nslots; ++i) {
            if (gc_slot_bit_is_ptr(b, i))
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
    default:
        return;
    }
}

/* 调用方持 GC 锁：块是否仍握 young 子指针（晋升挂 remset 用） */
static int gc_block_has_young_ptr(GCBlock* b) {
    char* u;
    if (!b || b->scan_kind == GC_KIND_OPAQUE) return 0;
    u = (char*)(b + 1);
    switch (b->scan_kind) {
    case GC_KIND_SLOTS:
    case GC_KIND_BITMAP: {
        size_t nslots = gc_slots_count(b);
        size_t i;
        for (i = 0; i < nslots; ++i) {
            GCBlock* c;
            if (!gc_slot_bit_is_ptr(b, i)) continue;
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
    default:
        return 0;
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
     * v0.65：IDLE 且无 STW —— new_val==null 完全无锁；否则短锁只做 remset。
     */
    if (__atomic_load_n(&gc_phase, __ATOMIC_ACQUIRE) == GC_PHASE_IDLE &&
        !__atomic_load_n(&gc_stw_request, __ATOMIC_ACQUIRE)) {
        if (!new_val) return;
        hao_gc_lock();
        if (__atomic_load_n(&gc_phase, __ATOMIC_ACQUIRE) == GC_PHASE_IDLE &&
            !__atomic_load_n(&gc_stw_request, __ATOMIC_ACQUIRE)) {
            GCBlock* db = gc_find_block(dst);
            GCBlock* nb = gc_find_block(new_val);
            if (db && nb && db->gen == GC_GEN_OLD && nb->gen == GC_GEN_YOUNG)
                gc_remset_add((void*)(db + 1));
            hao_gc_unlock();
            return;
        }
        hao_gc_unlock();
        /* 竞态落入慢路径 */
    }
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
        if (db && nb && db->gen == GC_GEN_OLD && nb->gen == GC_GEN_YOUNG)
            gc_remset_add((void*)(db + 1));
        if (__atomic_load_n(&gc_phase, __ATOMIC_ACQUIRE) == GC_PHASE_MARK) {
            if (ob) gc_enqueue(ob);
            if (nb) gc_enqueue(nb);
        }
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

/* H1：HAO_GC_VERIFY=1 时 collect 前校验本线程 shadow 根（持锁内，精确堆指针） */
static int gc_verify_env_on(void) {
    static int inited, on;
    const char* v;
    if (inited) return on;
    inited = 1;
    v = getenv("HAO_GC_VERIFY");
    on = (v && v[0] && !(v[0] == '0' && v[1] == '\0') &&
          !((v[0] == 'n' || v[0] == 'N') && (v[1] == 'o' || v[1] == 'O') &&
            v[2] == '\0'));
    return on;
}

/* 默认 concurrent sweep；HAO_GC_STW_SWEEP=1 强制旧「整段 STW free」 */
static int gc_stw_sweep_forced(void) {
    static int inited, on;
    const char* v;
    if (inited) return on;
    inited = 1;
    v = getenv("HAO_GC_STW_SWEEP");
    on = (v && v[0] && !(v[0] == '0' && v[1] == '\0') &&
          !((v[0] == 'n' || v[0] == 'N') && (v[1] == 'o' || v[1] == 'O') &&
            v[2] == '\0'));
    return on;
}

#define GC_SWEEP_YIELD_EVERY 128
/* SPAN：concurrent free 每 GC_SWEEP_YIELD_EVERY 块为一片（spanSweepChunks） */

/* V2/V6/V7/V8/V9/V10：冒烟夹具用 TLS 毒槽（文件作用域；MSVC 函数内 TLS 不可靠） */
#ifdef _WIN32
static __declspec(thread) void* g_verify_poison_cell;
static __declspec(thread) void* g_verify_poison_pin;
static __declspec(thread) int g_verify_poison_pin_armed;
static __declspec(thread) void* g_verify_poison_remset;
static __declspec(thread) int g_verify_poison_remset_armed;
static __declspec(thread) void* g_verify_poison_refl;
static __declspec(thread) int g_verify_poison_refl_armed;
static __declspec(thread) void* g_verify_poison_gpr;
static __declspec(thread) int g_verify_poison_gpr_armed;
static __declspec(thread) void* g_verify_poison_leaf;
static __declspec(thread) int g_verify_poison_leaf_armed;
static __declspec(thread) void* g_verify_poison_ext;
static __declspec(thread) int g_verify_poison_ext_armed;
#else
static __thread void* g_verify_poison_cell;
static __thread void* g_verify_poison_pin;
static __thread int g_verify_poison_pin_armed;
static __thread void* g_verify_poison_remset;
static __thread int g_verify_poison_remset_armed;
static __thread void* g_verify_poison_refl;
static __thread int g_verify_poison_refl_armed;
static __thread void* g_verify_poison_gpr;
static __thread int g_verify_poison_gpr_armed;
static __thread void* g_verify_poison_leaf;
static __thread int g_verify_poison_leaf_armed;
static __thread void* g_verify_poison_ext;
static __thread int g_verify_poison_ext_armed;
#endif

static void gc_verify_shadow_roots(void) {
    int ti;
    size_t i;
    if (!gc_verify_env_on()) return;
    /* 跨线程：扫全部已注册线程 shadow（对齐 mark 的 gc_threads 枚举） */
    for (ti = 0; ti < gc_thread_count; ++ti) {
        GcThread* t = &gc_threads[ti];
        size_t sn = (t->shadow_n && t->shadow_slot && *t->shadow_slot)
                        ? *t->shadow_n
                        : 0;
        void*** sh = t->shadow_slot ? *t->shadow_slot : NULL;
        for (i = 0; i < sn; ++i) {
            void** slot = sh ? sh[i] : NULL;
            void* p;
            if (!slot) continue;
            p = *slot;
            if (!p) continue;
            if (!gc_find_block_exact(p)) {
                gc_verify_fatal_ip("shadow root is not a live heap object",
                                   "shadow_i", (int64_t)i, p);
            }
        }
    }
}

/* V6：全线程 scan pin + 本线程毒针；坏槽 fatal 含 pin_i=/ptr= */
static void gc_verify_scan_pins(void) {
    int ti, i;
    if (!gc_verify_env_on()) return;
    for (ti = 0; ti < gc_thread_count; ++ti) {
        GcThread* t = &gc_threads[ti];
        int n = (t->scan_pin_n) ? *t->scan_pin_n : 0;
        void** pins = t->scan_pins;
        if (!pins) continue;
        for (i = 0; i < n && i < GC_SCAN_PINS; ++i) {
            void* p = pins[i];
            if (!p) continue;
            if (!gc_find_block_exact(p)) {
                gc_verify_fatal_ip("scan pin is not a live heap object", "pin_i",
                                   (int64_t)(i), p);
            }
        }
    }
    /* 毒针不依赖易被 clear 的 g_scan_pins 寿命；合成 pin_i=0 便于门禁 */
    if (g_verify_poison_pin_armed) {
        void* p = g_verify_poison_pin;
        if (p && !gc_find_block_exact(p)) {
            gc_verify_fatal_ip("scan pin is not a live heap object", "pin_i", (int64_t)(0), p);
        }
    }
}

/* V7：校验 remset 条目为活堆对象；毒针合成 remset_i=0 */
static void gc_verify_remset(void) {
    size_t i;
    if (!gc_verify_env_on()) return;
    for (i = 0; i < gc_remset_count; ++i) {
        void* p = gc_remset[i];
        if (!p) continue;
        if (!gc_find_block_exact(p)) {
            gc_verify_fatal_ip("remset entry is not a live heap object", "remset_i", (int64_t)(i), p);
        }
    }
    if (g_verify_poison_remset_armed) {
        void* p = g_verify_poison_remset;
        if (p && !gc_find_block_exact(p)) {
            gc_verify_fatal_ip("remset entry is not a live heap object", "remset_i", (int64_t)((size_t)0), p);
        }
    }
}

/* V8：全线程 refl_i64 pin；本线程毒针合成 refl_i64_i=0 */
static void gc_verify_refl_i64_pins(void) {
    int ti, i;
    if (!gc_verify_env_on()) return;
    for (ti = 0; ti < gc_thread_count; ++ti) {
        GcThread* t = &gc_threads[ti];
        int n = (t->refl_i64_pin_n) ? *t->refl_i64_pin_n : 0;
        void** pins = t->refl_i64_pins;
        if (!pins) continue;
        for (i = 0; i < n && i < GC_REFL_I64_PINS; ++i) {
            void* p = pins[i];
            if (!p) continue;
            if (!gc_find_block_exact(p)) {
                gc_verify_fatal_ip("refl_i64 pin is not a live heap object",
                                   "refl_i64_i", (int64_t)(i), p);
            }
        }
    }
    if (g_verify_poison_refl_armed) {
        void* p = g_verify_poison_refl;
        if (p && !gc_find_block_exact(p)) {
            gc_verify_fatal_ip("refl_i64 pin is not a live heap object", "refl_i64_i", (int64_t)(0), p);
        }
    }
}

/* V9：全线程 GPR 溅射槽；非堆整数跳过；堆区内非精确活对象 fatal；毒针合成 gpr_i=0 */
static void gc_verify_gpr_spill(void) {
    int ti;
    size_t i, n;
    if (!gc_verify_env_on()) return;
    n = GC_GPR_SPILL_BYTES / sizeof(void*);
    for (ti = 0; ti < gc_thread_count; ++ti) {
        GcThread* t = &gc_threads[ti];
        void** spill = (void**)t->gpr_spill;
        if (!spill) continue;
        for (i = 0; i < n; ++i) {
            void* p = spill[i];
            if (!p) continue;
            if (((uintptr_t)p & (sizeof(uintptr_t) - 1)) != 0) continue;
            if (gc_heap_lo && gc_heap_hi &&
                ((char*)p < gc_heap_lo || (char*)p >= gc_heap_hi))
                continue;
            if (!gc_find_block_exact(p)) {
                gc_verify_fatal_ip("gpr spill is not a live heap object", "gpr_i",
                                   (int64_t)(i), p);
            }
        }
    }
    if (g_verify_poison_gpr_armed) {
        void* p = g_verify_poison_gpr;
        if (p && !gc_find_block_exact(p)) {
            gc_verify_fatal_ip("gpr spill is not a live heap object", "gpr_i", (int64_t)((size_t)0), p);
        }
    }
}

/* V10：C 叶 VERIFY 仅毒针（禁止真实保守叶全扫假阳）；fatal 含 leaf_i=/ptr= */
static void gc_verify_c_leaf(void) {
    if (!gc_verify_env_on()) return;
    if (!g_verify_poison_leaf_armed) return;
    {
        void* p = g_verify_poison_leaf;
        if (p && !gc_find_block_exact(p)) {
            gc_verify_fatal_ip("c leaf is not a live heap object", "leaf_i", (int64_t)(0), p);
        }
    }
}

/* V11：外部根（gc_roots / slots）VERIFY 仅毒针；禁跨线程；fatal 含 ext_i=/ptr= */
static void gc_verify_ext_roots(void) {
    if (!gc_verify_env_on()) return;
    if (!g_verify_poison_ext_armed) return;
    {
        void* p = g_verify_poison_ext;
        if (p && !gc_find_block_exact(p)) {
            gc_verify_fatal_ip("ext root is not a live heap object", "ext_i", (int64_t)(0), p);
        }
    }
}

static void gc_verify_fatal_mismatch(const char* head, int64_t a, int64_t b) {
    char detail[160];
    int len = 0;
    len = hao_gc_append_str(detail, 160, len, head);
    len = hao_gc_append_str(detail, 160, len, " a=");
    len = hao_gc_append_i64(detail, 160, len, a);
    len = hao_gc_append_str(detail, 160, len, " b=");
    (void)hao_gc_append_i64(detail, 160, len, b);
    hao_report_fatal("gc_verify", detail);
}

static void gc_verify_heap_integrity(void) {
    GCBlock* b;
    int64_t n = 0;
    size_t bytes = 0;
    if (!gc_verify_env_on()) return;
    for (b = gc_heap; b; b = b->next) {
        n += 1;
        if (b->scan_kind == GC_KIND_FULL)
            gc_verify_fatal_ip("forbidden GC_KIND_FULL on live heap", "heap_i", n,
                               (void*)(b + 1));
        if (b->user_size == 0)
            gc_verify_fatal_ip("heap block user_size=0", "heap_i", n, (void*)(b + 1));
        if (b->user_size > ((size_t)1 << 28))
            gc_verify_fatal_ip("heap block user_size too large", "heap_i", n,
                               (void*)(b + 1));
        if (bytes > SIZE_MAX - b->user_size)
            gc_verify_fatal_ip("heap bytes overflow while walking", "heap_i", n,
                               (void*)(b + 1));
        bytes += b->user_size;
        if (n > gc_num_blocks + 1000000)
            gc_verify_fatal_mismatch("heap walk exceeded blockCount+slop", n,
                                     gc_num_blocks);
    }
    if (n != gc_num_blocks)
        gc_verify_fatal_mismatch("heap walk blockCount mismatch", n, gc_num_blocks);
    if (bytes != gc_heap_bytes)
        gc_verify_fatal_mismatch("heap walk heapBytes mismatch", (int64_t)bytes,
                                 (int64_t)gc_heap_bytes);
}

/* VERIFY：mspan allocBits/markBits + 空闲计数；collect 外不得残留 doomed */
static void gc_verify_freelist_integrity(void) {
    int sc;
    if (!gc_verify_env_on()) return;
    if (g_span_doomed) {
        gc_verify_fatal_mismatch("g_span_doomed non-null outside drain", 1, 0);
    }
    for (sc = -1; sc < GC_SPAN_NCLASS; ++sc) {
        MSpan* sp = (sc < 0) ? g_mspan_los : g_mspan_class[sc];
        while (sp) {
            size_t i, alloc_n = 0, free_n = 0;
            size_t bits_bytes = (sp->nelems + 7) / 8;
            if (!sp->allocBits || !sp->markBits || !sp->start)
                gc_verify_fatal_mismatch("mspan missing bits/start", (int64_t)sc, 0);
            for (i = 0; i < sp->nelems; ++i) {
                int a = gc_bit_test(sp->allocBits, i);
                if (a) alloc_n++;
                else free_n++;
                if (a) {
                    GCBlock* b = (GCBlock*)(sp->start + i * sp->elemsize);
                    int marked = gc_block_is_marked(b);
                    int mb = gc_bit_test(sp->markBits, i);
                    /* P2：活且在堆上时 markBits 应与 epoch 一致（允许 unmarked 在 IDLE） */
                    if (gc_find_block((void*)(b + 1))) {
                        if (marked && !mb)
                            gc_verify_fatal_ip("markBits clear but block marked",
                                               "span_i", (int64_t)i, (void*)(b + 1));
                    }
                }
            }
            if (alloc_n + free_n != sp->nelems)
                gc_verify_fatal_mismatch("mspan bit count", (int64_t)alloc_n,
                                         (int64_t)sp->nelems);
            if (free_n != sp->free_count)
                gc_verify_fatal_mismatch("mspan free_count", (int64_t)free_n,
                                         (int64_t)sp->free_count);
            (void)bits_bytes;
            sp = sp->next;
        }
    }
}

static void gc_verify_roots(void) {
    gc_verify_shadow_roots();
    gc_verify_scan_pins();
    gc_verify_remset();
    gc_verify_refl_i64_pins();
    gc_verify_gpr_spill();
    gc_verify_c_leaf();
    gc_verify_ext_roots();
    gc_verify_heap_integrity();
    gc_verify_freelist_integrity();
}

/* V2：冒烟/调试钩子——压入非堆指针，下一 VERIFY collect 应 fatal */
void hao_debug_poison_shadow_root(void) {
    /* 低地址非堆；null 会被 verify 跳过，故用 0x8 */
    g_verify_poison_cell = (void*)(uintptr_t)0x8;
    hao_gc_root_push(&g_verify_poison_cell);
}

/* V6：武装 pin 毒槽 */
void hao_debug_poison_scan_pin(void) {
    g_verify_poison_pin = (void*)(uintptr_t)0x8;
    g_verify_poison_pin_armed = 1;
}

/* V7：武装 remset 毒槽 */
void hao_debug_poison_remset(void) {
    g_verify_poison_remset = (void*)(uintptr_t)0x8;
    g_verify_poison_remset_armed = 1;
}

/* V8：武装 refl_i64 毒槽 */
void hao_debug_poison_refl_i64(void) {
    g_verify_poison_refl = (void*)(uintptr_t)0x8;
    g_verify_poison_refl_armed = 1;
}

/* V9：武装 GPR 溅射毒槽（写入 TLS 缓冲槽 0 + armed，非堆策略与 poison 一致） */
void hao_debug_poison_gpr(void) {
    g_verify_poison_gpr = (void*)(uintptr_t)0x8;
    g_verify_poison_gpr_armed = 1;
    ((void**)g_gpr_spill)[0] = g_verify_poison_gpr;
}

/* V10：武装 C 叶毒针（仅 armed 路径；不对真实叶全扫） */
void hao_debug_poison_c_leaf(void) {
    g_verify_poison_leaf = (void*)(uintptr_t)0x8;
    g_verify_poison_leaf_armed = 1;
}

/* V11：武装外部根毒针（仅 armed；不对真实 gc_roots 全扫假阳） */
void hao_debug_poison_ext_root(void) {
    g_verify_poison_ext = (void*)(uintptr_t)0x8;
    g_verify_poison_ext_armed = 1;
}

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

    /* VERIFY 在 gc_collect（trampoline 外）执行：裸垫片栈未 16 对齐时
     * CRT fprintf/fopen 可能 AV，导致坏根无法落到 hao-crash.log */

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
            g_stw_soft_phase = GC_STW_PHASE_ROOT;
            g_stw_soft_attempt = 0;
            int root_ok = gc_stw_enter_and_scan_soft(GC_STW_TOTAL_MS);
            gc_scanned_tid_add(self);
            if (root_ok == 2) {
                gc_abort_mark_cycle(GC_ABORT_PARK_WD);
                g_collect_c_hi = prev_c_hi;
                return;
            }
            if (root_ok == 0) {
                gc_abort_mark_cycle(GC_ABORT_ROOT);
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
        if (gc_stw_trip_armed()) {
            gc_abort_mark_cycle(GC_ABORT_PARK_WD);
            g_collect_c_hi = prev_c_hi;
            return;
        }
        gc_drain_worklist(1);
    }

    /* ---- Mark Termination：握手 seed/判空；未空则放行后并发 drain 再试 ---- */
    {
        int term_ok = 0;
        for (int attempt = 0; attempt < GC_STW_TERM_RETRIES; ++attempt) {
            if (gc_stw_trip_armed()) {
                gc_abort_mark_cycle(GC_ABORT_PARK_WD);
                g_collect_c_hi = prev_c_hi;
                return;
            }
            gc_scanned_tid_clear();
            g_stw_soft_phase = GC_STW_PHASE_TERM;
            g_stw_soft_attempt = attempt;
            int soft_ok = gc_stw_enter_and_scan_soft(GC_STW_TERM_TOTAL_MS);
            if (soft_ok == 2) {
                gc_abort_mark_cycle(GC_ABORT_PARK_WD);
                g_collect_c_hi = prev_c_hi;
                return;
            }
            gc_scanned_tid_add(self);
            gc_seed_roots_and_remset(regs, regs_size);
            /* 二次 seed：屏障窗口漏灰时第一轮可能假空 */
            if (soft_ok == 1 && gc_wl_head >= gc_wl_count)
                gc_seed_roots_and_remset(regs, regs_size);
            if (soft_ok == 1 && gc_all_threads_scanned(self) &&
                gc_wl_head >= gc_wl_count) {
                term_ok = 1;
                break;
            }
            gc_stw_leave();
            gc_drain_worklist(1);
            gc_hold_slice_end();
            hao_gc_unlock();
            gc_sleep_ms(2);
            hao_gc_lock();
            gc_hold_slice_begin();
        }
        if (!term_ok) {
            /* 禁止无限 MARK 黑囤：作废本轮色，下一轮从根重来 */
            gc_abort_mark_cycle(GC_ABORT_TERM);
            g_collect_c_hi = prev_c_hi;
            return;
        }
    }
    major = gc_cycle_is_major;

    __atomic_store_n(&gc_phase, GC_PHASE_IDLE, __ATOMIC_RELEASE);
    /* 强可达已定：弱/软仅当 referent 未强标时置空（平常 collect 不强清强活 soft） */
    gc_process_weak_refs_locked();
    /*
     * concurrent sweep + exact-size freelist（默开；HAO_GC_STW_SWEEP=1 关）：
     * 1) 持锁把白块从 gc_heap 摘到 doomed（不 free）。
     * 2) 先 stw_leave；垫片外再 drain 入 freelist（LOS 仍 free）。
     * STW 模式：整段保持 stw_request（旧行为）。
     */
    int concurrent_sweep = !gc_stw_sweep_forced();
    if (concurrent_sweep)
        gc_stw_leave();

    void** promoted_buf = NULL;
    size_t promoted_n = 0, promoted_cap = 0;
    GCBlock* doomed = NULL;

    gc_in_collect = 1;
    GCBlock** prev = &gc_heap;
    size_t live = 0;
    int64_t live_fin = 0; /* v0.55.53：存活且仍挂 finalizer */
    int64_t doomed_n = 0;
    int unlink_steps = 0;
    int64_t unlink_t0 = gc_mono_ms();
    int64_t unlink_slice = unlink_t0;
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
                    g_promote_count += 1;
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
            if (b->finalizer) live_fin += 1;
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
            if (gc_find_block((void*)(b + 1))) {
                fputs("panic: GC unlink still findable after page_index_remove\n",
                      stderr);
                exit(1);
            }
            b->next = doomed;
            doomed = b;
            doomed_n += 1;
            g_freed_bytes_total += (int64_t)b->user_size;
        }
        unlink_steps += 1;
        /* LAT-2：concurrent 路径 unlink 时间片让路，避免堵 memstats */
        if (concurrent_sweep &&
            ((unlink_steps % 64) == 0 ||
             (gc_mono_ms() - unlink_slice) >= GC_SWEEP_SLICE_MS)) {
            gc_sweep_yield_locked("unlink_batch");
            unlink_slice = gc_mono_ms();
            /* 让路后 prev 仍指向合法下一节点（链在持锁外未改拓扑的仅 alloc 挂头） */
            if (!*prev && doomed) {
                /* 无更多活链节点；循环将结束 */
            }
        }
    }
    g_last_unlink_ms = gc_mono_ms() - unlink_t0;

    /* 白块已摘链：freelist/free 延后到 trampoline 外（对齐栈），此处只挂起 */
    {
        gc_worklist_reset();
        g_span_doomed = doomed;
        g_span_doomed_concurrent = concurrent_sweep;
    }
    gc_in_collect = 0;
    if (concurrent_sweep)
        g_concurrent_sweep_cycles += 1;
    else
        gc_stw_leave();
    gc_worklist_reset();
    gc_scanned_tid_clear();

    if (major) {
        gc_remset_count = 0;
    } else {
        gc_remset_filter_live();
        for (size_t i = 0; i < promoted_n; ++i) {
            GCBlock* pb = gc_find_block(promoted_buf[i]);
            if (pb && gc_block_has_young_ptr(pb))
                gc_remset_add(promoted_buf[i]);
        }
    }
    free(promoted_buf);
    gc_nursery_alloc = 0;

    g_finalizer_live_at_sweep = live_fin;
    if (live_fin > 0)
        g_last_finalizer_diag = 1; /* 对象仍活 → runs 可不涨 */
    else if (g_finalizer_skip_abort > 0)
        g_last_finalizer_diag = 2;
    else
        g_last_finalizer_diag = 0;
    hao_trace("gc", "finalizer_diag live_at_sweep=%lld skip_abort=%lld tag=%d",
              (long long)live_fin, (long long)g_finalizer_skip_abort,
              g_last_finalizer_diag);

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
/* V4：VERIFY 开启时重入跳过计数（不改变跳过语义） */
static volatile int64_t g_verify_skip_reenter = 0;

int64_t hao_gc_verify_skip_reenter(void) {
    return __atomic_load_n(&g_verify_skip_reenter, __ATOMIC_RELAXED);
}

static void gc_note_verify_skip_reenter(void) {
    if (!gc_verify_env_on()) return;
    __atomic_fetch_add(&g_verify_skip_reenter, 1, __ATOMIC_RELAXED);
    hao_trace("gc", "verify skip reenter collect");
}


/* ---- 弱/软引用：强标后、摘白前处理；referent 不经侧表入灰 ---- */
static int gc_weak_grow_locked(void) {
    int ncap = g_weak_cap ? g_weak_cap * 2 : 256;
    GcWeakEnt* nw;
    int i;
    nw = (GcWeakEnt*)realloc(g_weaks, (size_t)ncap * sizeof(GcWeakEnt));
    if (!nw) return 0;
    for (i = g_weak_cap; i < ncap; ++i) {
        nw[i].wr = NULL;
        nw[i].referent = NULL;
        nw[i].soft = 0;
        nw[i].used = 0;
    }
    g_weaks = nw;
    g_weak_cap = ncap;
    return 1;
}

static void gc_weak_remove_at(int i) {
    if (i < 0 || i >= g_weak_n || !g_weaks) return;
    g_weaks[i] = g_weaks[g_weak_n - 1];
    g_weaks[g_weak_n - 1].used = 0;
    g_weak_n -= 1;
}

static void gc_process_weak_refs_locked(void) {
    int i = 0;
    while (i < g_weak_n) {
        GcWeakEnt* e = &g_weaks[i];
        GCBlock* wb;
        GCBlock* rb;
        if (!e->used || !e->wr) { gc_weak_remove_at(i); continue; }
        wb = gc_find_block_exact(e->wr);
        if (!wb || !gc_block_is_marked(wb)) { gc_weak_remove_at(i); continue; }
        if (!e->referent) { i += 1; continue; }
        rb = gc_find_block_exact(e->referent);
        /* weak/soft 同规则：仅未强标时置空；OOM 前另走 gc_clear_soft_refs_locked */
        if (!rb || !gc_block_is_marked(rb))
            e->referent = NULL;
        i += 1;
    }
}

static int gc_clear_soft_refs_locked(void) {
    int n = 0, i;
    for (i = 0; i < g_weak_n; ++i) {
        if (g_weaks[i].used && g_weaks[i].soft && g_weaks[i].referent) {
            g_weaks[i].referent = NULL;
            n += 1;
        }
    }
    return n;
}

/* HAO_GC_TEST_OOM=1 或 hao_gc_test_force_oom_once：下一次起 calloc 强制失败至 OOM */
static int gc_test_should_fail_calloc(void) {
    const char* v;
    if (g_test_force_oom) return 1;
    v = getenv("HAO_GC_TEST_OOM");
    if (v && v[0] && !(v[0] == '0' && v[1] == '\0') &&
        !((v[0] == 'n' || v[0] == 'N') && (v[1] == 'o' || v[1] == 'O') &&
          v[2] == '\0')) {
        g_test_force_oom = 1;
        return 1;
    }
    return 0;
}

void hao_gc_test_force_oom_once(void) {
    g_test_force_oom = 1;
}

void hao_gc_ensure_oom_exc(void) {
    /* 单例仅由 Hao GC.boot → hao_gc_set_oom_exception 安装；C 侧无法捏造 */
}

static void gc_oom_fail(size_t need) {
    char buf[192];
    int bl = 0;
    size_t snap_heap = 0, snap_live = 0, snap_thr = 0;
    int64_t snap_blocks = 0;
    g_test_force_oom = 0;
    hao_gc_lock();
    snap_heap = gc_heap_bytes;
    snap_live = gc_live;
    snap_thr = gc_threshold;
    snap_blocks = gc_num_blocks;
    hao_gc_unlock();
    bl = hao_gc_append_str(buf, 192, bl, "oom need=");
    bl = hao_gc_append_i64(buf, 192, bl, (int64_t)need);
    bl = hao_gc_append_str(buf, 192, bl, "B heap=");
    bl = hao_gc_append_i64(buf, 192, bl, (int64_t)snap_heap);
    bl = hao_gc_append_str(buf, 192, bl, " live=");
    bl = hao_gc_append_i64(buf, 192, bl, (int64_t)snap_live);
    bl = hao_gc_append_str(buf, 192, bl, " thr=");
    bl = hao_gc_append_i64(buf, 192, bl, (int64_t)snap_thr);
    bl = hao_gc_append_str(buf, 192, bl, " blocks=");
    (void)hao_gc_append_i64(buf, 192, bl, snap_blocks);
    if (g_oom_exc && hao_exc_has_catcher()) {
        /* OutOfMemoryException: [0]=vt [1]=message [2]=needBytes；先 barrier 再 store */
        void** slots = (void**)g_oom_exc;
        HaoString* msg = hao_str_from_cstr(buf);
        hao_gc_barrier(&slots[1], msg);
        slots[1] = msg;
        ((int64_t*)g_oom_exc)[2] = (int64_t)need;
        hao_throw(g_oom_exc);
        return;
    }
    hao_report_fatal("oom", buf);
}

static void gc_run_collect_locked(int major) {
    if (gc_collecting) {
        gc_note_verify_skip_reenter();
        return; /* 防 STW 放锁窗口内嵌套收集 */
    }
    char frame;
    char* prev_c_hi = g_collect_c_hi;
    /* 取更靠近 stack_top 的 C 边界（栈向下长） */
    if (!prev_c_hi || &frame > prev_c_hi) g_collect_c_hi = &frame;
    gc_collecting = 1;
    g_collect_want_major = major ? 1 : 0;
    g_hold_acc_ms = 0;
    gc_hold_slice_begin();
    /* V3：与 gc_collect 同口径——trampoline 外 VERIFY（禁垫片内 CRT fatal） */
    gc_verify_roots();
    gc_span_ensure();
    gc_collect_trampoline();
    /* LAT-2：与 gc_collect 同 — drain/scavenge 短批，返回时仍持锁 */
    gc_stats_publish_locked();
    gc_hold_slice_end();
    hao_gc_unlock();
    hao_gc_lock();
    gc_hold_slice_begin();
    gc_span_drain_doomed_locked();
    gc_mspan_scavenge_locked(g_collect_want_major ? 0 : 1);
    gc_verify_roots();
    gc_stats_publish_locked();
    g_last_collect_hold_ms = g_hold_acc_ms;
    if (g_hold_slice_t0 > 0)
        g_last_collect_hold_ms += gc_mono_ms() - g_hold_slice_t0;
    gc_hold_slice_end();
    gc_hold_slice_begin(); /* 调用方仍持锁 */
    gc_collecting = 0;
    g_collect_c_hi = prev_c_hi;
}

void gc_collect(void) {
    char collect_frame;
    char* prev_c_hi = g_collect_c_hi;
    g_collect_c_hi = &collect_frame;
    hao_trace("gc", "collect enter");
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
        gc_note_verify_skip_reenter();
        hao_gc_unlock();
        g_collect_c_hi = prev_c_hi;
        return;
    }
    gc_collecting = 1;
    g_collect_want_major = 1;
    g_hold_acc_ms = 0;
    gc_hold_slice_begin();
    /* V2：collect 前/后 VERIFY（trampoline 外，避免裸垫片栈对齐坑） */
    gc_verify_roots();
    gc_span_ensure();
    gc_collect_trampoline();
    /*
     * LAT-2：trampoline 内已 stw_leave（concurrent）；drain/scavenge 不得再整段占大锁。
     * 先 publish + unlock，再短批持锁消化 doomed。
     */
    gc_stats_publish_locked();
    gc_hold_slice_end();
    hao_gc_unlock();

    hao_gc_lock();
    gc_hold_slice_begin();
    /* freelist drain 必须在垫片外：trampoline 栈上 CRT/写 freelist 会 AV */
    gc_span_drain_doomed_locked();
    gc_mspan_scavenge_locked(1);
    gc_verify_roots();
    gc_stats_publish_locked();
    g_last_collect_hold_ms = g_hold_acc_ms;
    if (g_hold_slice_t0 > 0)
        g_last_collect_hold_ms += gc_mono_ms() - g_hold_slice_t0;
    gc_hold_slice_end();
    hao_trace("gc", "collect_hold");
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

/* 写入 gc.GcStats：槽 0 vtable；1..N 与 GC.hao 字段声明序一致 */
static void gc_stats_fill_into(int64_t* s) {
    if (!s) return;
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
    s[19] = (int64_t)gc_remset_count;
    s[20] = g_park_watchdog_trips;
    s[21] = g_stw_incomplete_root;
    s[22] = g_stw_incomplete_term;
    s[23] = g_mark_abort_root;
    s[24] = g_mark_abort_term;
    s[25] = g_mark_abort_park_wd;
    s[26] = (int64_t)g_last_stw_phase;
    s[27] = (int64_t)g_last_stw_attempt;
    s[28] = (int64_t)g_last_stw_targets;
    s[29] = (int64_t)g_last_stw_parked;
    s[30] = (int64_t)g_last_stw_missing;
    s[31] = (int64_t)g_last_stw_os_block_missing;
    s[32] = (int64_t)g_last_miss_tid_n;
    s[33] = g_last_miss_tids[0];
    s[34] = g_last_miss_tids[1];
    s[35] = g_last_miss_max_age_ms;
    s[36] = g_finalizer_skip_abort;
    s[37] = g_finalizer_live_at_sweep;
    s[38] = (int64_t)g_last_finalizer_diag;
    s[39] = g_concurrent_sweep_cycles;
    s[40] = g_span_sweep_chunks;
    s[41] = g_span_freelist_hits;
    {
        int64_t elapsed = gc_mono_ms() - g_proc_start_ms;
        if (elapsed < 1) elapsed = 1;
        if (g_proc_start_ms == 0) {
            g_proc_start_ms = gc_mono_ms();
            elapsed = 1;
        }
        s[42] = (g_alloc_bytes_total * 1000) / elapsed;
        s[43] = (g_freed_bytes_total * 1000) / elapsed;
        s[44] = g_stw_wait_ms_total;
        s[45] = g_promote_count;
        s[46] = (g_promote_count * 1000) / elapsed;
        s[47] = (int64_t)g_last_miss_line;
        s[48] = (int64_t)g_last_miss_col;
        s[49] = g_span_commit_bytes;
        s[50] = g_freelist_bytes;
        s[51] = g_scavenge_bytes;
        s[52] = g_scavenge_cycles;
        s[53] = g_span_count;
        /* v0.74 LAT-2 */
        s[54] = g_last_collect_hold_ms;
        s[55] = g_last_unlink_ms;
        s[56] = g_last_drain_ms;
        s[57] = g_last_scavenge_ms;
        s[58] = g_last_stats_lock_wait_ms;
        s[59] = g_last_publish_ms;
        s[60] = g_stats_publish_count;
        s[61] = g_last_stats_safepoint_ms;
    }
}

void hao_gc_stats(void* obj) {
    int64_t* s;
    int64_t t0, t1;
    int idx;
    if (!obj) return;
    s = (int64_t*)obj;
    t0 = gc_mono_ms();
    /* 协作：STW 中仍须 park；leave 后读快照不堵 sweep 大锁 */
    hao_gc_safepoint();
    t1 = gc_mono_ms();
    g_last_stats_safepoint_ms = t1 - t0;
    if (g_last_stats_safepoint_ms < 0) g_last_stats_safepoint_ms = 0;
    if (g_stats_publish_count == 0) {
        /* 冷启动：短持锁填首帧快照 */
        int64_t tw = gc_mono_ms();
        hao_gc_lock();
        g_last_stats_lock_wait_ms = gc_mono_ms() - tw;
        if (g_last_stats_lock_wait_ms < 0) g_last_stats_lock_wait_ms = 0;
        gc_stats_publish_locked();
        hao_gc_unlock();
        hao_trace("gc", "stats_wait");
    } else {
        g_last_stats_lock_wait_ms = 0;
    }
    idx = __atomic_load_n(&g_stats_snap_i, __ATOMIC_ACQUIRE);
    if (idx < 0 || idx > 1) idx = 0;
    memcpy(&s[1], &g_stats_snap[idx][1], (GC_STATS_SLOTS - 1) * sizeof(int64_t));
    /* 读侧刷新本次观测（不回写 snap） */
    s[58] = g_last_stats_lock_wait_ms;
    s[61] = g_last_stats_safepoint_ms;
}

int64_t hao_gc_remset_count(void) {
    gc_lock_stats_ro();
    int64_t n = (int64_t)gc_remset_count;
    hao_gc_unlock();
    return n;
}

int64_t hao_gc_mark_abort_cycles(void) {
    gc_lock_stats_ro();
    int64_t n = g_mark_abort_cycles;
    hao_gc_unlock();
    return n;
}

int64_t hao_gc_mark_worker_steps(void) {
    gc_lock_stats_ro();
    int64_t n = g_mark_worker_steps;
    hao_gc_unlock();
    return n;
}

int64_t hao_gc_live_bytes(void) {
    gc_lock_stats_ro();
    int64_t v = (int64_t)gc_live;
    hao_gc_unlock();
    return v;
}

int64_t hao_gc_threshold(void) {
    gc_lock_stats_ro();
    int64_t v = (int64_t)gc_threshold;
    hao_gc_unlock();
    return v;
}

int64_t hao_gc_allocated_since(void) {
    gc_lock_stats_ro();
    int64_t v = (int64_t)gc_allocated;
    hao_gc_unlock();
    return v;
}

int64_t hao_gc_block_count(void) {
    gc_lock_stats_ro();
    int64_t n = gc_num_blocks;
    hao_gc_unlock();
    return n;
}

int64_t hao_gc_collect_count(void) {
    gc_lock_stats_ro();
    int64_t v = gc_collect_count;
    hao_gc_unlock();
    return v;
}

int64_t hao_gc_minor_count(void) {
    gc_lock_stats_ro();
    int64_t v = gc_minor_count;
    hao_gc_unlock();
    return v;
}

int64_t hao_gc_major_count(void) {
    gc_lock_stats_ro();
    int64_t v = gc_major_count;
    hao_gc_unlock();
    return v;
}

/* 自上次 minor/major 以来 nursery 累计分配（触发 minor 的压力指标） */
int64_t hao_gc_nursery_bytes(void) {
    gc_lock_stats_ro();
    int64_t v = (int64_t)gc_nursery_alloc;
    hao_gc_unlock();
    return v;
}

/* 已向 GC 注册的线程数（参与 STW 栈扫描；≠ OS 线程总数） */
int64_t hao_gc_registered_threads(void) {
    gc_lock_stats_ro();
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
        hao_report_fatal("oom", "内存分配过大（超出可表示尺寸）");
    }
    size_t need = (n + GC_ALIGN - 1) & ~(size_t)(GC_ALIGN - 1);
    g_alloc_bytes_total += (int64_t)need;

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
    gc_span_ensure();
    /* 紧急 scavenge：缓存过大时先还空板 */
    if ((size_t)g_freelist_bytes > GC_FREELIST_SOFT_MAX * 2)
        gc_mspan_scavenge_locked(2);

    {
        GCBlock* reused = gc_span_pop_locked(need);
        if (!reused && gc_span_class_of(need) < 0)
            reused = gc_mspan_alloc_los_locked(need);
        if (reused) {
            char* u;
            size_t i;
            size_t zlim = total;
            MSpan* sp = gc_span_of_ptr(reused);
            if (sp && sp->elemsize > zlim) zlim = sp->elemsize;
            for (i = 0; i < zlim; ++i) ((char*)reused)[i] = 0;
            reused->user_size = need;
            reused->marked =
                (__atomic_load_n(&gc_phase, __ATOMIC_ACQUIRE) == GC_PHASE_MARK)
                    ? gc_mark_epoch
                    : 0;
            if (reused->marked) gc_mspan_mark_bit_for_block(reused);
            reused->scan_kind = kind;
            reused->scan_meta = (uint32_t)meta;
            reused->gen = GC_GEN_YOUNG;
            reused->age = 0;
            reused->next = gc_heap;
            gc_heap = reused;
            gc_num_blocks += 1;
            gc_heap_bytes += need;
            u = (char*)(reused + 1);
            if (!gc_heap_lo || u < gc_heap_lo) gc_heap_lo = u;
            if (!gc_heap_hi || u + need > gc_heap_hi) gc_heap_hi = u + need;
            gc_page_index_add(reused);
            hao_gc_unlock();
            gc_drain_finalizers();
            return u;
        }
    }
    /* mspan 分配失败：major + soft 清后重试（禁止再走 CRT calloc 永囤） */
    {
        GCBlock* b = NULL;
        int attempt;
        for (attempt = 0; attempt < 3 && !b; ++attempt) {
            if (attempt == 1) {
                gc_run_collect_locked(1);
            } else if (attempt == 2) {
                int cleared = gc_clear_soft_refs_locked();
                if (cleared > 0) gc_run_collect_locked(1);
            }
            b = gc_span_pop_locked(need);
            if (!b && gc_span_class_of(need) < 0)
                b = gc_mspan_alloc_los_locked(need);
            if (b) {
                char* u;
                size_t i;
                size_t zlim = total;
                MSpan* sp = gc_span_of_ptr(b);
                if (sp && sp->elemsize > zlim) zlim = sp->elemsize;
                for (i = 0; i < zlim; ++i) ((char*)b)[i] = 0;
                b->user_size = need;
                b->marked =
                    (__atomic_load_n(&gc_phase, __ATOMIC_ACQUIRE) == GC_PHASE_MARK)
                        ? gc_mark_epoch
                        : 0;
                if (b->marked) gc_mspan_mark_bit_for_block(b);
                b->scan_kind = kind;
                b->scan_meta = (uint32_t)meta;
                b->gen = GC_GEN_YOUNG;
                b->age = 0;
                b->next = gc_heap;
                gc_heap = b;
                gc_num_blocks += 1;
                gc_heap_bytes += need;
                u = (char*)(b + 1);
                if (!gc_heap_lo || u < gc_heap_lo) gc_heap_lo = u;
                if (!gc_heap_hi || u + need > gc_heap_hi) gc_heap_hi = u + need;
                gc_page_index_add(b);
                hao_gc_unlock();
                gc_drain_finalizers();
                return u;
            }
        }
        gc_oom_fail(need);
        return NULL;
    }
}

void* gc_alloc(size_t n) {
    /* 禁止 FULL 保守堆：未知载荷按叶（OPAQUE） */
    return gc_alloc_ex(n, GC_KIND_OPAQUE, 0);
}

int64_t hao_gc_finalizer_exceptions(void) {
    return __atomic_load_n(&g_finalizer_exceptions, __ATOMIC_RELAXED);
}

void hao_gc_set_oom_exception(void* exc) {
    void* old;
    hao_gc_lock();
    old = g_oom_exc;
    g_oom_exc = exc;
    hao_gc_unlock();
    if (old && old != exc) hao_gc_remove_root(old);
    if (exc) hao_gc_add_root(exc);
}

HaoString* hao_gc_last_miss_file(void) {
    const char* f = g_last_miss_file ? g_last_miss_file : "";
    return hao_str_from_cstr(f);
}

int64_t hao_gc_last_miss_line(void) { return (int64_t)g_last_miss_line; }
int64_t hao_gc_last_miss_col(void) { return (int64_t)g_last_miss_col; }

void hao_weak_register(void* weak_obj, void* referent, int32_t soft) {
    int i;
    if (!weak_obj) return;
    hao_gc_lock();
    for (i = 0; i < g_weak_n; ++i) {
        if (g_weaks[i].used && g_weaks[i].wr == weak_obj) {
            g_weaks[i].referent = referent;
            g_weaks[i].soft = soft ? 1 : 0;
            hao_gc_unlock();
            return;
        }
    }
    if (g_weak_n >= g_weak_cap) {
        if (!gc_weak_grow_locked()) {
            hao_gc_unlock();
            hao_report_fatal("gc", "weak ref table grow failed");
            return;
        }
    }
    g_weaks[g_weak_n].wr = weak_obj;
    g_weaks[g_weak_n].referent = referent;
    g_weaks[g_weak_n].soft = soft ? 1 : 0;
    g_weaks[g_weak_n].used = 1;
    g_weak_n += 1;
    hao_gc_unlock();
}

void* hao_weak_get(void* weak_obj) {
    int i;
    void* r = NULL;
    if (!weak_obj) return NULL;
    hao_gc_lock();
    for (i = 0; i < g_weak_n; ++i) {
        if (g_weaks[i].used && g_weaks[i].wr == weak_obj) {
            r = g_weaks[i].referent;
            break;
        }
    }
    hao_gc_unlock();
    return r;
}

void hao_weak_clear(void* weak_obj) {
    int i;
    if (!weak_obj) return;
    hao_gc_lock();
    for (i = 0; i < g_weak_n; ++i) {
        if (g_weaks[i].used && g_weaks[i].wr == weak_obj) {
            g_weaks[i].referent = NULL;
            break;
        }
    }
    hao_gc_unlock();
}
