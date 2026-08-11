/*
 * HaoLang 运行时 —— Debug 收口（Trace / Assert / Loc / Stack）
 */
#include "runtime_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define hao_raw_write(fd, p, n) _write((fd), (p), (unsigned)(n))
#else
#include <unistd.h>
#define hao_raw_write(fd, p, n) write((fd), (p), (n))
#endif

static int hao_env_truthy(const char* v) {
    if (!v || !*v) return 0;
    if (v[0] == '0' && v[1] == '\0') return 0;
    if ((v[0] == 'n' || v[0] == 'N') &&
        (v[1] == 'o' || v[1] == 'O') && v[2] == '\0')
        return 0;
    if ((v[0] == 'f' || v[0] == 'F') &&
        (v[1] == 'a' || v[1] == 'A') &&
        (v[2] == 'l' || v[2] == 'L') &&
        (v[3] == 's' || v[3] == 'S') &&
        (v[4] == 'e' || v[4] == 'E') && v[5] == '\0')
        return 0;
    return 1;
}

static int hao_trace_enabled(const char* module) {
    const char* all = getenv("HAO_TRACE");
    if (hao_env_truthy(all)) return 1;
    if (module && strncmp(module, "gc", 2) == 0 &&
        (module[2] == '\0' || module[2] == '.' || module[2] == '/')) {
        return hao_env_truthy(getenv("HAO_GC_TRACE"));
    }
    return 0;
}

void hao_trace(const char* module, const char* fmt, ...) {
    char buf[512];
    int n;
    if (!hao_trace_enabled(module)) return;
    /*
     * Win64：Hao 子线程栈可能未 16B 对齐，libcmt snprintf/movdqa → AV（av_addr=-1）。
     * TRACE 一律手工拼前缀+字面 fmt，不进 CRT（格式符暂不展开，保诊断可用）。
     */
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    {
        const char* mod = module ? module : "?";
        const char* msg = fmt ? fmt : "";
        n = 0;
        const char* pfx = "[hao:";
        while (*pfx && n < (int)sizeof(buf) - 1) buf[n++] = *pfx++;
        while (*mod && n < (int)sizeof(buf) - 1) buf[n++] = *mod++;
        if (n < (int)sizeof(buf) - 1) buf[n++] = ']';
        if (n < (int)sizeof(buf) - 1) buf[n++] = ' ';
        while (*msg && n < (int)sizeof(buf) - 1) buf[n++] = *msg++;
        if (n < (int)sizeof(buf) - 1) buf[n++] = '\n';
        buf[n] = '\0';
        (void)hao_raw_write(2, buf, (unsigned)n);
        return;
    }
#else
    {
        va_list ap;
        n = snprintf(buf, sizeof(buf), "[hao:%s] ", module ? module : "?");
        if (n < 0) n = 0;
        if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
        {
            int m;
            va_start(ap, fmt);
            m = vsnprintf(buf + n, sizeof(buf) - (size_t)n, fmt ? fmt : "", ap);
            va_end(ap);
            if (m > 0) {
                n += m;
                if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
            }
        }
        if (n < (int)sizeof(buf) - 1) {
            buf[n++] = '\n';
            buf[n] = '\0';
        } else {
            buf[sizeof(buf) - 2] = '\n';
            buf[sizeof(buf) - 1] = '\0';
            n = (int)sizeof(buf) - 1;
        }
        (void)hao_raw_write(2, buf, (unsigned)n);
    }
#endif
}

void hao_assert_fail(const char* expr, const char* file, int line) {
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "hao_assert: %s (%s:%d)\n",
                     expr ? expr : "?", file ? file : "?", line);
    if (n > 0) {
        if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
        (void)hao_raw_write(2, buf, (unsigned)n);
    }
    abort();
}

#ifdef _WIN32
static __declspec(thread) const char* g_dbg_src_file;
static __declspec(thread) int32_t g_dbg_src_line;
static __declspec(thread) int32_t g_dbg_src_col;
#else
static __thread const char* g_dbg_src_file;
static __thread int32_t g_dbg_src_line;
static __thread int32_t g_dbg_src_col;
#endif

/* P1/T1：调用帧小数组 TLS（~512B）；与 src TLS 同线程，禁大块 TLS */
#define HAO_DBG_STACK_MAX 32
typedef struct {
    const char* file;
    int32_t line;
    int32_t col;
} HaoDbgFrame;

#ifdef _WIN32
static __declspec(thread) HaoDbgFrame g_dbg_stack[HAO_DBG_STACK_MAX];
static __declspec(thread) int g_dbg_sp = 0;
#else
static __thread HaoDbgFrame g_dbg_stack[HAO_DBG_STACK_MAX];
static __thread int g_dbg_sp = 0;
#endif

/* stdlib / native.hao = 库帧；排障时 stack 标 [lib]，src= 优先用户帧 */
static int hao_dbg_path_is_lib(const char* file) {
    const char* p;
    if (!file || !*file) return 0;
    for (p = file; *p; ++p) {
        if ((p[0] == 's' || p[0] == 'S') &&
            (p[1] == 't' || p[1] == 'T') &&
            (p[2] == 'd' || p[2] == 'D') &&
            (p[3] == 'l' || p[3] == 'L') &&
            (p[4] == 'i' || p[4] == 'I') &&
            (p[5] == 'b' || p[5] == 'B') &&
            (p[6] == '/' || p[6] == '\\'))
            return 1;
    }
    p = file;
    while (*p) ++p;
    if (p - file >= 10) {
        const char* e = p - 10; /* "native.hao" */
        if ((e[0] == 'n' || e[0] == 'N') &&
            (e[1] == 'a' || e[1] == 'A') &&
            (e[2] == 't' || e[2] == 'T') &&
            (e[3] == 'i' || e[3] == 'I') &&
            (e[4] == 'v' || e[4] == 'V') &&
            (e[5] == 'e' || e[5] == 'E') &&
            e[6] == '.' &&
            (e[7] == 'h' || e[7] == 'H') &&
            (e[8] == 'a' || e[8] == 'A') &&
            (e[9] == 'o' || e[9] == 'O'))
            return 1;
    }
    return 0;
}

