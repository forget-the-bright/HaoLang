/*
 * HaoLang 运行时 —— 进程资源统计（v0.49.1）
 * ------------------------------------------------------------
 *  对标 .NET Process：工作集 / 专用字节 / 句柄数 / OS 线程数 /
 *  CPU%（相对上次采样）/ 进程运行时长。
 *  Windows：kernel32 dynload（5.12，无 windows.h / 无 SDK .lib）。
 *  Linux：读 /proc/self（尽力而为）。
 */
#include "runtime_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#include <stdint.h>

typedef void* (__stdcall *Fn_GetCurrentProcess)(void);
typedef uint32_t (__stdcall *Fn_GetCurrentProcessId)(void);
typedef int (__stdcall *Fn_K32GetProcessMemoryInfo)(void*, void*, uint32_t);
typedef int (__stdcall *Fn_GetProcessHandleCount)(void*, uint32_t*);
typedef int (__stdcall *Fn_GetProcessTimes)(void*, void*, void*, void*, void*);
typedef void (__stdcall *Fn_GetSystemTimeAsFileTime)(void*);
typedef void (__stdcall *Fn_GetSystemInfo)(void*);
typedef void* (__stdcall *Fn_CreateToolhelp32Snapshot)(uint32_t, uint32_t);
typedef int (__stdcall *Fn_Thread32First)(void*, void*);
typedef int (__stdcall *Fn_Thread32Next)(void*, void*);
typedef int (__stdcall *Fn_CloseHandle)(void*);

/* PROCESS_MEMORY_COUNTERS_EX（手写布局） */
typedef struct {
    uint32_t cb;
    uint32_t PageFaultCount;
    uint64_t PeakWorkingSetSize;
    uint64_t WorkingSetSize;
    uint64_t QuotaPeakPagedPoolUsage;
    uint64_t QuotaPagedPoolUsage;
    uint64_t QuotaPeakNonPagedPoolUsage;
    uint64_t QuotaNonPagedPoolUsage;
    uint64_t PagefileUsage;
    uint64_t PeakPagefileUsage;
    uint64_t PrivateUsage;
} HaoProcMemEx;

typedef struct {
    uint32_t dwLength;
    uint32_t cntUsage;
    uint32_t th32ThreadID;
    uint32_t th32OwnerProcessID;
    int32_t  tpBasePri;
    int32_t  tpDeltaPri;
    uint32_t dwFlags;
} HaoThreadEntry32;

/* SYSTEM_INFO 前若干字段够用 */
typedef struct {
    uint16_t wProcessorArchitecture;
    uint16_t wReserved;
    uint32_t dwPageSize;
    void*    lpMinimumApplicationAddress;
    void*    lpMaximumApplicationAddress;
    uint64_t dwActiveProcessorMask;
    uint32_t dwNumberOfProcessors;
} HaoSystemInfo;

#define HAO_TH32CS_SNAPTHREAD 0x00000004u

typedef struct {
    void* lib;
    Fn_GetCurrentProcess GetCurrentProcess;
    Fn_GetCurrentProcessId GetCurrentProcessId;
    Fn_K32GetProcessMemoryInfo K32GetProcessMemoryInfo;
    Fn_GetProcessHandleCount GetProcessHandleCount;
    Fn_GetProcessTimes GetProcessTimes;
    Fn_GetSystemTimeAsFileTime GetSystemTimeAsFileTime;
    Fn_GetSystemInfo GetSystemInfo;
    Fn_CreateToolhelp32Snapshot CreateToolhelp32Snapshot;
    Fn_Thread32First Thread32First;
    Fn_Thread32Next Thread32Next;
    Fn_CloseHandle CloseHandle;
} HaoProcApi;

static HaoProcApi g_proc;
static int g_proc_ready = 0;

