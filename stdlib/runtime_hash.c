/*
 * HaoLang 运行时 —— 哈希（浮点位模式；其余已上移 hash.Hash.hao）
 */
#include "runtime_internal.h"

#include <stdint.h>

static uint64_t mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

/* Double key：位模式；0.0 与 -0.0 值相等，统一为 0 */
int32_t hao_hash_float(double d) {
    if (d == 0.0) return 0;
    uint64_t bits;
    memcpy(&bits, &d, sizeof bits);
    return (int32_t)mix64(bits);
}

/* Float key（f32） */
int32_t hao_hash_f32(float f) {
    if (f == 0.0f) return 0;
    uint32_t bits;
    memcpy(&bits, &f, sizeof bits);
    return (int32_t)mix64((uint64_t)bits);
}
