/*
 * HaoLang 运行时 —— os 系统库
 * 路径/命令经 hao_ffi_dup_cstr 拷贝出桥后再调 libc（禁长期持有 GC 堆内指针）。
 * 阻塞调用包 hao_gc_os_block_*；形参挂根成对。
 * v0.55.62：readFile/writeFile/exists 已上移 Hao（FileStream）；本文件只留薄 libc。
 */
#include "runtime_internal.h"

HaoString* hao_os_getenv(HaoString* name) {
    char* name_c;
    const char* v;
    HaoString* r;
    if (!name) return NULL;
    hao_gc_add_root(name);
    name_c = hao_ffi_dup_cstr(name);
    v = getenv(name_c);
    free(name_c);
    r = v ? hao_str_from_cstr(v) : NULL;
    hao_gc_remove_root(name);
    return r;
}

int64_t hao_os_system(HaoString* cmd) {
    char* cmd_c;
    int64_t rc;
    if (!cmd) return -1;
    hao_gc_add_root(cmd);
    cmd_c = hao_ffi_dup_cstr(cmd);
    hao_gc_os_block_enter();
    rc = (int64_t)system(cmd_c);
    hao_gc_os_block_leave();
    free(cmd_c);
    hao_gc_remove_root(cmd);
    return rc;
}

int8_t hao_os_remove(HaoString* path) {
    char* path_c;
    int rc;
    if (!path) return 0;
    hao_gc_add_root(path);
    path_c = hao_ffi_dup_cstr(path);
    hao_gc_os_block_enter();
    rc = remove(path_c);
    hao_gc_os_block_leave();
    free(path_c);
    hao_gc_remove_root(path);
    return rc == 0 ? 1 : 0;
}
