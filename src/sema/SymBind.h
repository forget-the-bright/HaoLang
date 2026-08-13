// ============================================================
//  Sema 符号登记（G2-SYM）：所有业务 declare* 只经本模块
//  IRGen 禁止直接 syms_.declare / declareGlobal（门禁 sema_sym_gate）
// ============================================================
#pragma once

#include "sema/SymbolTable.h"

namespace hao {

/** 当前作用域登记局部/参数/绑定符号 */
bool symDeclare(SymbolTable& syms, const SymbolPtr& sym);

/** 全局作用域登记函数/类/接口代表符号 */
bool symDeclareGlobal(SymbolTable& syms, const SymbolPtr& sym);

} // namespace hao
