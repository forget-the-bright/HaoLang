/*
 * HaoLang 运行时 —— os 系统库（v0.27：String = HaoString*）
 */
#include "runtime_internal.h"

HaoString* hao_os_readfile(HaoString* path) {
    if (!path) return NULL;
    FILE* fp = fopen(path->data, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    /* HaoString 须留 1 字节给 '\0'；hao_str_alloc 会把 >=INT32_MAX 夹成 INT32_MAX-1，
       调用方若仍按原 sz 读写会堆越界。上限统一为 INT32_MAX-1。 */
    if (sz > (long)(INT32_MAX - 1)) { fclose(fp); return NULL; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }

    HaoString* r = hao_str_alloc((int32_t)sz);
    size_t rd = fread(r->data, 1, (size_t)sz, fp);
    r->data[rd] = '\0';
    r->len = (int32_t)rd;
    fclose(fp);
    return rd == 0 && sz > 0 ? NULL : r;
}

int8_t hao_os_writefile(HaoString* path, HaoString* content) {
    if (!path || !content) return 0;
    FILE* fp = fopen(path->data, "wb");
    if (!fp) return 0;
    size_t w = fwrite(content->data, 1, (size_t)content->len, fp);
    int ok = (w == (size_t)content->len);
    fclose(fp);
    return ok ? 1 : 0;
}

int8_t hao_os_exists(HaoString* path) {
    if (!path) return 0;
    FILE* fp = fopen(path->data, "rb");
    if (fp) { fclose(fp); return 1; }
    return 0;
}

HaoString* hao_os_getenv(HaoString* name) {
    if (!name) return NULL;
    const char* v = getenv(name->data);
    if (!v) return NULL;
    return hao_str_from_cstr(v);
}

int64_t hao_os_system(HaoString* cmd) {
    if (!cmd) return -1;
    return (int64_t)system(cmd->data);
}

int8_t hao_os_remove(HaoString* path) {
    if (!path) return 0;
    return remove(path->data) == 0 ? 1 : 0;
}