void hao_dbg_set_src_loc(const char* file, int32_t line, int32_t col) {
    /* extern 声明文件末行 loc 曾误导（CU：native.hao:35）；忽略 */
    if (file && hao_dbg_path_is_lib(file)) {
        size_t n = strlen(file);
        if (n >= 10) {
            const char* e = file + n - 10;
            if ((e[0] == 'n' || e[0] == 'N') &&
                (e[1] == 'a' || e[1] == 'A') &&
                (e[2] == 't' || e[2] == 'T') &&
                (e[3] == 'i' || e[3] == 'I') &&
                (e[4] == 'v' || e[4] == 'V') &&
                (e[5] == 'e' || e[5] == 'E') &&
                e[6] == '.' &&
                (e[7] == 'h' || e[7] == 'H') &&
                (e[8] == 'a' || e[8] == 'A') &&
                (e[9] == 'o' || e[9] == 'O'))
                return;
        }
    }
    g_dbg_src_file = file;
    g_dbg_src_line = line;
    g_dbg_src_col = col;
    /* 不改写 stack 帧：帧=函数入口；src=当前语句/调用点 */
}

void hao_dbg_clear_src_loc(void) {
    g_dbg_src_file = NULL;
    g_dbg_src_line = 0;
    g_dbg_src_col = 0;
}

void hao_dbg_push_frame(const char* file, int32_t line, int32_t col) {
    if (g_dbg_sp < HAO_DBG_STACK_MAX) {
        HaoDbgFrame* fr = &g_dbg_stack[g_dbg_sp++];
        fr->file = file;
        fr->line = line;
        fr->col = col;
    }
    /* 不覆盖 TLS src：保留调用点，直至 callee 语句再更新 */
}

void hao_dbg_pop_frame(void) {
    if (g_dbg_sp > 0) --g_dbg_sp;
    /* src 保持至下一 set_src_loc / clear；不强制回到入口行 */
}

static void hao_dbg_resolve_src(const char** file, int32_t* line, int32_t* col) {
    int i;
    *file = g_dbg_src_file;
    *line = g_dbg_src_line;
    *col = g_dbg_src_col;
    /* TLS 已漂到库帧时，改用 stack 上最近用户帧（CU 读法） */
    if ((!*file || *line <= 0 || hao_dbg_path_is_lib(*file)) && g_dbg_sp > 0) {
        for (i = g_dbg_sp - 1; i >= 0; --i) {
            const HaoDbgFrame* fr = &g_dbg_stack[i];
            if (fr->file && fr->line > 0 && !hao_dbg_path_is_lib(fr->file)) {
                *file = fr->file;
                *line = fr->line;
                *col = fr->col;
                break;
            }
        }
    }
}

void hao_dbg_fprint_src_loc(FILE* f) {
    const char* file;
    int32_t line, col;
    if (!f) return;
    hao_dbg_resolve_src(&file, &line, &col);
    if (file && line > 0)
        fprintf(f, "src=%s:%d:%d\n", file, (int)line, (int)col);
    else
        fprintf(f, "src=?\n");
}

void hao_dbg_fprint_stack(FILE* f) {
    int i, n;
    if (!f) return;
    n = g_dbg_sp;
    fprintf(f, "stack:\n");
    if (n <= 0) {
        fprintf(f, "  (empty)\n");
        return;
    }
    for (i = n - 1; i >= 0; --i) {
        const HaoDbgFrame* fr = &g_dbg_stack[i];
        if (fr->file && fr->line > 0)
            fprintf(f, "  #%d %s:%d:%d%s\n", n - 1 - i, fr->file,
                    (int)fr->line, (int)fr->col,
                    hao_dbg_path_is_lib(fr->file) ? " [lib]" : "");
        else
            fprintf(f, "  #%d ?\n", n - 1 - i);
    }
}

void hao_debug_trap_av(void) {
    volatile int* p = (volatile int*)0;
    *p = 1; /* Win AV / POSIX SIGSEGV；loc_smoke 专用 */
}

void hao_report_fatal(const char* kind, const char* msg) {
    FILE* f;
    const char* file;
    int32_t line, col;
    fprintf(stderr, "FATAL [%s]: %s\n", kind ? kind : "?", msg ? msg : "(null)");
    hao_dbg_resolve_src(&file, &line, &col);
    if (file && line > 0)
        fprintf(stderr, "  at %s:%d:%d\n", file, (int)line, (int)col);
    fflush(stderr);
    f = fopen("hao-crash.log", "a");
    if (f) {
        fprintf(f, "---- fatal ----\n");
        fprintf(f, "kind=%s msg=%s\n", kind ? kind : "?", msg ? msg : "(null)");
        hao_dbg_fprint_src_loc(f);
        hao_dbg_fprint_stack(f);
        hao_gc_fprint_debug_snapshot(f);
        fprintf(f, "---- end ----\n");
        fflush(f);
        fclose(f);
    } else {
        hao_dbg_fprint_src_loc(stderr);
        hao_dbg_fprint_stack(stderr);
        hao_gc_fprint_debug_snapshot(stderr);
    }
    exit(1);
}
