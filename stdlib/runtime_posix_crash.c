/*
 * HaoLang 运行时 —— POSIX 崩溃钩子（R4L）
 * ------------------------------------------------------------
 *  对等 Win UEF：SIGSEGV/SIGABRT/SIGBUS/SIGFPE → stderr + hao-crash.log
 *  + GC 快照 + 最近源码位。仅非 Windows 编译。
 */
#ifndef _WIN32

#include "runtime_internal.h"

#include <signal.h>
#include <string.h>
#include <unistd.h>

static void hao_posix_crash_handler(int sig) {
    const char* name = "?";
    FILE* f;
    switch (sig) {
        case SIGSEGV: name = "SIGSEGV"; break;
        case SIGABRT: name = "SIGABRT"; break;
#ifdef SIGBUS
        case SIGBUS: name = "SIGBUS"; break;
#endif
        case SIGFPE: name = "SIGFPE"; break;
        case SIGILL: name = "SIGILL"; break;
        default: break;
    }
    fprintf(stderr, "\nFATAL: 信号 %s(%d)（详见 hao-crash.log）\n", name, sig);
    fflush(stderr);
    f = fopen("hao-crash.log", "a");
    if (f) {
        fprintf(f, "---- crash ----\n");
        fprintf(f, "signal=%s(%d)\n", name, sig);
        hao_dbg_fprint_src_loc(f);
        hao_dbg_fprint_stack(f);
        hao_gc_fprint_debug_snapshot(f);
        fprintf(f, "---- end ----\n");
        fflush(f);
        fclose(f);
    }
    _exit(128 + (sig & 127));
}

__attribute__((constructor))
static void hao_posix_crash_install(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = hao_posix_crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
#ifdef SIGBUS
    sigaction(SIGBUS, &sa, NULL);
#endif
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
}

#endif /* !_WIN32 */
