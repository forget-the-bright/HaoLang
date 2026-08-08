/*
 * HaoLang 运行时 —— 哈希：Map/Set 的 key 哈希函数
 * ------------------------------------------------------------
 *  由编译器 extern 声明（hash 包）调用，供开放寻址哈希表散列。
 *  值类型按值哈希、String 按内容（FNV-1a）、对象按指针地址（身份哈希）。
 */
#include "runtime_internal.h"

#include <stdint.h>

/* 64 位整数的强度混合（splitmix64 风格），避免连续 key 聚集 */
static uint64_t mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

/* Int key（i32） */
int32_t hao_hash_int(int32_t x) {
    return (int32_t)mix64((uint64_t)(int64_t)x);
}

/* Long key（i64） */
int32_t hao_hash_long(int64_t x) {
    return (int32_t)mix64((uint64_t)x);
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

/* Bool key：固定两个质数 */
int32_t hao_hash_bool(int8_t b) {
    return b ? 1231 : 1237;
}

/* String key：内容 FNV-1a（按字节长度，不依赖嵌入 NUL） */
int32_t hao_hash_str(HaoString* s) {
    if (!s) return 0;
    uint64_t h = 14695981039346656037ULL;
    for (int32_t i = 0; i < s->len; ++i) {
        h ^= (unsigned char)s->data[i];
        h *= 1099511628211ULL;
    }
    return (int32_t)h;
}

/* 对象 key：指针地址（身份哈希） */
int32_t hao_hash_ptr(const void* p) {
    return (int32_t)mix64((uint64_t)(uintptr_t)p);
}