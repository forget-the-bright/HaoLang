/*
 * HaoLang 运行时 —— 异常（try / catch / finally / throw）
 * ------------------------------------------------------------
 *  用 setjmp/longjmp 实现的异常帧链。IR 生成器为每个 try 生成：
 *
 *      %r = call i32 @hao_try_begin()       ; 压帧 + setjmp
 *      switch i32 %r, label %try.body [ i32 1, label %exc.dispatch ]
 *    try.body:   ...正常体...  call @hao_try_end()   ; 正常完成，弹帧
 *    exc.dispatch:
 *      %ex = call ptr @hao_except_capture() ; 弹帧并取回异常对象
 *      ...用 hao_type_is 逐一匹配 catch...
 *      匹配   -> 执行 catch 体 -> finally
 *      都不匹配 -> finally -> call @hao_rethrow(%ex)
 *    finally:    ...finally 体...
 *
 *  关键点：
 *   - setjmp 不能放在 hao_try_alloc 这样会返回的辅助函数里——一旦该
 *     函数返回，它的栈帧就失效，longjmp 跳回去会崩溃。因此
 *     hao_try_alloc 只分配并初始化帧记录、返回 jmp_buf 指针，真正的
 *     setjmp 由 IR 在用户函数自身栈帧里直接调用。
 *   - 局部变量都 alloca 在栈上，longjmp 回 setjmp 点后，try 之前
 *     声明的变量内存仍然有效（store/load 不依赖寄存器），catch 能
 *     读到 try 中修改过的外层变量。
 *   - 一进入 dispatch 就 hao_except_capture() 弹帧，catch 体中再
 *     throw 会直接跳到外层 try，而不是回到本帧造成死循环。
 *   - 异常栈为 __thread TLS（与 GC 栈顶同模型），多线程各自独立 try/catch。
 */
#include "runtime_internal.h"

typedef struct HaoExFrame {
    jmp_buf buf;
    void* exception;
    int id;             // 本帧的唯一标识（单调递增），供 hao_try_end 幂等判断
    int depthAtEnter;   // 压入本帧前的 g_excDepth，用于跨函数 throw 时恢复
} HaoExFrame;

/* 帧由运行时自行管理（TLS 静态数组，保证 jmp_buf 生命周期不依赖任何
 * 函数栈帧），IR 只需持有它返回的不透明指针。*/
#define HAO_EXC_MAX_DEPTH 256
#ifdef _WIN32
static __declspec(thread) HaoExFrame g_excStack[HAO_EXC_MAX_DEPTH];
static __declspec(thread) int g_excDepth = 0;
static __declspec(thread) int g_excNextId = 1;
#else
static __thread HaoExFrame g_excStack[HAO_EXC_MAX_DEPTH];
static __thread int g_excDepth = 0;
static __thread int g_excNextId = 1;
#endif

/* 压入异常帧并返回其 jmp_buf 指针（供 IR 直接传给 setjmp）。
 * 同时把本帧 id 写入 *outId，供 hao_try_end 幂等弹帧。 */
void* hao_try_alloc(int* outId) {
    if (g_excDepth >= HAO_EXC_MAX_DEPTH) {
        fputs("panic: try 嵌套过深\n", stderr);
        exit(1);
    }
    HaoExFrame* f = &g_excStack[g_excDepth];
    f->exception = NULL;
    f->depthAtEnter = g_excDepth;
    f->id = g_excNextId++;
    ++g_excDepth;
    if (outId) *outId = f->id;
    return &f->buf;
}

/* try 体正常完成：弹出本帧。传入 begin 返回的 id；
 * 若本帧已被 except_capture 弹出（异常路径），则什么都不做——
 * 因此在共享的 cleanup 块里无条件调用也是安全的。 */
void hao_try_end(int id) {
    if (g_excDepth > 0 && g_excStack[g_excDepth - 1].id == id)
        --g_excDepth;
}

/* 异常到达本帧（setjmp 返回 1 后立即调用）：弹出本帧并返回异常对象。
 * 必须在做任何 catch 匹配之前调用，保证 catch 体中再 throw 时
 * 栈顶已指向外层帧。depthAtEnter 用于丢弃跨函数 throw 时中间函数
 * 残留在栈上的帧（setjmp/longjmp 不会逐个返回那些函数）。 */
void* hao_except_capture(void) {
    if (g_excDepth <= 0) return NULL;
    HaoExFrame* f = &g_excStack[g_excDepth - 1];
    void* e = f->exception;
    f->exception = NULL;
    if (e) hao_gc_remove_root(e); /* 与 throw 成对：异常对象离开 TLS 临时根 */
    g_excDepth = f->depthAtEnter;
    return e;
}

/* 抛出异常：沿帧栈 longjmp 到最近的 try；没有则终止进程 */
void hao_throw(void* obj) {
    if (g_excDepth <= 0) {
        hao_report_fatal("uncaught", "未捕获的异常");
    }
    HaoExFrame* f = &g_excStack[g_excDepth - 1];
    f->exception = obj;
    /* STW 期间 TLS 异常指针不在栈上，须挂外部根防误回收 */
    if (obj) hao_gc_add_root(obj);
    longjmp(f->buf, 1);
}

/* 重新抛出（用于 catch 都不匹配、finally 之后）*/
void hao_rethrow(void* obj) {
    hao_throw(obj);
}
