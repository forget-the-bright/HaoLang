/*
 * HaoLang 运行时 —— 时间（v0.56）
 * ------------------------------------------------------------
 *  时间戳为自 Unix 纪元起的纳秒数（i64）。
 *  仅保留时钟底座：now_ns / 本地时区偏移。
 *  民用历字段与 format 已上移 Hao（time.hao）。
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

/* 本地时区相对 UTC 的偏移（秒，东向为正；北京 ≈ +28800） */
int32_t hao_time_offset(void) {
    time_t t = time(NULL);
    struct tm g;
    time_t as_local;
    if (HLT_GMTIME(&t, &g) != 0) return 0;
    g.tm_isdst = 0;
    /* 把 UTC 民用字段当成「本地」喂 mktime，差即东向偏移 */
    as_local = mktime(&g);
    if (as_local == (time_t)-1) return 0;
    return (int32_t)((int64_t)t - (int64_t)as_local);
}
