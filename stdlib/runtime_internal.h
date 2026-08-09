/*
 * HaoLang 运行时内部共享头
 * ------------------------------------------------------------
 * 只在运行时库的各 runtime_*.c 之间共享，编译器（IRGen）不需要它——
 * IR 里直接 declare 用到的 hao_* / gc_* 函数。
 *
 * 拆分原则：每个 runtime_*.c 对应一个功能模块（GC / 字符串 / 数组 /
 * 对象 / 装箱 / 异常 / 打印 / panic / 控制台），模块间只通过本头
 * 暴露少量交叉依赖。
 */
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>

#ifndef _WIN32
#include <unistd.h>
#else
#include <intrin.h>   /* _AddressOfReturnAddress（栈扫描）；禁止 windows.h */
#endif

/* ============================================================
 *  GC（runtime_gc.c）—— v3：精确堆扫 + 非移动分代 + 写屏障
 * ============================================================ */

/* 扫描种类（存在 GCBlock.scan_kind） */
#define GC_KIND_OPAQUE  0   /* 叶：String / 值类型 box，用户区无堆指针 */
#define GC_KIND_SLOTS   1   /* 对象/闭包：scan_meta 为最多 32 槽位图；>32 槽走 FULL */
#define GC_KIND_ARRAY   2   /* 数组：scan_meta 低位 is_ptr；扫元素区 */
#define GC_KIND_FULL    3   /* 过渡：整块用户区保守扫 */

/* typed 分配：返回 16 对齐、已清零的用户指针。kind/meta 写入块头。 */
void* gc_alloc_ex(size_t n, uint8_t kind, uint64_t meta);
/* 薄包装 → gc_alloc_ex(n, GC_KIND_FULL, 0) */
void* gc_alloc(size_t n);

/* 混合写屏障：dst 须为槽地址；MARK 期 shade(old)+shade(new)；old→young 记 remset。 */
void hao_gc_barrier(void* dst, void* new_val);
/*
 * Hao 精确根（shadow stack）：登记/水位回退；STW 始终扫 *slot。
 * os_block 期间另扫 GPR+有界 C 叶（诚实双轨，见 docs/IR与GC契约.md R2）。
 */
size_t hao_gc_root_watermark(void);
void   hao_gc_root_push(void** slot);
void   hao_gc_root_unwind(size_t wm);
/* mark worker 累计推进的 grey 块数（v0.54+） */
int64_t hao_gc_mark_worker_steps(void);
int64_t hao_gc_remset_count(void);
/* MARK 期 shade 指针（供单指针补标；非 MARK 为 no-op）。 */
void hao_gc_shade(void* p);
/*
 * 数组扩容：在同一把 GC 锁内 memcpy + 补 shade（中间无 safepoint）。
 * 禁止「先 memcpy 再可打断的逐元素 shade」——否则黑父未扫子会被 sweep。
 */
void hao_gc_array_copy_and_shade(char* newbase, const char* oldbase, size_t nbytes,
                                 int64_t len, int is_ptr);

/* 协作式 safepoint：STW 请求时溅射 GPR 并 park。勿持 GC 锁调用。 */
void hao_gc_safepoint(void);

/* 显式触发一次 major 回收（经过寄存器垫片）。*/
void gc_collect(void);

/* 记录主线程栈基址。Windows 由控制台初始化在 main 前调用；
 * Linux 由 runtime_gc.c 的 constructor 自行调用。*/
void gc_init(void);

/* 注册/注销一个外部 GC 根（线程闭包 env 等跨线程引用）。*/
void hao_gc_add_root(void* p);
/* 仅当 p 落在某 GC 块用户区时挂根；持锁查堆、**无 safepoint**（channel 整型载荷用） */
void hao_gc_add_root_if_heap(void* p);
/* 是否指向某 GC 块用户区（持 GC 锁查堆链；内含 safepoint——禁用于挂根前探测） */
int8_t hao_gc_is_heap_ptr(void* p);
/* 已废除全标活（恒 0）；请用 hao_gc_stw_incomplete */
int64_t hao_gc_stw_mark_all_fallbacks(void);
/* STW 未齐 park、本轮跳过 sweep 的累计次数（v0.50.4+） */
int64_t hao_gc_stw_incomplete(void);
/* 已进入并发标记窗口的回收轮次（v0.51+） */
int64_t hao_gc_concurrent_mark_cycles(void);
/* 堆上用户区字节合计（alloc/sweep 维护；比 liveBytes 更能反映当前堆压） */
int64_t hao_gc_heap_bytes(void);
/* mark assist 累计推进的 grey 块数（v0.52+） */
int64_t hao_gc_mark_assist_steps(void);
void hao_gc_remove_root(void* p);

