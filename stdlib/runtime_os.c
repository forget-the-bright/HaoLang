/*
 * HaoLang 运行时 —— os 系统库（v0.27：String = HaoString*）
 * 阻塞文件/进程调用包 hao_gc_os_block_*（v0.53.5）。
 * GC 形参皮带：add_root / remove_root 必须成对（v0.55.1）。
 */
#include "runtime_internal.h"

HaoString* hao_os_readfile(HaoString* path) {
    if (!path) return NULL;
    hao_gc_add_root(path);
    hao_gc_os_block_enter();
    FILE* fp = fopen(path->data, "rb");
    if (!fp) {
        hao_gc_os_block_leave();
        hao_gc_remove_root(path);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        hao_gc_os_block_leave();
        hao_gc_remove_root(path);
        return NULL;
    }
    long sz = ftell(fp);
    if (sz < 0) {
        fclose(fp);
        hao_gc_os_block_leave();
        hao_gc_remove_root(path);
        return NULL;
    }
    /* HaoString 须留 1 字节给 '\0'；hao_str_alloc 会把 >=INT32_MAX 夹成 INT32_MAX-1，
       调用方若仍按原 sz 读写会堆越界。上限统一为 INT32_MAX-1。 */
    if (sz > (long)(INT32_MAX - 1)) {
        fclose(fp);
        hao_gc_os_block_leave();
        hao_gc_remove_root(path);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        hao_gc_os_block_leave();
        hao_gc_remove_root(path);
        return NULL;
    }

    /* alloc 可能触发 GC：先 leave，再分配，再 enter 读 */
    hao_gc_os_block_leave();
    HaoString* r = hao_str_alloc((int32_t)sz);
    hao_gc_add_root(r);
    hao_gc_os_block_enter();
    size_t rd = fread(r->data, 1, (size_t)sz, fp);
    r->data[rd] = '\0';
    r->len = (int32_t)rd;
    fclose(fp);
    hao_gc_os_block_leave();
    hao_gc_remove_root(path);
    hao_gc_remove_root(r);
    return rd == 0 && sz > 0 ? NULL : r;
}

int8_t hao_os_writefile(HaoString* path, HaoString* content) {
    if (!path || !content) return 0;
    hao_gc_add_root(path);
    hao_gc_add_root(content);
    hao_gc_os_block_enter();
    FILE* fp = fopen(path->data, "wb");
    if (!fp) {
        hao_gc_os_block_leave();
        hao_gc_remove_root(path);
        hao_gc_remove_root(content);
        return 0;
    }
    size_t w = fwrite(content->data, 1, (size_t)content->len, fp);
    int ok = (w == (size_t)content->len);
    fclose(fp);
    hao_gc_os_block_leave();
    hao_gc_remove_root(path);
    hao_gc_remove_root(content);
    return ok ? 1 : 0;
}

int8_t hao_os_exists(HaoString* path) {
    if (!path) return 0;
    hao_gc_add_root(path);
    hao_gc_os_block_enter();
    FILE* fp = fopen(path->data, "rb");
    if (fp) {
        fclose(fp);
        hao_gc_os_block_leave();
        hao_gc_remove_root(path);
        return 1;
    }
    hao_gc_os_block_leave();
    hao_gc_remove_root(path);
    return 0;
}

HaoString* hao_os_getenv(HaoString* name) {
    if (!name) return NULL;
    /* str_from_cstr 会分配：name 须挂根（对齐 system/readfile） */
    hao_gc_add_root(name);
    const char* v = getenv(name->data);
    HaoString* r = v ? hao_str_from_cstr(v) : NULL;
    hao_gc_remove_root(name);
    return r;
}

int64_t hao_os_system(HaoString* cmd) {
    if (!cmd) return -1;
    hao_gc_add_root(cmd);
    hao_gc_os_block_enter();
    int64_t rc = (int64_t)system(cmd->data);
    hao_gc_os_block_leave();
    hao_gc_remove_root(cmd);
    return rc;
}

int8_t hao_os_remove(HaoString* path) {
    if (!path) return 0;
    hao_gc_add_root(path);
    hao_gc_os_block_enter();
    int rc = remove(path->data);
    hao_gc_os_block_leave();
    hao_gc_remove_root(path);
    return rc == 0 ? 1 : 0;
}
