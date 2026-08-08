/*
 * HaoLang 运行时 —— 动态库加载（v0.21 / 5.12）
 * ------------------------------------------------------------
 *  统一封装打开系统预装 DLL/so 与取符号，避免链接期依赖 SDK 导入库。
 *
 *  Windows：不 #include <windows.h>、不产生 __imp_LoadLibrary 等导入。
 *  通过 TEB→PEB→LDR 找到已映射的 kernel32.dll，解析导出表得到
 *  GetProcAddress / LoadLibraryA / FreeLibrary，再对外提供 hao_dl_*。
 *  （kernel32 在每个 Win32 进程里必然已加载，无需先 LoadLibrary。）
 *
 *  POSIX：dlopen / dlsym / dlclose。
 */
#include "runtime_internal.h"

#ifndef _WIN32
#include <dlfcn.h>
#else

#include <intrin.h>

/* ---- 最小 PE/LDR 布局（手写，不依赖 windows.h）---- */
typedef struct HaoListEntry {
    struct HaoListEntry* Flink;
    struct HaoListEntry* Blink;
} HaoListEntry;

typedef struct HaoUnicodeString {
    uint16_t Length;
    uint16_t MaximumLength;
    uint16_t* Buffer;
} HaoUnicodeString;

typedef struct HaoDosHeader {
    uint16_t e_magic;
    uint16_t e_cblp, e_cp, e_crlc, e_cparhdr, e_minalloc, e_maxalloc;
    uint16_t e_ss, e_sp, e_csum, e_ip, e_cs, e_lfarlc, e_ovno;
    uint16_t e_res[4];
    uint16_t e_oemid, e_oeminfo;
    uint16_t e_res2[10];
    int32_t  e_lfanew;
} HaoDosHeader;

typedef struct HaoDataDir {
    uint32_t VirtualAddress;
    uint32_t Size;
} HaoDataDir;

typedef struct HaoExportDir {
    uint32_t Characteristics;
    uint32_t TimeDateStamp;
    uint16_t MajorVersion;
    uint16_t MinorVersion;
    uint32_t Name;
    uint32_t Base;
    uint32_t NumberOfFunctions;
    uint32_t NumberOfNames;
    uint32_t AddressOfFunctions;
    uint32_t AddressOfNames;
    uint32_t AddressOfNameOrdinals;
} HaoExportDir;

typedef void*  (__stdcall *HaoFn_LoadLibraryA)(const char*);
typedef void*  (__stdcall *HaoFn_GetProcAddress)(void*, const char*);
typedef int    (__stdcall *HaoFn_FreeLibrary)(void*);

static HaoFn_LoadLibraryA    g_LoadLibraryA = NULL;
static HaoFn_GetProcAddress  g_GetProcAddress = NULL;
static HaoFn_FreeLibrary     g_FreeLibrary = NULL;
static int                   g_k32_ready = 0;

static void* hao_teb(void) {
#if defined(_M_X64) || defined(__x86_64__)
    return (void*)__readgsqword(0x30);
#elif defined(_M_ARM64) || defined(__aarch64__)
    void* t;
    __asm__ __volatile__("mov %0, x18" : "=r"(t));
    return t;
#else
#error "hao_dl: unsupported Windows arch for TEB access"
#endif
}

/* 当前线程栈顶（NT_TIB.StackBase，TEB+0x08）。供 GC 使用。 */
char* hao_win_stack_base(void) {
    return *(char**)((char*)hao_teb() + 0x08);
}

static int hao_wcs_eq_ci(const uint16_t* a, size_t abytes, const char* ascii) {
    size_t n = abytes / 2;
    size_t i = 0;
    for (; i < n && ascii[i]; ++i) {
        uint16_t ca = a[i];
        char cb = ascii[i];
        if (ca >= 'A' && ca <= 'Z') ca = (uint16_t)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != (uint16_t)(unsigned char)cb) return 0;
    }
    return ascii[i] == 0 && i == n;
}