/* 注册静态/全局 ptr 槽地址：标记时解引用 *slot（类静态 String/对象字段等）。*/
void hao_gc_add_root_slot(void* slot);

/* 原生资源 finalizer：obj 须为 gc_alloc* 返回的用户指针。 */
typedef void (*HaoFinalizer)(void* user);
void hao_gc_set_finalizer(void* obj, HaoFinalizer fn);
void hao_gc_clear_finalizer(void* obj);
int64_t hao_gc_finalizer_runs(void);
int64_t hao_gc_finalizer_sets(void);

/* GC 详情只读快照（供 stdlib gc.GC）*/
int64_t hao_gc_live_bytes(void);
int64_t hao_gc_threshold(void);
int64_t hao_gc_allocated_since(void);
int64_t hao_gc_block_count(void);
int64_t hao_gc_collect_count(void);
int64_t hao_gc_minor_count(void);
int64_t hao_gc_major_count(void);
int64_t hao_gc_nursery_bytes(void);
int64_t hao_gc_registered_threads(void);
/*
 * 一次持锁写入 Hao 对象字段（槽 0=vtable；槽 1..16 见 gc.GcStats 声明序）。
 * 供 /api/gc 等避免十几次独立加锁。
 */
void hao_gc_stats(void* obj);
/* 终止失败 abort MARK 累计次数（v0.53.3+） */
int64_t hao_gc_mark_abort_cycles(void);

/* 测试辅助（finalizer 复活冒烟；正式 API 勿依赖） */
void  hao_gc_test_arm_rescue_finalizer(void* obj);
void  hao_gc_test_arm_nop_finalizer(void* obj);
void* hao_gc_test_get_rescue(void);
void  hao_gc_test_clear_rescue(void);

/* 进程资源快照（runtime_proc.c；供 os.Process，对标 .NET Process） */
int64_t hao_proc_working_set_bytes(void);
int64_t hao_proc_private_bytes(void);
int64_t hao_proc_handle_count(void);
int64_t hao_proc_thread_count(void);
int32_t hao_proc_cpu_percent(void); /* 相对上次调用；首次 -1；0～100×逻辑核可略超 */
int64_t hao_proc_uptime_ms(void);

/* 注册/注销当前线程（GC v2 STW）*/
void gc_register_thread(void);
void gc_unregister_thread(void);
/* Mark parked across OS block (Sleep/accept/recv); stack remains scannable. */
void hao_gc_os_block_enter(void);
void hao_gc_os_block_leave(void);
/* 持锁即将 cond_wait：只挂 park，勿 safepoint（见 runtime_thread 池 worker）。 */
void hao_gc_os_block_arm(void);
void hao_gc_os_block_disarm(void);

/* channel（runtime_channel.c）：句柄在 Long? 盒；载荷 i64；堆指针入队挂根 */
int8_t  hao_chan_make(int64_t* unit, int32_t capacity);
int32_t hao_chan_send(int64_t* unit, int64_t bits);
int64_t hao_chan_recv(int64_t* unit);
int32_t hao_chan_try_send(int64_t* unit, int64_t bits);
int32_t hao_chan_try_recv(int64_t* unit, int64_t* out);
void    hao_chan_close(int64_t* unit);

/* ============================================================
 *  动态库加载（runtime_dynload.c，5.12）
 * ============================================================ */
void* hao_dl_open(const char* name);
void* hao_dl_sym(void* handle, const char* name);
void hao_dl_close(void* handle);

#ifdef _WIN32
#define HAO_WIN_CRITSEC_BYTES  64
#define HAO_WIN_CONDVAR_BYTES  8
#define HAO_WIN_CP_UTF8        65001u

char*    hao_win_stack_base(void);
uint32_t hao_win_get_current_thread_id(void);
void     hao_win_sleep_ms(uint32_t ms);
int      hao_win_switch_to_thread(void);
typedef  uint32_t (*HaoWinThreadProc)(void*);
void*    hao_win_create_thread(HaoWinThreadProc start, void* arg);
void     hao_win_join_close(void* handle);
void     hao_win_close_handle(void* handle);
void     hao_win_crit_init(void* cs);
void     hao_win_crit_enter(void* cs);
void     hao_win_crit_leave(void* cs);
void     hao_win_cond_init(void* cv);
int      hao_win_cond_wait(void* cv, void* cs);
void     hao_win_cond_wake(void* cv);
void     hao_win_cond_wake_all(void* cv);
/* 成功扫到返回 1；无法挂起/取上下文返回 0（调用方须放弃本轮回收）。 */
int      hao_win_suspend_scan(uint32_t tid, char* stack_top,
                              void (*scan)(char*, char*));