static int hao_proc_ensure(void) {
    if (g_proc_ready) return g_proc.lib != NULL;
    g_proc_ready = 1;
    memset(&g_proc, 0, sizeof g_proc);
    g_proc.lib = hao_dl_open("kernel32.dll");
    if (!g_proc.lib) return 0;
#define BIND(name) do { \
        g_proc.name = (Fn_##name)hao_dl_sym(g_proc.lib, #name); \
        if (!g_proc.name) goto fail; \
    } while (0)
    BIND(GetCurrentProcess);
    BIND(GetCurrentProcessId);
    BIND(K32GetProcessMemoryInfo);
    BIND(GetProcessHandleCount);
    BIND(GetProcessTimes);
    BIND(GetSystemTimeAsFileTime);
    BIND(GetSystemInfo);
    BIND(CreateToolhelp32Snapshot);
    BIND(Thread32First);
    BIND(Thread32Next);
    BIND(CloseHandle);
#undef BIND
    return 1;
fail:
    hao_dl_close(g_proc.lib);
    memset(&g_proc, 0, sizeof g_proc);
    return 0;
}

static int hao_proc_mem(HaoProcMemEx* out) {
    if (!hao_proc_ensure() || !out) return 0;
    memset(out, 0, sizeof *out);
    out->cb = (uint32_t)sizeof(HaoProcMemEx);
    void* hp = g_proc.GetCurrentProcess();
    return g_proc.K32GetProcessMemoryInfo(hp, out, out->cb) != 0;
}

int64_t hao_proc_working_set_bytes(void) {
    HaoProcMemEx m;
    if (!hao_proc_mem(&m)) return 0;
    return (int64_t)m.WorkingSetSize;
}

int64_t hao_proc_private_bytes(void) {
    HaoProcMemEx m;
    if (!hao_proc_mem(&m)) return 0;
    return (int64_t)m.PrivateUsage;
}

int64_t hao_proc_handle_count(void) {
    if (!hao_proc_ensure()) return 0;
    uint32_t n = 0;
    void* hp = g_proc.GetCurrentProcess();
    if (!g_proc.GetProcessHandleCount(hp, &n)) return 0;
    return (int64_t)n;
}

int64_t hao_proc_thread_count(void) {
    if (!hao_proc_ensure()) return 0;
    uint32_t pid = g_proc.GetCurrentProcessId();
    void* snap = g_proc.CreateToolhelp32Snapshot(HAO_TH32CS_SNAPTHREAD, 0);
    if (!snap || snap == (void*)(intptr_t)-1) return 0;
    HaoThreadEntry32 te;
    memset(&te, 0, sizeof te);
    te.dwLength = (uint32_t)sizeof te;
    int64_t n = 0;
    if (g_proc.Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid) n += 1;
        } while (g_proc.Thread32Next(snap, &te));
    }
    g_proc.CloseHandle(snap);
    return n;
}

static int64_t hao_ft_to_100ns(const uint8_t ft[8]) {
    return (int64_t)(
        (uint64_t)ft[0] | ((uint64_t)ft[1] << 8) | ((uint64_t)ft[2] << 16) |
        ((uint64_t)ft[3] << 24) | ((uint64_t)ft[4] << 32) |
        ((uint64_t)ft[5] << 40) | ((uint64_t)ft[6] << 48) |
        ((uint64_t)ft[7] << 56));
}

static int64_t g_cpu_last_wall = 0;
static int64_t g_cpu_last_proc = 0;

int32_t hao_proc_cpu_percent(void) {
    if (!hao_proc_ensure()) return -1;
    uint8_t create[8], exit_t[8], kernel[8], user[8], now[8];
    void* hp = g_proc.GetCurrentProcess();
    if (!g_proc.GetProcessTimes(hp, create, exit_t, kernel, user)) return -1;
    g_proc.GetSystemTimeAsFileTime(now);
    int64_t wall = hao_ft_to_100ns(now);
    int64_t proc = hao_ft_to_100ns(kernel) + hao_ft_to_100ns(user);
    if (g_cpu_last_wall == 0) {
        g_cpu_last_wall = wall;
        g_cpu_last_proc = proc;
        return -1; /* 首次采样，尚无区间 */
    }
    int64_t dw = wall - g_cpu_last_wall;
    int64_t dp = proc - g_cpu_last_proc;
    g_cpu_last_wall = wall;
    g_cpu_last_proc = proc;
    if (dw <= 0) return 0;
    HaoSystemInfo si;
    memset(&si, 0, sizeof si);
    g_proc.GetSystemInfo(&si);
    int64_t cores = si.dwNumberOfProcessors > 0 ? (int64_t)si.dwNumberOfProcessors : 1;
    /* (进程 CPU 时间 / 墙钟 / 核数) × 100 → 约 0～100 */
    int64_t pct = (dp * 100) / (dw * cores);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return (int32_t)pct;
}

