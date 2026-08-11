/*
 * HaoLang 运行时 —— Metrics 表化（R3）
 * ------------------------------------------------------------
 *  计数器名称 → getter 一张表；业务/摘要仍走原有 hao_gc_* / GC.summary。
 *  本模块只提供统一枚举入口，禁止散落 printf。
 */
#include "runtime_internal.h"

typedef int64_t (*hao_metric_get_fn)(void);

typedef struct {
    const char* name;
    hao_metric_get_fn get;
} HaoMetricSlot;

/* 与 GC.stats / summary 字段对齐的只读视图 */
static const HaoMetricSlot g_hao_metrics[] = {
    {"liveBytes", hao_gc_live_bytes},
    {"heapBytes", hao_gc_heap_bytes},
    {"collectCount", hao_gc_collect_count},
    {"minorCount", hao_gc_minor_count},
    {"majorCount", hao_gc_major_count},
    {"blockCount", hao_gc_block_count},
    {"remsetCount", hao_gc_remset_count},
    {"verifySkipReenter", hao_gc_verify_skip_reenter},
};

size_t hao_metric_count(void) {
    return sizeof(g_hao_metrics) / sizeof(g_hao_metrics[0]);
}

const char* hao_metric_name(size_t i) {
    if (i >= hao_metric_count()) return NULL;
    return g_hao_metrics[i].name;
}

int64_t hao_metric_value(size_t i) {
    if (i >= hao_metric_count()) return 0;
    return g_hao_metrics[i].get();
}

void hao_metrics_fprint(FILE* f) {
    size_t i, n;
    if (!f) return;
    n = hao_metric_count();
    fprintf(f, "metrics:");
    for (i = 0; i < n; ++i) {
        fprintf(f, " %s=%lld", g_hao_metrics[i].name,
                (long long)g_hao_metrics[i].get());
    }
    fprintf(f, "\n");
    fflush(f);
}
