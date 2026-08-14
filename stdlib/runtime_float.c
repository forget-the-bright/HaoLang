/*
 * HaoLang 运行时 —— 自研浮点解析（P7c）
 * 禁 libc strtof/strtod；toStr 业务已上移 Hao。
 */
#include "runtime_internal.h"
#include <math.h>

int hao_parse_double_cstr(const char* p, const char** endp, double* out) {
    const char* s = p;
    int neg = 0;
    double v = 0.0;
    double frac = 1.0;
    int seen = 0;
    int exp = 0;
    int eneg = 0;
    if (!s || !*s) { if (endp) *endp = s; return -1; }
    if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }
    if ((s[0] == 'i' || s[0] == 'I') && (s[1] == 'n' || s[1] == 'N') &&
        (s[2] == 'f' || s[2] == 'F')) {
        s += 3;
        *out = neg ? -INFINITY : INFINITY;
        if (endp) *endp = s;
        return (*s == '\0') ? 0 : -1;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10.0 + (double)(*s - '0');
        s++;
        seen = 1;
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            frac *= 0.1;
            v += frac * (double)(*s - '0');
            s++;
            seen = 1;
        }
    }
    if (!seen) { if (endp) *endp = p; return -1; }
    if (*s == 'e' || *s == 'E') {
        s++;
        if (*s == '+' || *s == '-') { eneg = (*s == '-'); s++; }
        if (!(*s >= '0' && *s <= '9')) { if (endp) *endp = p; return -1; }
        while (*s >= '0' && *s <= '9') {
            exp = exp * 10 + (*s - '0');
            s++;
        }
        if (eneg) exp = -exp;
        while (exp > 0) { v *= 10.0; exp--; }
        while (exp < 0) { v *= 0.1; exp++; }
    }
    if (endp) *endp = s;
    if (*s != '\0') return -1;
    *out = neg ? -v : v;
    return 0;
}

/* v0.79+：浮点 toStr/fmt 业务已上移 Hao；本文件仅 parse_cstr */
