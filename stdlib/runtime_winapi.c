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
#define HAO_CRITSEC_BUF           64
#define HAO_CONDVAR_BUF           8

typedef void*    (__stdcall *Fn_CreateThread)(void*, size_t, HaoWinThreadProc, void*, uint32_t, uint32_t*);
typedef uint32_t (__stdcall *Fn_WaitForSingleObject)(void*, uint32_t);
typedef int      (__stdcall *Fn_CloseHandle)(void*);
typedef void     (__stdcall *Fn_Sleep)(uint32_t);
typedef int      (__stdcall *Fn_SwitchToThread)(void);
typedef uint32_t (__stdcall *Fn_GetCurrentThreadId)(void);
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
typedef long     (__stdcall *Fn_UnhandledExceptionFilter)(void* /*EXCEPTION_POINTERS*/);
typedef Fn_UnhandledExceptionFilter (__stdcall *Fn_SetUnhandledExceptionFilter)(
    Fn_UnhandledExceptionFilter);
typedef void*    (__stdcall *Fn_GetModuleHandleA)(const char*);
/* CaptureStackBackTrace：kernel32 导出（同 RtlCaptureStackBackTrace） */
typedef uint16_t (__stdcall *Fn_CaptureStackBackTrace)(uint32_t framesToSkip,
                                                       uint32_t framesToCapture,
                                                       void** backTrace,
                                                       uint32_t* backTraceHash);

typedef struct {
    void* lib;
    Fn_CreateThread CreateThread;
    Fn_WaitForSingleObject WaitForSingleObject;
    Fn_CloseHandle CloseHandle;
    Fn_Sleep Sleep;
    Fn_SwitchToThread SwitchToThread;
    Fn_GetCurrentThreadId GetCurrentThreadId;
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
    Fn_SetUnhandledExceptionFilter SetUnhandledExceptionFilter;
    Fn_GetModuleHandleA GetModuleHandleA;
    Fn_CaptureStackBackTrace CaptureStackBackTrace;
} HaoWinApi;

static HaoWinApi g_win;
static int g_win_ready = 0;
static int g_crash_filter_installed = 0;

/*
 * 静默 AV 时控制台往往什么都不打就回到提示符。
 * 未处理异常过滤器：写 stderr + 仓库 cwd 下 hao-crash.log。
 * EXCEPTION_POINTERS：slots[0]=EXCEPTION_RECORD*，slots[1]=CONTEXT*。
 * EXCEPTION_RECORD（x64）：+0 code, +4 flags, +16 ExceptionAddress,
 *   +24 NumberParameters, +32 ExceptionInformation[]。
 * CONTEXT（x64）：Rax 起连续 16×8 @ HAO_CONTEXT_RAX_OFF；Rip @ +0xF8。
 */
#define HAO_CONTEXT_RAX_OFF       0x78u
#define HAO_CONTEXT_RSP_OFF       0x98u
#define HAO_CONTEXT_GPRS_BYTES    (16 * 8)
#define HAO_CONTEXT_RIP_OFF 0xF8u
#define HAO_CRASH_STACK_MAX 48

static void hao_crash_fprint_rva(FILE* f, const char* label, const void* p,
                                 void* base) {
    if (!f) return;
    if (base && p && (uintptr_t)p >= (uintptr_t)base) {
        fprintf(f, "%s=%p rva=0x%llx\n", label, p,
                (unsigned long long)((uintptr_t)p - (uintptr_t)base));
    } else {
        fprintf(f, "%s=%p rva=?\n", label, p);
    }
}

