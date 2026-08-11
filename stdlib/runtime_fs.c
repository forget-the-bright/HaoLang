/*
 * HaoLang 运行时 —— 文件流（v0.55.61：NativeHandle 代理 FILE*）
 * FILE* 属 C；Handle 代理 fclose。路径/模式/写串经 hao_ffi_dup_* 拷贝出桥。
 * 阻塞 I/O 包 hao_gc_os_block_*；形参挂根成对。
 */
#include "runtime_internal.h"

static void hao_fs_drop_file(void* raw) {
    if (raw) fclose((FILE*)raw);
}

static FILE* hao_fs_file(HaoNativeHandle* h) {
    return (FILE*)hao_handle_raw(h);
}

int8_t hao_fs_open(HaoNativeHandle* h, HaoString* path, HaoString* mode) {
    char* path_c;
    char* mode_c;
    FILE* fp;
    if (!h || !path || !mode) return 0;

    hao_gc_add_root(h);
    hao_gc_add_root(path);
    hao_gc_add_root(mode);
    path_c = hao_ffi_dup_cstr(path);
    mode_c = hao_ffi_dup_cstr(mode);
    hao_gc_os_block_enter();
    fp = fopen(path_c, mode_c);
    hao_gc_os_block_leave();
    free(path_c);
    free(mode_c);
    hao_gc_remove_root(path);
    hao_gc_remove_root(mode);
    if (!fp) {
        hao_gc_remove_root(h);
        return 0;
    }
    hao_handle_attach(h, fp, hao_fs_drop_file);
    hao_gc_remove_root(h);
    return 1;
}

int32_t hao_fs_close(HaoNativeHandle* h) {
    if (!h) return -1;
    hao_handle_close(h);
    return 0;
}

int32_t hao_fs_write_str(HaoNativeHandle* h, HaoString* data) {
    FILE* fp = hao_fs_file(h);
    char* buf;
    int32_t n;
    size_t w;
    if (!fp || !data) return -1;
    hao_gc_add_root(h);
    hao_gc_add_root(data);
    n = hao_str_byte_len(data);
    buf = hao_ffi_dup_cstr(data);
    hao_gc_os_block_enter();
    w = fwrite(buf, 1, (size_t)n, fp);
    hao_gc_os_block_leave();
    free(buf);
    hao_gc_remove_root(data);
    hao_gc_remove_root(h);
    return (int32_t)w;
}

HaoString* hao_fs_read_str(HaoNativeHandle* h, int32_t max) {
    FILE* fp = hao_fs_file(h);
    HaoString* buf;
    size_t n;
    if (!fp || max <= 0) return NULL;
    if (max > INT32_MAX - 1) max = INT32_MAX - 1;
    hao_gc_add_root(h);
    buf = hao_str_alloc(max);
    hao_gc_add_root(buf);
    hao_gc_os_block_enter();
    n = fread(hao_str_data(buf), 1, (size_t)max, fp);
    hao_gc_os_block_leave();
    hao_str_set_byte_len(buf, (int32_t)n);
    hao_gc_remove_root(buf);
    hao_gc_remove_root(h);
    return buf;
}

int32_t hao_fs_write_bytes(HaoNativeHandle* h, void* arr) {
    FILE* fp = hao_fs_file(h);
    int64_t len;
    int64_t esz;
    void* copy;
    size_t w;
    if (!fp || !arr) return -1;
    len = hao_array_len(arr);
    esz = *(int64_t*)((char*)arr - HAO_ARR_ESZ_OFF);
    if (esz != 1) return -1;
    if (len <= 0) return 0;
    hao_gc_add_root(h);
    hao_gc_add_root(arr);
    copy = hao_ffi_dup_bytes(arr, (size_t)len);
    hao_gc_os_block_enter();
    w = fwrite(copy, 1, (size_t)len, fp);
    hao_gc_os_block_leave();
    free(copy);
    hao_gc_remove_root(arr);
    hao_gc_remove_root(h);
    return (int32_t)w;
}

void* hao_fs_read_bytes(HaoNativeHandle* h, int32_t max) {
    FILE* fp = hao_fs_file(h);
    void* arr;
    size_t n;
    if (!fp || max <= 0) return hao_array_new(0, 1, 0);
    if (max > INT32_MAX - 1) max = INT32_MAX - 1;
    hao_gc_add_root(h);
    arr = hao_array_new(max, 1, 0);
    hao_gc_add_root(arr);
    hao_gc_os_block_enter();
    n = fread(arr, 1, (size_t)max, fp);
    hao_gc_os_block_leave();
    *(int64_t*)((char*)arr - HAO_ARR_LEN_OFF) = (int64_t)n;
    hao_gc_remove_root(arr);
    hao_gc_remove_root(h);
    return arr;
}

int8_t hao_fs_seek(HaoNativeHandle* h, int64_t off, int32_t whence) {
    FILE* fp = hao_fs_file(h);
    int rc;
    if (!fp) return 0;
    hao_gc_os_block_enter();
    rc = fseek(fp, (long)off, whence);
    hao_gc_os_block_leave();
    return rc == 0 ? 1 : 0;
}

int64_t hao_fs_tell(HaoNativeHandle* h) {
    FILE* fp = hao_fs_file(h);
    long pos;
    if (!fp) return -1;
    hao_gc_os_block_enter();
    pos = ftell(fp);
    hao_gc_os_block_leave();
    return (int64_t)pos;
}

int8_t hao_fs_flush(HaoNativeHandle* h) {
    FILE* fp = hao_fs_file(h);
    int rc;
    if (!fp) return 0;
    hao_gc_os_block_enter();
    rc = fflush(fp);
    hao_gc_os_block_leave();
    return rc == 0 ? 1 : 0;
}
