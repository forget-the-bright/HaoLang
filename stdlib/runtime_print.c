/*
 * HaoLang 运行时 —— 输出（fmt 包底层）
 * ------------------------------------------------------------
 *  Windows 控制台 UTF-8 初始化也放在本文件：任何有输出的程序都会
 *  引用 hao_println_* / hao_print_str，从而把本目标文件链入，附带把
 *  .CRT$XIU 初始化器链入。若单独放在一个不导出 hao_* 符号的文件里，
 *  静态库链接时会被整体丢弃，导致中文乱码。
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

/* fmt.println(String) — HaoString* */
void hao_println_str(HaoString* s) {
    fputs(s ? s->data : "null", stdout);
    fputc('\n', stdout);
}

void hao_println_sbyte(int8_t v) {
    printf("%d\n", (int)v);
}

void hao_println_byte(uint8_t v) {
    printf("%u\n", (unsigned)v);
}

void hao_println_short(int16_t v) {
    printf("%d\n", (int)v);
}

void hao_println_ushort(uint16_t v) {
    printf("%u\n", (unsigned)v);
}

void hao_println_int(int32_t v) {
    printf("%d\n", (int)v);
}

void hao_println_uint(int32_t v) {
    printf("%u\n", (unsigned)(uint32_t)v);
}

void hao_println_long(int64_t v) {
    printf("%lld\n", (long long)v);
}

void hao_println_ulong(uint64_t v) {
    printf("%llu\n", (unsigned long long)v);
}

void hao_println_float(float v) {
    printf("%g\n", (double)v);
}

void hao_println_double(double v) {
    printf("%g\n", v);
}

void hao_println_bool(int8_t v) {
    fputs(v ? "true" : "false", stdout);
    fputc('\n', stdout);
}

/* fmt.println(Char) — 打印 UTF-8 字形 */
void hao_println_char(int32_t cp) {
    HaoString* s = hao_char_to_str(cp);
    fputs(s ? s->data : "?", stdout);
    fputc('\n', stdout);
}

void hao_print_str(HaoString* s) {
    fputs(s ? s->data : "null", stdout);
}