static long __stdcall hao_crash_uef(void* ep) {
    uint32_t code = 0, flags = 0, nparam = 0;
    const void* at = NULL;
    uint32_t tid = 0;
    void* base = NULL;
    uintptr_t rva = 0;
    uintptr_t info0 = 0, info1 = 0;
    const unsigned char* ctx = NULL;
    uintptr_t gprs[16];
    uintptr_t rip = 0, rsp = 0;
    void* stack[HAO_CRASH_STACK_MAX];
    uint16_t nstack = 0;
    int i;

    memset(gprs, 0, sizeof gprs);
    memset(stack, 0, sizeof stack);
    if (g_win.GetCurrentThreadId) tid = g_win.GetCurrentThreadId();
    if (g_win.GetModuleHandleA) base = g_win.GetModuleHandleA(NULL);
    if (ep) {
        void** slots = (void**)ep;
        unsigned char* er = (unsigned char*)slots[0];
        ctx = (const unsigned char*)slots[1];
        if (er) {
            code = *(uint32_t*)er;
            flags = *(uint32_t*)(er + 4);
            at = *(const void**)(er + 16);
            nparam = *(uint32_t*)(er + 24);
            if (nparam > 0) info0 = *(uintptr_t*)(er + 32);
            if (nparam > 1) info1 = *(uintptr_t*)(er + 32 + sizeof(uintptr_t));
        }
        if (ctx) {
            memcpy(gprs, ctx + HAO_CONTEXT_RAX_OFF, HAO_CONTEXT_GPRS_BYTES);
            rip = *(const uintptr_t*)(ctx + HAO_CONTEXT_RIP_OFF);
            rsp = *(const uintptr_t*)(ctx + HAO_CONTEXT_RSP_OFF);
        }
    }
    if (base && at && (uintptr_t)at >= (uintptr_t)base)
        rva = (uintptr_t)at - (uintptr_t)base;
    if (g_win.CaptureStackBackTrace)
        nstack = g_win.CaptureStackBackTrace(0, HAO_CRASH_STACK_MAX, stack, NULL);

    fprintf(stderr,
            "\nFATAL: 未处理异常 0x%08X 于 %p rva=0x%llx base=%p tid=%u（详见 hao-crash.log）\n",
            (unsigned)code, at, (unsigned long long)rva, base, (unsigned)tid);
    fflush(stderr);
    {
        FILE* f = fopen("hao-crash.log", "a");
        if (f) {
            fprintf(f, "---- crash ----\n");
            hao_dbg_fprint_time(f);
            hao_dbg_fprint_where(f);
            fprintf(f, "exception=0x%08X flags=0x%X addr=%p rva=0x%llx base=%p tid=%u\n",
                    (unsigned)code, (unsigned)flags, at, (unsigned long long)rva,
                    base, (unsigned)tid);
            hao_dbg_fprint_src_loc(f);
            hao_dbg_fprint_stack(f);
            if (code == 0xC0000005u) {
                fprintf(f, "access=%s av_addr=%p\n",
                        info0 ? "write" : "read", (void*)info1);
            } else if (nparam > 0) {
                fprintf(f, "info0=0x%llx info1=0x%llx nparam=%u\n",
                        (unsigned long long)info0, (unsigned long long)info1,
                        (unsigned)nparam);
            }
            if (ctx) {
                fprintf(f, "rax=%p rcx=%p rdx=%p rbx=%p\n",
                        (void*)gprs[0], (void*)gprs[1], (void*)gprs[2],
                        (void*)gprs[3]);
                fprintf(f, "rsp=%p rbp=%p rsi=%p rdi=%p\n",
                        (void*)rsp, (void*)gprs[5], (void*)gprs[6],
                        (void*)gprs[7]);
                fprintf(f, "r8=%p r9=%p r10=%p r11=%p\n",
                        (void*)gprs[8], (void*)gprs[9], (void*)gprs[10],
                        (void*)gprs[11]);
                fprintf(f, "r12=%p r13=%p r14=%p r15=%p\n",
                        (void*)gprs[12], (void*)gprs[13], (void*)gprs[14],
                        (void*)gprs[15]);
                hao_crash_fprint_rva(f, "rip", (const void*)rip, base);
                /* rcx 常为 Win64 第一形参（hao_str_len 的 s） */
                hao_crash_fprint_rva(f, "rcx", (const void*)gprs[1], base);
            }
            fprintf(f, "native_stack=%u\n", (unsigned)nstack);
            for (i = 0; i < (int)nstack; ++i) {
                const void* fp = stack[i];
                if (base && fp && (uintptr_t)fp >= (uintptr_t)base)
                    fprintf(f, "  #%d %p rva=0x%llx\n", i, fp,
                            (unsigned long long)((uintptr_t)fp - (uintptr_t)base));
                else
                    fprintf(f, "  #%d %p rva=?\n", i, fp);
            }
            hao_gc_fprint_debug_snapshot(f);
            fprintf(f, "---- end ----\n");
            fflush(f);
            fclose(f);
        }
    }
    return 1; /* EXCEPTION_EXECUTE_HANDLER */
}

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
    /* 可选：崩溃日志 + 主模块基址（算 RVA） */
    g_win.GetModuleHandleA =
        (Fn_GetModuleHandleA)hao_dl_sym(g_win.lib, "GetModuleHandleA");
    g_win.CaptureStackBackTrace =
        (Fn_CaptureStackBackTrace)hao_dl_sym(g_win.lib, "RtlCaptureStackBackTrace");
    if (!g_win.CaptureStackBackTrace)
        g_win.CaptureStackBackTrace =
            (Fn_CaptureStackBackTrace)hao_dl_sym(g_win.lib, "CaptureStackBackTrace");
    g_win.SetUnhandledExceptionFilter =
        (Fn_SetUnhandledExceptionFilter)hao_dl_sym(
            g_win.lib, "SetUnhandledExceptionFilter");
    if (g_win.SetUnhandledExceptionFilter && !g_crash_filter_installed) {
        g_win.SetUnhandledExceptionFilter(hao_crash_uef);
        g_crash_filter_installed = 1;
    }
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

/* ---- VirtualAlloc / VirtualFree（mspan 页堆）---- */
typedef void* (__stdcall *Fn_VirtualAlloc)(void*, size_t, uint32_t, uint32_t);
typedef int   (__stdcall *Fn_VirtualFree)(void*, size_t, uint32_t);
static Fn_VirtualAlloc g_VirtualAlloc;
static Fn_VirtualFree  g_VirtualFree;
#define HAO_MEM_COMMIT     0x1000u
#define HAO_MEM_RESERVE    0x2000u
#define HAO_PAGE_READWRITE 0x04u
#define HAO_MEM_RELEASE    0x8000u

static void hao_win_vmem_ensure(void) {
    if (g_VirtualAlloc) return;
    if (!hao_win_ensure()) return;
    g_VirtualAlloc = (Fn_VirtualAlloc)hao_dl_sym(g_win.lib, "VirtualAlloc");
    g_VirtualFree = (Fn_VirtualFree)hao_dl_sym(g_win.lib, "VirtualFree");
}

void* hao_os_valloc(size_t n) {
    hao_win_vmem_ensure();
    if (!g_VirtualAlloc || n == 0) return NULL;
    return g_VirtualAlloc(NULL, n, HAO_MEM_COMMIT | HAO_MEM_RESERVE, HAO_PAGE_READWRITE);
}

void hao_os_vfree(void* p, size_t n) {
    (void)n;
    hao_win_vmem_ensure();
    if (!g_VirtualFree || !p) return;
    g_VirtualFree(p, 0, HAO_MEM_RELEASE);
}

#endif /* _WIN32 */