int64_t  hao_win_now_ns(void);
uint32_t hao_win_get_console_output_cp(void);
void     hao_win_set_console_output_cp(uint32_t cp);
void     hao_win_set_console_cp(uint32_t cp);
#endif

/* ============================================================
 *  Panic（runtime_panic.c）
 * ============================================================ */
void hao_panic_index(int64_t idx, int64_t len);
void hao_panic_overflow(void);
void hao_panic_msg(const char* msg);

/* ============================================================
 *  数组布局（runtime_array.c）
 * ============================================================
 *  base: [pad:i64][cap:i64][len:i64][esz:i64][elem × esz]
 *  返回给 IR 的指针 p 指向元素区（须 16 对齐）：
 *    p[-8]=esz，p[-16]=len，p[-24]=cap，p[-32]=pad
 */
#define HAO_ARR_HEADER (4 * (int64_t)sizeof(int64_t))   /* pad+cap+len+esz = 32 */
#define HAO_ARR_CAP_OFF (3 * (int64_t)sizeof(int64_t))
#define HAO_ARR_LEN_OFF (2 * (int64_t)sizeof(int64_t))
#define HAO_ARR_ESZ_OFF ((int64_t)sizeof(int64_t))
#define HAO_ARR_CAP_OFF_BASE ((int64_t)sizeof(int64_t))
#define HAO_ARR_LEN_OFF_BASE (2 * (int64_t)sizeof(int64_t))
#define HAO_ARR_ESZ_OFF_BASE (3 * (int64_t)sizeof(int64_t))

/* is_ptr≠0：元素为 GC 指针，精确堆扫扫 [0..len)；写屏障在 push 时触发 */
void*   hao_array_new(int64_t len, int64_t esz, int64_t is_ptr);
int64_t hao_array_len(void* arr);
int64_t hao_array_cap(void* arr);
int64_t hao_array_check(void* arr, int64_t idx);
void*   hao_array_push(void* arr, int64_t value);
int64_t hao_array_pop(void* arr);

/* ============================================================
 *  字符串堆头（runtime_string.c）—— 语言 String = HaoString*
 * ============================================================ */
typedef struct HaoString {
    int32_t len;
    int32_t cap;
    char    data[];
} HaoString;

HaoString* hao_str_alloc(int32_t byte_len);
HaoString* hao_str_from_cstr(const char* c);
HaoString* hao_str_from_bytes(const char* bytes, int32_t byte_len);
HaoString* hao_char_to_str(int32_t cp);
const char* hao_str_cstr(const HaoString* s);
HaoString* hao_str_byte_slice(HaoString* s, int32_t start, int32_t end);
int32_t hao_str_byte_len(HaoString* s);
/* 字节下标 indexOf；from 起搜；未找到 -1 */
int32_t hao_str_byte_index_of(HaoString* s, HaoString* sub, int32_t from);
/* 码点下标 indexOf；from 起搜；未找到 -1（Java 对齐） */
int32_t hao_str_index_of_from(HaoString* s, HaoString* sub, int32_t from);
int32_t hao_str_index_of(HaoString* s, HaoString* sub);

void* hao_parse_int(HaoString* s);
void* hao_parse_long(HaoString* s);
void* hao_parse_uint(HaoString* s);
void* hao_parse_ulong(HaoString* s);
void* hao_parse_float(HaoString* s);
void* hao_parse_double(HaoString* s);
void* hao_parse_bool(HaoString* s);

/* ============================================================
 *  值类型可空装箱（runtime_box.c）
 * ============================================================ */
void*   hao_box_i64(int64_t v);
void*   hao_box_f64(double v);
void*   hao_box_f32(float v);
void*   hao_box_i32(int32_t v);
int64_t hao_unbox_i64(void* p);
double  hao_unbox_f64(void* p);
float   hao_unbox_f32(void* p);
int32_t hao_unbox_i32(void* p);

/* ============================================================
 *  对象（runtime_object.c）
 * ============================================================ */
/* bitmap：bit i=1 表示槽 i 为 GC 指针；vtable/fnptr 槽须为 0 */
void* hao_object_new(int64_t nfields, int64_t bitmap);
