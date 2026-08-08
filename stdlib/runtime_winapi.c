/*
 * HaoLang 运行时 —— Windows API 惰性解析（5.12）
 * ------------------------------------------------------------
 *  通过 hao_dl_open("kernel32.dll") 解析本进程已用到的 Win32 API，
 *  供 gc / thread / time / print 使用。禁止 #include <windows.h>，
 *  禁止产生 __imp_* 导入（链接期不依赖 SDK 的 kernel32.lib 符号表
 *  —— CRT 自带的 kernel32.lib 仍可能因 libcmt DEFAULTLIB 被带上，
 *  那是 CRT 架构层，与本模块无关）。
 */
#include "runtime_internal.h"

#ifdef _WIN32

#include <stdint.h>

/* ---- 手写 ABI 常量 / 不透明缓冲 ---- */
#define HAO_INFINITE              0xFFFFFFFFu
#define HAO_THREAD_SUSPEND_RESUME 0x0002u
#define HAO_THREAD_GET_CONTEXT    0x0008u
#define HAO_CONTEXT_CONTROL       0x00100001u
#define HAO_CONTEXT_INTEGER       0x00100002u
#define HAO_CONTEXT_CONTROL_INTEGER (HAO_CONTEXT_CONTROL | HAO_CONTEXT_INTEGER)
#define HAO_CONTEXT_RAX_OFF       0x78u  /* Rax..R15 连续 16×8 */
#define HAO_CONTEXT_RSP_OFF       0x98u
#define HAO_CONTEXT_FLAGS_OFF     0x30u
#define HAO_CONTEXT_BUF           0x500u
#define HAO_CONTEXT_GPRS_BYTES    (16 * 8)
#define HAO_CRITSEC_BUF           64
#define HAO_CONDVAR_BUF           8

typedef void*    (__stdcall *Fn_CreateThread)(void*, size_t, HaoWinThreadProc, void*, uint32_t, uint32_t*);
typedef uint32_t (__stdcall *Fn_WaitForSingleObject)(void*, uint32_t);
typedef int      (__stdcall *Fn_CloseHandle)(void*);
typedef void     (__stdcall *Fn_Sleep)(uint32_t);
typedef int      (__stdcall *Fn_SwitchToThread)(void);
typedef uint32_t (__stdcall *Fn_GetCurrentThreadId)(void);
typedef void*    (__stdcall *Fn_OpenThread)(uint32_t, int, uint32_t);
typedef uint32_t (__stdcall *Fn_SuspendThread)(void*);
typedef uint32_t (__stdcall *Fn_ResumeThread)(void*);
typedef int      (__stdcall *Fn_GetThreadContext)(void*, void*);
typedef void     (__stdcall *Fn_InitializeCriticalSection)(void*);
typedef void     (__stdcall *Fn_EnterCriticalSection)(void*);
typedef void     (__stdcall *Fn_LeaveCriticalSection)(void*);
typedef void     (__stdcall *Fn_InitializeConditionVariable)(void*);
typedef int      (__stdcall *Fn_SleepConditionVariableCS)(void*, void*, uint32_t);
typedef void     (__stdcall *Fn_WakeConditionVariable)(void*);
typedef void     (__stdcall *Fn_WakeAllConditionVariable)(void*);
typedef void     (__stdcall *Fn_GetSystemTimePreciseAsFileTime)(void*);
typedef uint32_t (__stdcall *Fn_GetConsoleOutputCP)(void);
typedef int      (__stdcall *Fn_SetConsoleOutputCP)(uint32_t);
typedef int      (__stdcall *Fn_SetConsoleCP)(uint32_t);

