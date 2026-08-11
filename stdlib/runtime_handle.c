/*
 * HaoLang 运行时 —— NativeHandle（v0.55.61）
 * ------------------------------------------------------------
 *  C 原生资源的 GC 托管代理句柄。
 *  权责：raw 仍属 C 运行时；Handle 只保证有且仅一次调用 drop（代理释放）。
 *  禁止把 raw 暴露给 Hao 用户代码；禁止用 Long? 藏针。
 */
#include "runtime_internal.h"

static void hao_handle_finalize(void* user) {
    HaoNativeHandle* h = (HaoNativeHandle*)user;
    if (!h || h->closed) return;
    /* 块已摘链：只 drop，勿再进 GC */
    if (h->drop && h->raw) h->drop(h->raw);
    h->raw = NULL;
    h->drop = NULL;
    h->closed = 1;
}

HaoNativeHandle* hao_handle_alloc(void) {
    HaoNativeHandle* h = (HaoNativeHandle*)gc_alloc_ex(
        sizeof(HaoNativeHandle), GC_KIND_OPAQUE, 0);
    h->raw = NULL;
    h->drop = NULL;
    h->closed = 1;
    h->_pad = 0;
    return h;
}

void hao_handle_close(HaoNativeHandle* h) {
    if (!h || h->closed) return;
    hao_gc_clear_finalizer(h);
    if (h->drop && h->raw) h->drop(h->raw);
    h->raw = NULL;
    h->drop = NULL;
    h->closed = 1;
}

void hao_handle_attach(HaoNativeHandle* h, void* raw, HaoNativeDrop drop) {
    if (!h) return;
    hao_handle_close(h);
    h->raw = raw;
    h->drop = drop;
    h->closed = 0;
    hao_gc_set_finalizer(h, hao_handle_finalize);
}

void* hao_handle_raw(HaoNativeHandle* h) {
    if (!h || h->closed) return NULL;
    return h->raw;
}

int8_t hao_handle_is_open(HaoNativeHandle* h) {
    return (h && !h->closed && h->raw) ? 1 : 0;
}

/* 包装永生/外部指针：drop=NULL，GC 回收 Handle 时不 free raw */
HaoNativeHandle* hao_handle_wrap(void* raw) {
    HaoNativeHandle* h = hao_handle_alloc();
    if (raw) hao_handle_attach(h, raw, NULL);
    return h;
}

/* 比较两句柄的 raw（reflect meta 身份） */
int8_t hao_handle_raw_eq(HaoNativeHandle* a, HaoNativeHandle* b) {
    return hao_handle_raw(a) == hao_handle_raw(b) ? 1 : 0;
}

/* sync 原子胞：malloc(8) 置 0，drop=free */
static void hao_sync_cell_drop(void* raw) {
    free(raw);
}

HaoNativeHandle* hao_sync_cell_new(void) {
    int64_t* cell = (int64_t*)calloc(1, sizeof(int64_t));
    HaoNativeHandle* h;
    if (!cell) {
        fputs("panic: hao_sync_cell_new OOM\n", stderr);
        exit(1);
    }
    h = hao_handle_alloc();
    hao_handle_attach(h, cell, hao_sync_cell_drop);
    return h;
}

/* ---- FFI 拷贝（属 C malloc；调用方 free）---- */

char* hao_ffi_dup_cstr(HaoString* s) {
    int32_t n;
    const char* src;
    char* out;
    if (!s) {
        out = (char*)malloc(1);
        if (out) out[0] = '\0';
        return out;
    }
    hao_gc_add_root(s);
    n = hao_str_byte_len(s);
    src = hao_str_cstr(s);
    if (!src) src = "";
    if (n < 0) n = 0;
    out = (char*)malloc((size_t)n + 1);
    if (!out) {
        hao_gc_remove_root(s);
        fputs("panic: hao_ffi_dup_cstr OOM\n", stderr);
        exit(1);
    }
    if (n > 0) memcpy(out, src, (size_t)n);
    out[n] = '\0';
    hao_gc_remove_root(s);
    return out;
}

void* hao_ffi_dup_bytes(const void* p, size_t n) {
    void* out;
    if (n == 0) {
        out = malloc(1);
        return out;
    }
    if (!p) return NULL;
    out = malloc(n);
    if (!out) {
        fputs("panic: hao_ffi_dup_bytes OOM\n", stderr);
        exit(1);
    }
    memcpy(out, p, n);
    return out;
}
