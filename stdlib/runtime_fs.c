/*
 * HaoLang 运行时 —— 文件流（v0.28 FileStream）
 * FILE* 句柄存于 Long?（8 字节 GC 块）。
 */
#include "runtime_internal.h"

static void hao_fs_store(int64_t* unit, FILE* fp) {
    *unit = (int64_t)(uintptr_t)fp;
}
static FILE* hao_fs_load(const int64_t* unit) {
    return (FILE*)(uintptr_t)(*unit);
}

/* 清扫回调：块已摘链；只 fclose，勿再进 GC */
static void hao_fs_unit_finalize(void* user) {
    int64_t* unit = (int64_t*)user;
    if (!unit) return;
    FILE* fp = hao_fs_load(unit);
    if (fp) fclose(fp);
    *unit = 0;
}

int8_t hao_fs_open(int64_t* unit, HaoString* path, HaoString* mode) {
    if (!unit || !path || !mode) return 0;
    FILE* old = hao_fs_load(unit);
    if (old) {
        hao_gc_clear_finalizer(unit);
        fclose(old);
        *unit = 0;
    }
    FILE* fp = fopen(path->data, mode->data);
    if (!fp) return 0;
    hao_fs_store(unit, fp);
    hao_gc_set_finalizer(unit, hao_fs_unit_finalize);
    return 1;
}

int32_t hao_fs_close(int64_t* unit) {
    if (!unit) return -1;
    hao_gc_clear_finalizer(unit);
    FILE* fp = hao_fs_load(unit);
    if (!fp) return 0;
    int rc = fclose(fp);
    *unit = 0;
    return rc == 0 ? 0 : -1;
}

int32_t hao_fs_write_str(int64_t* unit, HaoString* data) {
    FILE* fp = hao_fs_load(unit);
    if (!fp || !data) return -1;
    size_t w = fwrite(data->data, 1, (size_t)data->len, fp);
    return (int32_t)w;
}

HaoString* hao_fs_read_str(int64_t* unit, int32_t max) {
    FILE* fp = hao_fs_load(unit);
    if (!fp || max <= 0) return NULL;
    /* 与 hao_str_alloc 容量同源：最多 INT32_MAX-1 字节有效载荷 */
    if (max > INT32_MAX - 1) max = INT32_MAX - 1;
    HaoString* buf = hao_str_alloc(max);
    size_t n = fread(buf->data, 1, (size_t)max, fp);
    buf->data[n] = '\0';
    buf->len = (int32_t)n;
    return buf;
}

int32_t hao_fs_write_bytes(int64_t* unit, void* arr) {
    FILE* fp = hao_fs_load(unit);
    if (!fp || !arr) return -1;
    int64_t len = hao_array_len(arr);
    int64_t esz = *(int64_t*)((char*)arr - HAO_ARR_ESZ_OFF);
    if (esz != 1) return -1;
    if (len <= 0) return 0;
    size_t w = fwrite(arr, 1, (size_t)len, fp);
    return (int32_t)w;
}

void* hao_fs_read_bytes(int64_t* unit, int32_t max) {
    FILE* fp = hao_fs_load(unit);
    if (!fp || max <= 0) return hao_array_new(0, 1, 0);
    if (max > INT32_MAX - 1) max = INT32_MAX - 1;
    void* arr = hao_array_new(max, 1, 0);
    size_t n = fread(arr, 1, (size_t)max, fp);
    /* 缩 len */
    *(int64_t*)((char*)arr - HAO_ARR_LEN_OFF) = (int64_t)n;
    return arr;
}

int8_t hao_fs_seek(int64_t* unit, int64_t off, int32_t whence) {
    FILE* fp = hao_fs_load(unit);
    if (!fp) return 0;
    return fseek(fp, (long)off, whence) == 0 ? 1 : 0;
}

int64_t hao_fs_tell(int64_t* unit) {
    FILE* fp = hao_fs_load(unit);
    if (!fp) return -1;
    return (int64_t)ftell(fp);
}

int8_t hao_fs_flush(int64_t* unit) {
    FILE* fp = hao_fs_load(unit);
    if (!fp) return 0;
    return fflush(fp) == 0 ? 1 : 0;
}
