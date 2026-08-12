/*
 * HaoLang 运行时 —— 自研浮点十进制（P7c）
 * 禁 libc strtof/strtod/snprintf(%g)；供 toStr / parse / reflect field。
 */
#include "runtime_internal.h"
#include <math.h>

static int hao_fmt_u64(char* out, int cap, int len, uint64_t u) {
    char tmp[24];
    int n = 0;
    if (len < 0) len = 0;
    if (len >= cap) return cap;
    do {
        tmp[n++] = (char)('0' + (u % 10ull));
        u /= 10ull;
    } while (u && n < (int)sizeof(tmp));
    if (len + n >= cap) n = cap - len - 1;
    if (n < 0) n = 0;
    for (int i = 0; i < n; ++i) out[len + i] = tmp[n - 1 - i];
    len += n;
    if (len < cap) out[len] = '\0';
    return len;
}

int hao_fmt_double(double v, char* out, int cap) {
    int len = 0;
    int neg;
    int exp10;
    double frac;
    uint64_t digs;
    int nd;
    if (cap < 4) return 0;
    if (v != v) {
        if (cap > 3) { out[0]='n'; out[1]='a'; out[2]='n'; out[3]='\0'; return 3; }
        return 0;
    }
    if (isinf(v)) {
        if (v < 0 && len + 1 < cap) out[len++] = '-';
        if (len + 3 < cap) { out[len++]='i'; out[len++]='n'; out[len++]='f'; out[len]='\0'; }
        return len;
    }
    neg = (v < 0.0) || (v == 0.0 && signbit(v));
    if (neg) v = -v;
    if (neg && len + 1 < cap) out[len++] = '-';
    if (v == 0.0) {
        if (len + 1 < cap) { out[len++] = '0'; out[len] = '\0'; }
        return len;
    }
    exp10 = 0;
    while (v >= 10.0 && exp10 < 308) { v /= 10.0; exp10++; }
    while (v < 1.0 && exp10 > -308) { v *= 10.0; exp10--; }
    frac = v;
    for (nd = 0; nd < 14; ++nd) frac *= 10.0;
    digs = (uint64_t)(frac + 0.5);
    if (digs >= 1000000000000000ull) {
        digs = 100000000000000ull;
        exp10++;
    }
    nd = 15;
    while (nd > 1 && (digs % 10ull) == 0) { digs /= 10ull; nd--; }
    if (exp10 >= -4 && exp10 < 15) {
        int whole_digits = exp10 + 1;
        if (whole_digits <= 0) {
            if (len + 2 < cap) { out[len++] = '0'; out[len++] = '.'; }
            for (int z = 0; z < -whole_digits && len + 1 < cap; ++z) out[len++] = '0';
            len = hao_fmt_u64(out, cap, len, digs);
        } else if (whole_digits >= nd) {
            len = hao_fmt_u64(out, cap, len, digs);
            for (int z = 0; z < whole_digits - nd && len + 1 < cap; ++z) out[len++] = '0';
        } else {
            uint64_t pow = 1;
            for (int i = 0; i < nd - whole_digits; ++i) pow *= 10ull;
            uint64_t wi = digs / pow;
            uint64_t fr = digs % pow;
            len = hao_fmt_u64(out, cap, len, wi);
            if (fr && len + 1 < cap) {
                out[len++] = '.';
                uint64_t p2 = pow / 10ull;
                while (p2 > fr && p2 > 0 && len + 1 < cap) {
                    out[len++] = '0';
                    p2 /= 10ull;
                }
                len = hao_fmt_u64(out, cap, len, fr);
            }
        }
    } else {
        uint64_t pow = 1;
        for (int i = 0; i < nd - 1; ++i) pow *= 10ull;
        uint64_t lead = digs / pow;
        uint64_t rest = digs % pow;
        len = hao_fmt_u64(out, cap, len, lead);
        if (rest && len + 1 < cap) {
            out[len++] = '.';
            len = hao_fmt_u64(out, cap, len, rest);
        }
        if (len + 1 < cap) out[len++] = 'e';
        if (exp10 < 0) {
            if (len + 1 < cap) out[len++] = '-';
            exp10 = -exp10;
        } else if (len + 1 < cap) {
            out[len++] = '+';
        }
        len = hao_fmt_u64(out, cap, len, (uint64_t)exp10);
    }
    if (len < cap) out[len] = '\0';
    return len;
}

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

HaoString* hao_float_to_str(float v) {
    char buf[96];
    int n = hao_fmt_double((double)v, buf, (int)sizeof(buf));
    if (n < 0) n = 0;
    return hao_str_from_bytes(buf, n);
}

HaoString* hao_double_to_str(double v) {
    char buf[96];
    int n = hao_fmt_double(v, buf, (int)sizeof(buf));
    if (n < 0) n = 0;
    return hao_str_from_bytes(buf, n);
}

void* hao_parse_double(HaoString* s) {
    char* p = hao_ffi_dup_cstr(s);
    const char* end = NULL;
    double v;
    if (!p || !*p) { free(p); return NULL; }
    if (hao_parse_double_cstr(p, &end, &v) != 0) {
        free(p);
        return NULL;
    }
    free(p);
    return hao_box_f64(v);
}

void* hao_parse_float(HaoString* s) {
    char* p = hao_ffi_dup_cstr(s);
    const char* end = NULL;
    double v;
    if (!p || !*p) { free(p); return NULL; }
    if (hao_parse_double_cstr(p, &end, &v) != 0) {
        free(p);
        return NULL;
    }
    free(p);
    return hao_box_f32((float)v);
}
