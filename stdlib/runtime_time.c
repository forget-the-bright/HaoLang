/*
 * HaoLang 运行时 —— 时间（v0.19.0）
 * ------------------------------------------------------------
 *  时间戳为自 Unix 纪元（1970-01-01T00:00:00Z）起的纳秒数（i64）。
 *  时区以「相对 UTC 的秒偏移」表示：本地时区自动取系统时区，UTC 偏移 0。
 *  字段与格式化统一按「秒 + 偏移」换算成 UTC 时间再取，保证跨平台一致。
 *
 *  跨平台：
 *    - 当前纳秒：Windows GetSystemTimePreciseAsFileTime，Linux clock_gettime；
 *    - 本地时区位移：localtime/gmtime 差值；
 *    - 按偏移取字段/格式化：gmtime 处理 (秒+偏移)。
 *  MSVC 无 localtime_r/gmtime_r，用 _localtime64_s/_gmtime64_s。
 */
#include "runtime_internal.h"

#include <time.h>

#ifdef _WIN32
#ifdef _MSC_VER
#define HLT_LOCALTIME(t, out)  _localtime64_s((out), (t))
#define HLT_GMTIME(t, out)     _gmtime64_s((out), (t))
#else
#define HLT_LOCALTIME(t, out)  localtime_r((t), (out))
#define HLT_GMTIME(t, out)     gmtime_r((t), (out))
#endif
#else
#define HLT_LOCALTIME(t, out)  localtime_r((t), (out))
#define HLT_GMTIME(t, out)     gmtime_r((t), (out))
#endif

/* 当前 Unix 时间戳（纳秒） */
int64_t hao_time_now_ns(void) {
#ifdef _WIN32
    return hao_win_now_ns();
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif
}

/* 本地时区相对 UTC 的偏移（秒） */
int32_t hao_time_offset(void) {
    time_t t = time(NULL);
    struct tm l, g;
    if (HLT_LOCALTIME(&t, &l) != 0) return 0;
    if (HLT_GMTIME(&t, &g) != 0) return 0;
    return (int32_t)(mktime(&g) - mktime(&l));
}

/* 按 (纳秒时间戳 + 时区偏移秒) 取字段。
 * field: 0年 1月 2日 3时 4分 5秒 6周几(0=周日) 7一年第几天 */
int32_t hao_time_field(int64_t ts, int32_t offset, int32_t field) {
    time_t sec = (time_t)(ts / 1000000000LL + offset);
    struct tm g;
    if (HLT_GMTIME(&sec, &g) != 0) return 0;
    switch (field) {
        case 0: return g.tm_year + 1900;
        case 1: return g.tm_mon + 1;
        case 2: return g.tm_mday;
        case 3: return g.tm_hour;
        case 4: return g.tm_min;
        case 5: return g.tm_sec;
        case 6: return g.tm_wday;
        case 7: return g.tm_yday + 1;
    }
    return 0;
}

/* 按自定义格式字符串格式化时间。
 * 支持占位符：yyyy(年4位) MM(月2位) dd(日2位) HH(时2位) mm(分) ss(秒)
 *           SSS(毫秒3位)；其余字符原样输出。 */
HaoString* hao_time_format(int64_t ts, int32_t offset, HaoString* pattern) {
    time_t sec = (time_t)(ts / 1000000000LL + offset);
    struct tm g;
    if (HLT_GMTIME(&sec, &g) != 0) return NULL;
    int64_t ms = (ts % 1000000000LL + 1000000000LL) % 1000000000LL / 1000000;

    char out[512];
    size_t o = 0;
    const char* p = pattern ? pattern->data : "";
    while (*p && o < sizeof out - 1) {
        const char* q = p;
        /* 匹配占位符 token */
        size_t tok = 0;
        while (q[tok] && q[tok] == p[0]) tok++;
        if (tok >= 2 && p[0] == 'y' && tok >= 4 && q[0] == 'y') {
            o += (size_t)snprintf(out + o, sizeof out - o, "%04d", g.tm_year + 1900);
            p += 4;
        } else if (tok >= 2 && p[0] == 'M') {
            o += (size_t)snprintf(out + o, sizeof out - o, "%02d", g.tm_mon + 1);
            p += 2;
        } else if (tok >= 2 && p[0] == 'd') {
            o += (size_t)snprintf(out + o, sizeof out - o, "%02d", g.tm_mday);
            p += 2;
        } else if (tok >= 2 && p[0] == 'H') {
            o += (size_t)snprintf(out + o, sizeof out - o, "%02d", g.tm_hour);
            p += 2;
        } else if (tok >= 2 && p[0] == 'm') {
            o += (size_t)snprintf(out + o, sizeof out - o, "%02d", g.tm_min);
            p += 2;
        } else if (tok >= 2 && p[0] == 's') {
            o += (size_t)snprintf(out + o, sizeof out - o, "%02d", g.tm_sec);
            p += 2;
        } else if (tok >= 3 && p[0] == 'S') {
            o += (size_t)snprintf(out + o, sizeof out - o, "%03lld",
                                  (long long)ms);
            p += 3;
        } else {
            out[o++] = *p;
            p++;
        }
    }
    out[o] = 0;
    return hao_str_from_bytes(out, (int32_t)o);
}