int64_t hao_proc_uptime_ms(void) {
    if (!hao_proc_ensure()) return 0;
    uint8_t create[8], exit_t[8], kernel[8], user[8], now[8];
    void* hp = g_proc.GetCurrentProcess();
    if (!g_proc.GetProcessTimes(hp, create, exit_t, kernel, user)) return 0;
    g_proc.GetSystemTimeAsFileTime(now);
    int64_t c = hao_ft_to_100ns(create);
    int64_t n = hao_ft_to_100ns(now);
    if (n <= c) return 0;
    return (n - c) / 10000; /* 100ns → ms */
}

#else /* POSIX：/proc 尽力而为 */

#include <dirent.h>
#include <unistd.h>
#include <time.h>

static int64_t hao_read_status_kb(const char* key) {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    int64_t v = 0;
    size_t klen = strlen(key);
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, klen) == 0 && line[klen] == ':') {
            char* p = line + klen + 1;
            while (*p == ' ' || *p == '\t') p++;
            v = (int64_t)strtoll(p, NULL, 10) * 1024;
            break;
        }
    }
    fclose(f);
    return v;
}

int64_t hao_proc_working_set_bytes(void) {
    return hao_read_status_kb("VmRSS");
}

int64_t hao_proc_private_bytes(void) {
    /* 无完美对应；用 VmData 近似「程序数据段」 */
    return hao_read_status_kb("VmData");
}

int64_t hao_proc_handle_count(void) {
    DIR* d = opendir("/proc/self/fd");
    if (!d) return 0;
    int64_t n = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        n += 1;
    }
    closedir(d);
    return n;
}

int64_t hao_proc_thread_count(void) {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    int64_t v = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "Threads:", 8) == 0) {
            v = (int64_t)strtoll(line + 8, NULL, 10);
            break;
        }
    }
    fclose(f);
    return v;
}

static int64_t g_cpu_last_wall_ns = 0;
static int64_t g_cpu_last_proc_ns = 0;

int32_t hao_proc_cpu_percent(void) {
    FILE* f = fopen("/proc/self/stat", "r");
    if (!f) return -1;
    /* 跳过 comm 字段（可能含空格/括号） */
    unsigned long ut = 0, st = 0;
    int n = fscanf(f,
        "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
        &ut, &st);
    fclose(f);
    if (n != 2) return -1;
    long clk = sysconf(_SC_CLK_TCK);
    if (clk <= 0) clk = 100;
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores <= 0) cores = 1;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    int64_t wall = (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
    int64_t proc = ((int64_t)ut + (int64_t)st) * (1000000000LL / clk);
    if (g_cpu_last_wall_ns == 0) {
        g_cpu_last_wall_ns = wall;
        g_cpu_last_proc_ns = proc;
        return -1;
    }
    int64_t dw = wall - g_cpu_last_wall_ns;
    int64_t dp = proc - g_cpu_last_proc_ns;
    g_cpu_last_wall_ns = wall;
    g_cpu_last_proc_ns = proc;
    if (dw <= 0) return 0;
    int64_t pct = (dp * 100) / (dw * cores);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return (int32_t)pct;
}

int64_t hao_proc_uptime_ms(void) {
    FILE* f = fopen("/proc/self/stat", "r");
    if (!f) return 0;
    unsigned long long start = 0;
    /* starttime 是第 22 个字段 */
    int n = fscanf(f,
        "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u "
        "%*u %*u %*d %*d %*d %*d %*d %*d %llu",
        &start);
    fclose(f);
    if (n != 1) return 0;
    long clk = sysconf(_SC_CLK_TCK);
    if (clk <= 0) clk = 100;
    double up = 0;
    FILE* uf = fopen("/proc/uptime", "r");
    if (uf) {
        if (fscanf(uf, "%lf", &up) != 1) up = 0;
        fclose(uf);
    }
    double started = (double)start / (double)clk;
    double alive = up - started;
    if (alive < 0) alive = 0;
    return (int64_t)(alive * 1000.0);
}

#endif