typedef struct {
    void* lib;
    Fn_CreateThread CreateThread;
    Fn_WaitForSingleObject WaitForSingleObject;
    Fn_CloseHandle CloseHandle;
    Fn_Sleep Sleep;
    Fn_SwitchToThread SwitchToThread;
    Fn_GetCurrentThreadId GetCurrentThreadId;
    Fn_OpenThread OpenThread;
    Fn_SuspendThread SuspendThread;
    Fn_ResumeThread ResumeThread;
    Fn_GetThreadContext GetThreadContext;
    Fn_InitializeCriticalSection InitializeCriticalSection;
    Fn_EnterCriticalSection EnterCriticalSection;
    Fn_LeaveCriticalSection LeaveCriticalSection;
    Fn_InitializeConditionVariable InitializeConditionVariable;
    Fn_SleepConditionVariableCS SleepConditionVariableCS;
    Fn_WakeConditionVariable WakeConditionVariable;
    Fn_WakeAllConditionVariable WakeAllConditionVariable;
    Fn_GetSystemTimePreciseAsFileTime GetSystemTimePreciseAsFileTime;
    Fn_GetConsoleOutputCP GetConsoleOutputCP;
    Fn_SetConsoleOutputCP SetConsoleOutputCP;
    Fn_SetConsoleCP SetConsoleCP;
} HaoWinApi;

static HaoWinApi g_win;
static int g_win_ready = 0;

static int hao_win_ensure(void) {
    if (g_win_ready) return g_win.lib != NULL;
    g_win_ready = 1;
    memset(&g_win, 0, sizeof g_win);
    g_win.lib = hao_dl_open("kernel32.dll");
    if (!g_win.lib) return 0;

#define BIND(name) do { \
        g_win.name = (Fn_##name)hao_dl_sym(g_win.lib, #name); \
        if (!g_win.name) goto fail; \
    } while (0)

    BIND(CreateThread);
    BIND(WaitForSingleObject);
    BIND(CloseHandle);
    BIND(Sleep);
    BIND(SwitchToThread);
    BIND(GetCurrentThreadId);
    BIND(OpenThread);
    BIND(SuspendThread);
    BIND(ResumeThread);
    BIND(GetThreadContext);
    BIND(InitializeCriticalSection);
    BIND(EnterCriticalSection);
    BIND(LeaveCriticalSection);
    BIND(InitializeConditionVariable);
    BIND(SleepConditionVariableCS);
    BIND(WakeConditionVariable);
    BIND(WakeAllConditionVariable);
    BIND(GetSystemTimePreciseAsFileTime);
    BIND(GetConsoleOutputCP);
    BIND(SetConsoleOutputCP);
    BIND(SetConsoleCP);
#undef BIND
    return 1;
fail:
    hao_dl_close(g_win.lib);
    memset(&g_win, 0, sizeof g_win);
    return 0;
}

/* ---- 供其它 runtime_*.c 调用的薄封装 ---- */

uint32_t hao_win_get_current_thread_id(void) {
    if (!hao_win_ensure()) return 0;
    return g_win.GetCurrentThreadId();
}

void hao_win_sleep_ms(uint32_t ms) {
    if (!hao_win_ensure()) return;
    g_win.Sleep(ms);
}

int hao_win_switch_to_thread(void) {
    if (!hao_win_ensure()) return 0;
    return g_win.SwitchToThread();
}

void* hao_win_create_thread(HaoWinThreadProc start, void* arg) {
    if (!hao_win_ensure()) return NULL;
    return g_win.CreateThread(NULL, 0, start, arg, 0, NULL);
}

void hao_win_join_close(void* handle) {
    if (!handle || !hao_win_ensure()) return;
    g_win.WaitForSingleObject(handle, HAO_INFINITE);
    g_win.CloseHandle(handle);
}

void hao_win_close_handle(void* handle) {
    if (!handle || !hao_win_ensure()) return;
    g_win.CloseHandle(handle);
}

