/*
 * HaoLang 运行时 —— panic / 运行时错误终止
 */
#include "runtime_internal.h"

/* 空安全断言 !! 失败时调用 */
void hao_panic_null(void) {
    fputs("panic: 对 null 值执行了非空断言 (!!)\n", stderr);
    exit(1);
}

/* 整数除零 */
void hao_panic_div_zero(void) {
    fputs("panic: 整数除以零\n", stderr);
    exit(1);
}

/* 数组越界 */
void hao_panic_index(int64_t idx, int64_t len) {
    fprintf(stderr, "panic: 索引越界: 索引 %lld 超出长度 %lld\n",
            (long long)idx, (long long)len);
    exit(1);
}

/* 有符号长度/算术溢出（如数组展开总长回绕） */
void hao_panic_overflow(void) {
    fputs("panic: 整数溢出\n", stderr);
    exit(1);
}

/* as 转换失败时调用 */
void hao_panic_cast(const char* target) {
    fprintf(stderr, "panic: 类型转换失败，对象不是 %s 类型\n",
            target ? target : "目标");
    exit(1);
}

/* 通用运行时 panic（反射 arity 等防御路径） */
void hao_panic_msg(const char* msg) {
    fprintf(stderr, "panic: %s\n", msg ? msg : "(null)");
    exit(1);
}
