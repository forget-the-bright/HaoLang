// ============================================================
//  源码位置（I0）—— 供 IROps 挂 !dbg，不进 AST 分支手写 DI
// ============================================================
#pragma once

#include <string>

namespace hao {

struct SourceLoc {
    std::string file;
    unsigned line = 0;
    unsigned col = 0;

    bool valid() const { return line != 0; }
};

}  // namespace hao