void hao_win_crit_init(void* cs /* HAO_CRITSEC_BUF */) {
    if (!hao_win_ensure()) return;
    g_win.InitializeCriticalSection(cs);
}
void hao_win_crit_enter(void* cs) {
    if (!hao_win_ensure()) return;
    g_win.EnterCriticalSection(cs);
}
void hao_win_crit_leave(void* cs) {
    if (!hao_win_ensure()) return;
    g_win.LeaveCriticalSection(cs);
}

void hao_win_cond_init(void* cv /* HAO_CONDVAR_BUF */) {
    if (!hao_win_ensure()) return;
    g_win.InitializeConditionVariable(cv);
}
int hao_win_cond_wait(void* cv, void* cs) {
    if (!hao_win_ensure()) return 0;
    return g_win.SleepConditionVariableCS(cv, cs, HAO_INFINITE);
}
void hao_win_cond_wake(void* cv) {
    if (!hao_win_ensure()) return;
    g_win.WakeConditionVariable(cv);
}
void hao_win_cond_wake_all(void* cv) {
    if (!hao_win_ensure()) return;
    g_win.WakeAllConditionVariable(cv);
}

/* 挂起 tid：扫 GPR + 栈。失败返回 0（禁止静默当成功）。 */
int hao_win_suspend_scan(uint32_t tid, char* stack_top,
                         void (*scan)(char*, char*)) {
    if (!hao_win_ensure() || !scan) return 0;
    void* h = NULL;
    for (int attempt = 0; attempt < 128 && !h; ++attempt) {
        h = g_win.OpenThread(HAO_THREAD_SUSPEND_RESUME | HAO_THREAD_GET_CONTEXT,
                             0, tid);
        if (!h) hao_win_switch_to_thread();
    }
    if (!h) return 0;
    uint32_t susp = (uint32_t)-1;
    for (int attempt = 0; attempt < 128; ++attempt) {
        susp = g_win.SuspendThread(h);
        if (susp != (uint32_t)-1) break;
        hao_win_switch_to_thread();
    }
    if (susp == (uint32_t)-1) {
        g_win.CloseHandle(h);
        return 0;
    }
    unsigned char ctx[HAO_CONTEXT_BUF];
    memset(ctx, 0, sizeof ctx);
    *(uint32_t*)(ctx + HAO_CONTEXT_FLAGS_OFF) = HAO_CONTEXT_CONTROL_INTEGER;
    if (!g_win.GetThreadContext(h, ctx)) {
        g_win.ResumeThread(h);
        g_win.CloseHandle(h);
        return 0;
    }
    char* gprs = (char*)(ctx + HAO_CONTEXT_RAX_OFF);
    scan(gprs, gprs + HAO_CONTEXT_GPRS_BYTES);
    char* sp = *(char**)(ctx + HAO_CONTEXT_RSP_OFF);
    if (stack_top && sp && sp < stack_top &&
        (size_t)(stack_top - sp) <= (size_t)8 * 1024 * 1024)
        scan(sp, stack_top);
    g_win.ResumeThread(h);
    g_win.CloseHandle(h);
    return 1;
}

/* FILETIME → Unix 纳秒（与旧 runtime_time 公式一致） */
int64_t hao_win_now_ns(void) {
    if (!hao_win_ensure()) return 0;
    struct { uint32_t lo, hi; } ft;
    g_win.GetSystemTimePreciseAsFileTime(&ft);
    uint64_t q = ((uint64_t)ft.hi << 32) | ft.lo;
    return (int64_t)((q - 116444736000000000ULL) * 100);
}

uint32_t hao_win_get_console_output_cp(void) {
    if (!hao_win_ensure()) return 0;
    return g_win.GetConsoleOutputCP();
}
void hao_win_set_console_output_cp(uint32_t cp) {
    if (!hao_win_ensure()) return;
    g_win.SetConsoleOutputCP(cp);
}
void hao_win_set_console_cp(uint32_t cp) {
    if (!hao_win_ensure()) return;
    g_win.SetConsoleCP(cp);
}

#endif /* _WIN32 */
