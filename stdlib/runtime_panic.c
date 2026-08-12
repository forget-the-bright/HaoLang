/*
 * HaoLang 运行时 —— panic / 运行时错误终止
 */
#include "runtime_internal.h"

/* 空安全断言 !! 失败时调用 */
void hao_panic_null(void) {
    hao_report_fatal("panic", "对 null 值执行了非空断言 (!!)");
}

/* 整数除零 */
void hao_panic_div_zero(void) {
    hao_report_fatal("panic", "整数除以零");
}

/* 数组越界 */
void hao_panic_index(int64_t idx, int64_t len) {
    char buf[160];
    int n = 0;
    const char* a = "索引越界: 索引 ";
    const char* b = " 超出长度 ";
    while (*a && n < (int)sizeof(buf) - 1) buf[n++] = *a++;
    n = n + hao_fmt_i64_dec(buf + n, (int)sizeof(buf) - n, idx);
    while (*b && n < (int)sizeof(buf) - 1) buf[n++] = *b++;
    n = n + hao_fmt_i64_dec(buf + n, (int)sizeof(buf) - n, len);
    if (n < (int)sizeof(buf)) buf[n] = '\0';
    hao_report_fatal("panic", buf);
}

/* 有符号长度/算术溢出（如数组展开总长回绕） */
void hao_panic_overflow(void) {
    hao_report_fatal("panic", "整数溢出");
}

/* as 转换失败时调用 */
void hao_panic_cast(const char* target) {
    char buf[192];
    int n = 0;
    const char* a = "类型转换失败，对象不是 ";
    const char* b = " 类型";
    const char* t = target ? target : "目标";
    while (*a && n < (int)sizeof(buf) - 1) buf[n++] = *a++;
    while (*t && n < (int)sizeof(buf) - 1) buf[n++] = *t++;
    while (*b && n < (int)sizeof(buf) - 1) buf[n++] = *b++;
    buf[n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1] = '\0';
    hao_report_fatal("panic", buf);
}

/* 通用运行时 panic（反射 arity 等防御路径） */
void hao_panic_msg(const char* msg) {
    hao_report_fatal("panic", msg ? msg : "(null)");
}
