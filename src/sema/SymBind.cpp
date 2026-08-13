#include "sema/SymBind.h"

namespace hao {

bool symDeclare(SymbolTable& syms, const SymbolPtr& sym) {
    return syms.declare(sym);
}

bool symDeclareGlobal(SymbolTable& syms, const SymbolPtr& sym) {
    return syms.declareGlobal(sym);
}

} // namespace hao