static void* hao_pe_get_export(void* base, const char* name) {
    if (!base || !name) return NULL;
    const HaoDosHeader* dos = (const HaoDosHeader*)base;
    if (dos->e_magic != 0x5A4D) return NULL; /* MZ */
    const uint8_t* p = (const uint8_t*)base + dos->e_lfanew;
    if (*(const uint32_t*)p != 0x00004550) return NULL; /* PE\0\0 */
    /* COFF + optional header：Magic at +0x18 from PE sig */
    const uint16_t magic = *(const uint16_t*)(p + 0x18);
    uint32_t export_rva;
    if (magic == 0x20B) { /* PE32+ */
        export_rva = *(const uint32_t*)(p + 0x88); /* DataDirectory[0] */
    } else if (magic == 0x10B) { /* PE32 */
        export_rva = *(const uint32_t*)(p + 0x78);
    } else {
        return NULL;
    }
    if (!export_rva) return NULL;
    const HaoExportDir* exp = (const HaoExportDir*)((const uint8_t*)base + export_rva);
    const uint32_t* names = (const uint32_t*)((const uint8_t*)base + exp->AddressOfNames);
    const uint16_t* ords = (const uint16_t*)((const uint8_t*)base + exp->AddressOfNameOrdinals);
    const uint32_t* funcs = (const uint32_t*)((const uint8_t*)base + exp->AddressOfFunctions);
    for (uint32_t i = 0; i < exp->NumberOfNames; ++i) {
        const char* nm = (const char*)base + names[i];
        const char* a = name;
        const char* b = nm;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (*a == 0 && *b == 0) {
            uint32_t rva = funcs[ords[i]];
            return (void*)((uint8_t*)base + rva);
        }
    }
    return NULL;
}

static void* hao_find_kernel32(void) {
    void* teb = hao_teb();
    if (!teb) return NULL;
    void* peb = *(void**)((char*)teb + 0x60); /* PEB* */
    if (!peb) return NULL;
    void* ldr = *(void**)((char*)peb + 0x18); /* PEB_LDR_DATA* */
    if (!ldr) return NULL;
    HaoListEntry* head = (HaoListEntry*)((char*)ldr + 0x20); /* InMemoryOrderModuleList */
    HaoListEntry* cur = head->Flink;
    for (int guard = 0; cur && cur != head && guard < 256; ++guard, cur = cur->Flink) {
        /* cur 指向 InMemoryOrderLinks（结构内偏移 0x10） */
        void* dll_base = *(void**)((char*)cur + 0x20);       /* DllBase */
        HaoUnicodeString* base_name =
            (HaoUnicodeString*)((char*)cur + 0x48);          /* BaseDllName */
        if (!dll_base || !base_name || !base_name->Buffer) continue;
        if (hao_wcs_eq_ci(base_name->Buffer, base_name->Length, "kernel32.dll"))
            return dll_base;
    }
    return NULL;
}

/* 0=未开始 1=进行中 2=成功 3=失败 */
static int g_k32_once = 0;

static int hao_k32_bootstrap(void) {
    int st = __atomic_load_n(&g_k32_once, __ATOMIC_ACQUIRE);
    if (st == 2) return g_GetProcAddress != NULL;
    if (st == 3) return 0;
    int expected = 0;
    if (!__atomic_compare_exchange_n(&g_k32_once, &expected, 1, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        while ((st = __atomic_load_n(&g_k32_once, __ATOMIC_ACQUIRE)) == 1)
            ; /* 自旋：此时尚不可调 Sleep/SwitchToThread（符号未就绪） */
        return st == 2 && g_GetProcAddress != NULL;
    }
    void* k32 = hao_find_kernel32();
    if (!k32) {
        __atomic_store_n(&g_k32_once, 3, __ATOMIC_RELEASE);
        return 0;
    }
    g_GetProcAddress = (HaoFn_GetProcAddress)hao_pe_get_export(k32, "GetProcAddress");
    g_LoadLibraryA   = (HaoFn_LoadLibraryA)hao_pe_get_export(k32, "LoadLibraryA");
    g_FreeLibrary    = (HaoFn_FreeLibrary)hao_pe_get_export(k32, "FreeLibrary");
    if (!g_GetProcAddress || !g_LoadLibraryA || !g_FreeLibrary) {
        g_GetProcAddress = NULL;
        g_LoadLibraryA = NULL;
        g_FreeLibrary = NULL;
        __atomic_store_n(&g_k32_once, 3, __ATOMIC_RELEASE);
        return 0;
    }
    g_k32_ready = 1;
    __atomic_store_n(&g_k32_once, 2, __ATOMIC_RELEASE);
    return 1;
}

#endif /* _WIN32 */

void* hao_dl_open(const char* name) {
    if (!name || !*name) return NULL;
#ifdef _WIN32
    if (!hao_k32_bootstrap()) return NULL;
    return (void*)g_LoadLibraryA(name);
#else
    return dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
}

void* hao_dl_sym(void* handle, const char* name) {
    if (!handle || !name || !*name) return NULL;
#ifdef _WIN32
    if (!hao_k32_bootstrap()) return NULL;
    return (void*)g_GetProcAddress(handle, name);
#else
    return dlsym(handle, name);
#endif
}

void hao_dl_close(void* handle) {
    if (!handle) return;
#ifdef _WIN32
    if (!hao_k32_bootstrap()) return;
    g_FreeLibrary(handle);
#else
    dlclose(handle);
#endif
}
