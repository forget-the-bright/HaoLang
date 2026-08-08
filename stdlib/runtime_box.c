/*
 * HaoLang 运行时 —— 可空值类型装箱
 *  Long?/Double? → 8 字节单元；Int?/Short?/Byte?/Bool?/Float? → 8 字节槽内窄值。
 *  GC v3：叶对象（OPAQUE），载荷不是堆指针。
 */
#include "runtime_internal.h"

void* hao_box_i64(int64_t v) {
    int64_t* p = (int64_t*)gc_alloc_ex(sizeof(int64_t), GC_KIND_OPAQUE, 0);
    *p = v;
    return p;
}

void* hao_box_f64(double v) {
    double* p = (double*)gc_alloc_ex(sizeof(double), GC_KIND_OPAQUE, 0);
    *p = v;
    return p;
}

void* hao_box_f32(float v) {
    float* p = (float*)gc_alloc_ex(8, GC_KIND_OPAQUE, 0);
    *p = v;
    return (void*)p;
}

void* hao_box_i32(int32_t v) {
    int32_t* p = (int32_t*)gc_alloc_ex(8, GC_KIND_OPAQUE, 0);
    *p = v;
    return (void*)p;
}

int64_t hao_unbox_i64(void* p) { return *(int64_t*)p; }
double  hao_unbox_f64(void* p) { return *(double*)p;  }
float   hao_unbox_f32(void* p) { return *(float*)p;   }
int32_t hao_unbox_i32(void* p) { return *(int32_t*)p; }
