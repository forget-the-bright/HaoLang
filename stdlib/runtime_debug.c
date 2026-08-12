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

/* P1/T1：调用帧 TLS；与 src TLS 同线程 */
#define HAO_DBG_STACK_MAX 32
typedef struct {
    const char* name;
    int32_t kind;
    int64_t raw;
} HaoDbgArg;

typedef struct {
    const char* file;      /* 函数入口 */
    int32_t line;
    int32_t col;
    const char* func;
    const char* pc_file;   /* 最近语句 */
    int32_t pc_line;
    int32_t pc_col;
    int nargs;
    HaoDbgArg args[HAO_DBG_ARG_MAX];
} HaoDbgFrame;

#ifdef _WIN32
static __declspec(thread) HaoDbgFrame g_dbg_stack[HAO_DBG_STACK_MAX];
static __declspec(thread) int g_dbg_sp = 0;
#else
static __thread HaoDbgFrame g_dbg_stack[HAO_DBG_STACK_MAX];
static __thread int g_dbg_sp = 0;
#endif

/* stdlib / native.hao = 库帧；排障时 stack 标 [lib] */
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

static int hao_dbg_is_native_hao(const char* file) {
    size_t n;
    const char* e;
    if (!file) return 0;
    n = strlen(file);
    if (n < 10) return 0;
    e = file + n - 10;
    return ((e[0] == 'n' || e[0] == 'N') &&
            (e[1] == 'a' || e[1] == 'A') &&
            (e[2] == 't' || e[2] == 'T') &&
            (e[3] == 'i' || e[3] == 'I') &&
            (e[4] == 'v' || e[4] == 'V') &&
            (e[5] == 'e' || e[5] == 'E') &&
            e[6] == '.' &&
            (e[7] == 'h' || e[7] == 'H') &&
            (e[8] == 'a' || e[8] == 'A') &&
            (e[9] == 'o' || e[9] == 'O'));
}

/* crash 路径禁 snprintf：手写十进制 / 十六进制 */
static int hao_dbg_put_u64(char* buf, int cap, int len, uint64_t u) {
    char tmp[24];
    int n = 0;
    if (len < 0) len = 0;
    if (len >= cap) return cap;
    do {
        tmp[n++] = (char)('0' + (u % 10));
        u /= 10;
    } while (u && n < (int)sizeof(tmp));
    if (len + n >= cap) n = cap - len - 1;
    if (n < 0) n = 0;
    for (int i = 0; i < n; ++i) buf[len + i] = tmp[n - 1 - i];
    len += n;
    if (len < cap) buf[len] = '\0';
    return len;
}

static int hao_dbg_put_i64(char* buf, int cap, int len, int64_t v) {
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
    return hao_dbg_put_u64(buf, cap, len, u);
}

static int hao_dbg_put_hex64(char* buf, int cap, int len, uint64_t u) {
    static const char* digs = "0123456789abcdef";
    char tmp[16];
    int n = 0;
    if (len < 0) len = 0;
    if (len + 2 >= cap) return len;
    buf[len++] = '0';
    buf[len++] = 'x';
    do {
        tmp[n++] = digs[u & 0xf];
        u >>= 4;
    } while (u && n < 16);
    while (n < 1) tmp[n++] = '0';
    if (len + n >= cap) n = cap - len - 1;
    for (int i = 0; i < n; ++i) buf[len + i] = tmp[n - 1 - i];
    len += n;
    if (len < cap) buf[len] = '\0';
    return len;
}

static int hao_dbg_put_str(char* buf, int cap, int len, const char* s) {
    if (!s) s = "?";
    while (*s && len < cap - 1) buf[len++] = *s++;
    if (len < cap) buf[len] = '\0';
    return len;
}

static int hao_dbg_put_pad2(char* buf, int cap, int len, int v) {
    if (v < 0) v = 0;
    if (v > 99) v = 99;
    if (len + 2 >= cap) return len;
    buf[len++] = (char)('0' + (v / 10));
    buf[len++] = (char)('0' + (v % 10));
    buf[len] = '\0';
    return len;
}

