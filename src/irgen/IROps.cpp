#include "irgen/IROps.h"

namespace hao {

void IROps::emitInst(const std::string& inst) {
    em_.emit(inst + dbgSuffix());
}

std::string IROps::emitLoad(const std::string& ty, const std::string& ptr) {
    std::string r = em_.nextTemp();
    emitInst(r + " = load " + ty + ", ptr " + ptr);
    return r;
}

void IROps::emitStore(const std::string& ty, const std::string& val,
                      const std::string& ptr) {
    emitInst("store " + ty + " " + val + ", ptr " + ptr);
}

std::string IROps::emitCall(const std::string& retTy, const std::string& callee,
                            const std::string& argsIr) {
    std::string r = em_.nextTemp();
    if (argsIr.empty())
        emitInst(r + " = call " + retTy + " " + callee + "()");
    else
        emitInst(r + " = call " + retTy + " " + callee + "(" + argsIr + ")");
    return r;
}

void IROps::emitCallVoid(const std::string& callee, const std::string& argsIr) {
    if (argsIr.empty())
        emitInst("call void " + callee + "()");
    else
        emitInst("call void " + callee + "(" + argsIr + ")");
}

void IROps::emitCallTyped(const std::string& fnTy, const std::string& callee,
                          const std::string& argsIr) {
    if (argsIr.empty())
        emitInst("call " + fnTy + " " + callee + "()");
    else
        emitInst("call " + fnTy + " " + callee + "(" + argsIr + ")");
}

void IROps::emitBr(const std::string& label) {
    emitInst("br label %" + label);
}

void IROps::emitCondBr(const std::string& cond, const std::string& trueL,
                       const std::string& falseL) {
    emitInst("br i1 " + cond + ", label %" + trueL + ", label %" + falseL);
}

void IROps::emitRetVoid() {
    emitInst("ret void");
}

void IROps::emitRet(const std::string& ty, const std::string& val) {
    emitInst("ret " + ty + " " + val);
}

std::string IROps::emitAlloca(const std::string& ty) {
    std::string r = em_.nextTemp();
    emitInst(r + " = alloca " + ty);
    return r;
}

std::string IROps::emitAllocaNamed(const std::string& hint,
                                   const std::string& ty) {
    std::string addr = em_.nextNamed(hint);
    emitInst(addr + " = alloca " + ty);
    return addr;
}

void IROps::emitAllocaAt(const std::string& addr, const std::string& ty) {
    emitInst(addr + " = alloca " + ty);
}

void IROps::emitDbgDeclare(const std::string& addr, const std::string& name,
                           unsigned line, unsigned arg) {
    if (!debugEnabled_ || addr.empty() || name.empty()) return;
    unsigned varId = em_.internDILocalVariable(name, line, arg);
    unsigned exprId = em_.diExpressionId();
    unsigned locId = em_.internDILocation(line ? line : 1, 1);
    if (varId == 0 || exprId == 0 || locId == 0) return;
    // 不走 emitInst：避免再叠一层 dbgSuffix；!dbg 已显式附上
    em_.emit("call void @llvm.dbg.declare(metadata ptr " + addr +
             ", metadata !" + std::to_string(varId) + ", metadata !" +
             std::to_string(exprId) + "), !dbg !" + std::to_string(locId));
}

void IROps::emitDbgValue(const std::string& llvmTy, const std::string& val,
                         const std::string& name, unsigned line, unsigned arg) {
    if (!debugEnabled_ || llvmTy.empty() || val.empty() || name.empty()) return;
    unsigned varId = em_.internDILocalVariable(name, line, arg);
    unsigned exprId = em_.diExpressionId();
    unsigned locId = em_.internDILocation(line ? line : 1, 1);
    if (varId == 0 || exprId == 0 || locId == 0) return;
    em_.emit("call void @llvm.dbg.value(metadata " + llvmTy + " " + val +
             ", metadata !" + std::to_string(varId) + ", metadata !" +
             std::to_string(exprId) + "), !dbg !" + std::to_string(locId));
}

std::string IROps::emitBinOp(const std::string& op, const std::string& ty,
                             const std::string& lhs, const std::string& rhs) {
    std::string r = em_.nextTemp();
    emitInst(r + " = " + op + " " + ty + " " + lhs + ", " + rhs);
    return r;
}

std::string IROps::emitICmp(const std::string& pred, const std::string& ty,
                            const std::string& lhs, const std::string& rhs) {
    std::string r = em_.nextTemp();
    emitInst(r + " = icmp " + pred + " " + ty + " " + lhs + ", " + rhs);
    return r;
}

std::string IROps::emitPhi(const std::string& ty,
                           const std::string& incomingsIr) {
    std::string r = em_.nextTemp();
    emitInst(r + " = phi " + ty + " " + incomingsIr);
    return r;
}

std::string IROps::emitFCmp(const std::string& pred, const std::string& ty,
                            const std::string& lhs, const std::string& rhs) {
    std::string r = em_.nextTemp();
    emitInst(r + " = fcmp " + pred + " " + ty + " " + lhs + ", " + rhs);
    return r;
}

std::string IROps::emitSelect(const std::string& cond, const std::string& ty,
                              const std::string& tVal,
                              const std::string& fVal) {
    std::string r = em_.nextTemp();
    emitInst(r + " = select i1 " + cond + ", " + ty + " " + tVal + ", " + ty +
             " " + fVal);
    return r;
}

std::string IROps::emitCast(const std::string& op, const std::string& fromTy,
                            const std::string& val, const std::string& toTy) {
    std::string r = em_.nextTemp();
    emitInst(r + " = " + op + " " + fromTy + " " + val + " to " + toTy);
    return r;
}

std::string IROps::emitGep(const std::string& pointeeTy, const std::string& ptr,
                           const std::string& idxTy, const std::string& idx) {
    std::string r = em_.nextTemp();
    emitInst(r + " = getelementptr " + pointeeTy + ", ptr " + ptr + ", " +
             idxTy + " " + idx);
    return r;
}

std::string IROps::emitPtrToInt(const std::string& intTy,
                                const std::string& ptr) {
    std::string r = em_.nextTemp();
    emitInst(r + " = ptrtoint ptr " + ptr + " to " + intTy);
    return r;
}

std::string IROps::emitIntToPtr(const std::string& intTy,
                                const std::string& val) {
    std::string r = em_.nextTemp();
    emitInst(r + " = inttoptr " + intTy + " " + val + " to ptr");
    return r;
}

std::string IROps::emitExtractValue(const std::string& aggTy,
                                    const std::string& agg, int index) {
    std::string r = em_.nextTemp();
    emitInst(r + " = extractvalue " + aggTy + " " + agg + ", " +
             std::to_string(index));
    return r;
}

std::string IROps::emitFNeg(const std::string& ty, const std::string& val) {
    std::string r = em_.nextTemp();
    emitInst(r + " = fneg " + ty + " " + val);
    return r;
}

void IROps::emitUnreachable() {
    emitInst("unreachable");
}

}  // namespace hao
