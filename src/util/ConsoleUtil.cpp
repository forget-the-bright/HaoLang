// ============================================================
//  HaoLang —— 控制台工具实现
// ============================================================

#include "util/ConsoleUtil.h"

#ifdef _WIN32
#include <windows.h>
#include <cstdlib>
#endif

namespace hao {

void initConsoleUtf8() {
#ifdef _WIN32
    static bool done = false;
    if (done) return;
    done = true;

    static UINT oldCP = 0;
    oldCP = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    // 退出时恢复，避免污染终端会话
    std::atexit([]() { if (oldCP) SetConsoleOutputCP(oldCP); });
#endif
}

} // namespace hao