static int hao_dbg_put_pad3(char* buf, int cap, int len, int v) {
    if (v < 0) v = 0;
    if (v > 999) v = 999;
    if (len + 3 >= cap) return len;
    buf[len++] = (char)('0' + (v / 100));
    buf[len++] = (char)('0' + ((v / 10) % 10));
    buf[len++] = (char)('0' + (v % 10));
    buf[len] = '\0';
    return len;
}

static int hao_dbg_put_pad4(char* buf, int cap, int len, int v) {
    if (v < 0) v = 0;
    if (v > 9999) v = 9999;
    if (len + 4 >= cap) return len;
    buf[len++] = (char)('0' + (v / 1000));
    buf[len++] = (char)('0' + ((v / 100) % 10));
    buf[len++] = (char)('0' + ((v / 10) % 10));
    buf[len++] = (char)('0' + (v % 10));
    buf[len] = '\0';
    return len;
}

/* Howard Hinnant civil_from_days → y/m/d */
static void hao_dbg_civil_from_days(int64_t z, int* y, int* m, int* d) {
    int64_t era, doe, yoe, doy, mp, yy, mm, dd;
    z += 719468;
    if (z >= 0) era = z / 146097;
    else era = (z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    yy = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    dd = doy - (153 * mp + 2) / 5 + 1;
    if (mp < 10) mm = mp + 3;
    else mm = mp - 9;
    if (mm <= 2) yy = yy + 1;
    *y = (int)yy;
    *m = (int)mm;
    *d = (int)dd;
}

void hao_dbg_fprint_time(FILE* f) {
    char buf[64];
    int len = 0;
    int64_t ns, sec, rem, day, sod;
    int32_t off;
    int y, m, d, hh, mi, ss, ms, off_h, off_m;
    if (!f) return;
    ns = hao_time_now_ns();
    off = hao_time_offset();
    sec = ns / 1000000000LL + (int64_t)off;
    rem = ns % 1000000000LL;
    if (rem < 0) rem += 1000000000LL;
    ms = (int)(rem / 1000000LL);
    if (sec >= 0) day = sec / 86400;
    else day = (sec - 86399) / 86400;
    sod = sec % 86400;
    if (sod < 0) sod += 86400;
    hao_dbg_civil_from_days(day, &y, &m, &d);
    hh = (int)(sod / 3600);
    mi = (int)((sod % 3600) / 60);
    ss = (int)(sod % 60);
    len = hao_dbg_put_str(buf, (int)sizeof(buf), 0, "time=");
    len = hao_dbg_put_pad4(buf, (int)sizeof(buf), len, y);
    len = hao_dbg_put_str(buf, (int)sizeof(buf), len, "-");
    len = hao_dbg_put_pad2(buf, (int)sizeof(buf), len, m);
    len = hao_dbg_put_str(buf, (int)sizeof(buf), len, "-");
    len = hao_dbg_put_pad2(buf, (int)sizeof(buf), len, d);
    len = hao_dbg_put_str(buf, (int)sizeof(buf), len, " ");
    len = hao_dbg_put_pad2(buf, (int)sizeof(buf), len, hh);
    len = hao_dbg_put_str(buf, (int)sizeof(buf), len, ":");
    len = hao_dbg_put_pad2(buf, (int)sizeof(buf), len, mi);
    len = hao_dbg_put_str(buf, (int)sizeof(buf), len, ":");
    len = hao_dbg_put_pad2(buf, (int)sizeof(buf), len, ss);
    len = hao_dbg_put_str(buf, (int)sizeof(buf), len, ".");
    len = hao_dbg_put_pad3(buf, (int)sizeof(buf), len, ms);
    len = hao_dbg_put_str(buf, (int)sizeof(buf), len, " ");
    if (off >= 0) len = hao_dbg_put_str(buf, (int)sizeof(buf), len, "+");
    else {
        len = hao_dbg_put_str(buf, (int)sizeof(buf), len, "-");
        off = -off;
    }
    off_h = off / 3600;
    off_m = (off % 3600) / 60;
    len = hao_dbg_put_pad2(buf, (int)sizeof(buf), len, off_h);
    len = hao_dbg_put_pad2(buf, (int)sizeof(buf), len, off_m);
    buf[len < (int)sizeof(buf) ? len : (int)sizeof(buf) - 1] = '\0';
    fprintf(f, "%s\n", buf);
}

static void hao_dbg_fprint_args(FILE* f, const HaoDbgFrame* fr) {
    int i;
    fprintf(f, "(");
    for (i = 0; i < fr->nargs; ++i) {
        const HaoDbgArg* a = &fr->args[i];
        if (i) fprintf(f, ", ");
        fprintf(f, "%s=", a->name ? a->name : "?");
        if (a->kind == HAO_DBG_ARG_PTR)
            fprintf(f, "%p", (void*)(uintptr_t)a->raw);
        else if (a->kind == HAO_DBG_ARG_BOOL)
            fprintf(f, "%s", a->raw ? "true" : "false");
        else if (a->kind == HAO_DBG_ARG_F64)
            fprintf(f, "bits=%llx", (unsigned long long)a->raw);
        else
            fprintf(f, "%lld", (long long)a->raw);
    }
    fprintf(f, ")");
}

static void hao_dbg_frame_site(const HaoDbgFrame* fr,
                               const char** file, int32_t* line, int32_t* col) {
    if (fr->pc_file && fr->pc_line > 0) {
        *file = fr->pc_file;
        *line = fr->pc_line;
        *col = fr->pc_col;
    } else {
        *file = fr->file;
        *line = fr->line;
        *col = fr->col;
    }
}

void hao_dbg_set_src_loc(const char* file, int32_t line, int32_t col) {
    /* extern 声明文件末行 loc 曾误导（CU：native.hao:35）；忽略 */
    if (file && hao_dbg_is_native_hao(file))
        return;
    g_dbg_src_file = file;
    g_dbg_src_line = line;
    g_dbg_src_col = col;
    /* 同步栈顶帧 pc（语句位） */
    if (g_dbg_sp > 0) {
        HaoDbgFrame* fr = &g_dbg_stack[g_dbg_sp - 1];
        fr->pc_file = file;
        fr->pc_line = line;
        fr->pc_col = col;
    }
}

void hao_dbg_clear_src_loc(void) {
    g_dbg_src_file = NULL;
    g_dbg_src_line = 0;
    g_dbg_src_col = 0;
}

void hao_dbg_push_frame(const char* file, int32_t line, int32_t col,
                        const char* func) {
    if (g_dbg_sp < HAO_DBG_STACK_MAX) {
        HaoDbgFrame* fr = &g_dbg_stack[g_dbg_sp++];
        memset(fr, 0, sizeof(*fr));
        fr->file = file;
        fr->line = line;
        fr->col = col;
        fr->func = func;
        fr->pc_file = file;
        fr->pc_line = line;
        fr->pc_col = col;
    }
}

void hao_dbg_pop_frame(void) {
    if (g_dbg_sp > 0) --g_dbg_sp;
}

void hao_dbg_clear_frame_args(void) {
    if (g_dbg_sp <= 0) return;
    g_dbg_stack[g_dbg_sp - 1].nargs = 0;
}

void hao_dbg_add_frame_arg(const char* name, int32_t kind, int64_t raw) {
    HaoDbgFrame* fr;
    if (g_dbg_sp <= 0) return;
    fr = &g_dbg_stack[g_dbg_sp - 1];
    if (fr->nargs >= HAO_DBG_ARG_MAX) return;
    fr->args[fr->nargs].name = name;
    fr->args[fr->nargs].kind = kind;
    fr->args[fr->nargs].raw = raw;
    fr->nargs++;
}

static void hao_dbg_resolve_src(const char** file, int32_t* line, int32_t* col) {
    int i;
    *file = g_dbg_src_file;
    *line = g_dbg_src_line;
    *col = g_dbg_src_col;
    /* TLS 已漂到库帧时，改用 stack 上最近用户帧的 pc/入口（CU 读法） */
    if ((!*file || *line <= 0 || hao_dbg_path_is_lib(*file)) && g_dbg_sp > 0) {
        for (i = g_dbg_sp - 1; i >= 0; --i) {
            const HaoDbgFrame* fr = &g_dbg_stack[i];
            const char* ff = NULL;
            int32_t fl = 0, fc = 0;
            hao_dbg_frame_site(fr, &ff, &fl, &fc);
            if (ff && fl > 0 && !hao_dbg_path_is_lib(ff)) {
                *file = ff;
                *line = fl;
                *col = fc;
                break;
            }
        }
    }
}

void hao_dbg_fprint_where(FILE* f) {
    const HaoDbgFrame* fr;
    const char* file = NULL;
    int32_t line = 0, col = 0;
    if (!f) return;
    fprintf(f, "where=");
    if (g_dbg_sp <= 0) {
        hao_dbg_resolve_src(&file, &line, &col);
        fprintf(f, "? at ");
        if (file && line > 0)
            fprintf(f, "%s:%d:%d\n", file, (int)line, (int)col);
        else
            fprintf(f, "?\n");
        return;
    }
    fr = &g_dbg_stack[g_dbg_sp - 1];
    fprintf(f, "%s", fr->func ? fr->func : "?");
    hao_dbg_fprint_args(f, fr);
    fprintf(f, " at ");
    /* where 用真实 TLS（若有）否则帧 pc —— 不强制漂到用户入口 */
    if (g_dbg_src_file && g_dbg_src_line > 0 && !hao_dbg_is_native_hao(g_dbg_src_file)) {
        file = g_dbg_src_file;
        line = g_dbg_src_line;
        col = g_dbg_src_col;
    } else {
        hao_dbg_frame_site(fr, &file, &line, &col);
    }
    if (file && line > 0)
        fprintf(f, "%s:%d:%d\n", file, (int)line, (int)col);
    else
        fprintf(f, "?\n");
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
    fprintf(f, "hao_stack:\n");
    if (n <= 0) {
        fprintf(f, "  (empty)\n");
        return;
    }
    for (i = n - 1; i >= 0; --i) {
        const HaoDbgFrame* fr = &g_dbg_stack[i];
        const char* file = NULL;
        int32_t line = 0, col = 0;
        hao_dbg_frame_site(fr, &file, &line, &col);
        fprintf(f, "  #%d %s", n - 1 - i, fr->func ? fr->func : "?");
        hao_dbg_fprint_args(f, fr);
        fprintf(f, " at ");
        if (file && line > 0)
            fprintf(f, "%s:%d:%d%s\n", file, (int)line, (int)col,
                    hao_dbg_path_is_lib(file) ? " [lib]" : "");
        else
            fprintf(f, "?%s\n",
                    (fr->file && hao_dbg_path_is_lib(fr->file)) ? " [lib]" : "");
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
        hao_dbg_fprint_time(f);
        fprintf(f, "kind=%s msg=%s\n", kind ? kind : "?", msg ? msg : "(null)");
        hao_dbg_fprint_where(f);
        hao_dbg_fprint_src_loc(f);
        hao_dbg_fprint_stack(f);
        hao_gc_fprint_debug_snapshot(f);
        fprintf(f, "---- end ----\n");
        fflush(f);
        fclose(f);
    } else {
        hao_dbg_fprint_time(stderr);
        hao_dbg_fprint_where(stderr);
        hao_dbg_fprint_src_loc(stderr);
        hao_dbg_fprint_stack(stderr);
        hao_gc_fprint_debug_snapshot(stderr);
    }
    exit(1);
}
