/*
 * HaoLang 运行时 —— 输出（fmt 包底层）
 * ------------------------------------------------------------
 *  Windows 控制台 UTF-8 初始化也放在本文件：任何有输出的程序都会
 *  引用 hao_println_str / hao_print_str，从而把本目标文件链入，附带把
 *  .CRT$XIU 初始化器链入。标量格式化已上移 Hao（P7a）。
 */
#include "runtime_internal.h"

#ifdef _WIN32
static uint32_t g_oldOutputCP = 0;

static void hao_console_restore(void) {
    if (g_oldOutputCP) hao_win_set_console_output_cp(g_oldOutputCP);
}

static int hao_console_init(void) {
    gc_init();   /* 在用户 main 之前初始化 GC 栈基址 */
    g_oldOutputCP = hao_win_get_console_output_cp();
    hao_win_set_console_output_cp(HAO_WIN_CP_UTF8);
    hao_win_set_console_cp(HAO_WIN_CP_UTF8);
    atexit(hao_console_restore);
    setvbuf(stdout, NULL, _IONBF, 0);
    return 0;
}

/* 注册到 CRT 初始化表：XIU 段在用户 main 之前被调用 */
#pragma section(".CRT$XIU", long, read)
__declspec(allocate(".CRT$XIU"))
static int (*hao_console_init_ptr)(void) = hao_console_init;
#endif /* _WIN32 */

/* fmt.println(String) — 出桥拷贝后再 fputs（禁借 GC 堆内 cstr） */
void hao_println_str(HaoString* s) {
    char* tmp;
    if (!s) {
        fputs("null\n", stdout);
        return;
    }
    tmp = hao_ffi_dup_cstr(s);
    fputs(tmp ? tmp : "null", stdout);
    fputc('\n', stdout);
    free(tmp);
}

void hao_print_str(HaoString* s) {
    char* tmp;
    if (!s) {
        fputs("null", stdout);
        return;
    }
    tmp = hao_ffi_dup_cstr(s);
    fputs(tmp ? tmp : "null", stdout);
    free(tmp);
}
