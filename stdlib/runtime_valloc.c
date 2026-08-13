/*
 * GC-MSPAN P0/P1：OS 页分配（VirtualAlloc / mmap）
 * 供 runtime_gc.c mspan 使用；禁止对 CRT calloc 块 VirtualFree。
 */
#include "runtime_internal.h"

#ifdef _WIN32
/* 实现在 runtime_winapi.c */
#else
#include <sys/mman.h>
void* hao_os_valloc(size_t n) {
    if (n == 0) return NULL;
    void* p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    return p;
}
void hao_os_vfree(void* p, size_t n) {
    if (!p || n == 0) return;
    munmap(p, n);
}
#endif
