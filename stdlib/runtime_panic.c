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
    snprintf(buf, sizeof(buf), "索引越界: 索引 %lld 超出长度 %lld",
             (long long)idx, (long long)len);
    hao_report_fatal("panic", buf);
}

/* 有符号长度/算术溢出（如数组展开总长回绕） */
void hao_panic_overflow(void) {
    hao_report_fatal("panic", "整数溢出");
}

/* as 转换失败时调用 */
void hao_panic_cast(const char* target) {
    char buf[192];
    snprintf(buf, sizeof(buf), "类型转换失败，对象不是 %s 类型",
             target ? target : "目标");
    hao_report_fatal("panic", buf);
}

/* 通用运行时 panic（反射 arity 等防御路径） */
void hao_panic_msg(const char* msg) {
    hao_report_fatal("panic", msg ? msg : "(null)");
}
