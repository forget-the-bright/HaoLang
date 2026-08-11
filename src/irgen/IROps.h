// ============================================================
//  HaoLang IR 指令收口层（文本 .ll）
// ------------------------------------------------------------
//  职责：通用 LLVM 指令拼装；不含 GC/类型/AST 语义。
//  I3：仅在本层附加 `, !dbg !N`，调用方零改动。
//  缓冲与命名仍由 IREmitter 提供。
// ============================================================
#pragma once

#include "irgen/IREmitter.h"
#include "irgen/SourceLoc.h"

#include <string>

namespace hao {

class IROps {
public:
    explicit IROps(IREmitter& em) : em_(em) {}

    void setDebugEnabled(bool v) {
        debugEnabled_ = v;
        em_.setDebugEnabled(v);
    }
    void setDebugLoc(SourceLoc loc) { cur_ = std::move(loc); }
    void clearDebugLoc() { cur_ = SourceLoc{}; }
    // D4：供 setjmp/switch 等裸 emit 附加 `, !dbg !N`
    std::string dbgSuffix() {
        if (!debugEnabled_ || !cur_.valid()) return "";
        unsigned n = em_.internDILocation(cur_.line, cur_.col);
        if (n == 0) return "";
        return ", !dbg !" + std::to_string(n);
    }

    // ---- 内存 ----
    std::string emitLoad(const std::string& ty, const std::string& ptr);
    void emitStore(const std::string& ty, const std::string& val,
                   const std::string& ptr);
    std::string emitAlloca(const std::string& ty);
    std::string emitAllocaNamed(const std::string& hint, const std::string& ty);
    void emitAllocaAt(const std::string& addr, const std::string& ty);
    // D3：-g 下薄 llvm.dbg.declare（arg=0 局部，>=1 形参）
    void emitDbgDeclare(const std::string& addr, const std::string& name,
                        unsigned line, unsigned arg = 0);
    // D5：-g 下薄 llvm.dbg.value（初值/赋值可见）
    void emitDbgValue(const std::string& llvmTy, const std::string& val,
                      const std::string& name, unsigned line, unsigned arg = 0);

    // ---- 调用 / 控制流 ----
    std::string emitCall(const std::string& retTy, const std::string& callee,
                         const std::string& argsIr);
    void emitCallVoid(const std::string& callee, const std::string& argsIr);
    void emitCallTyped(const std::string& fnTy, const std::string& callee,
                       const std::string& argsIr);
    void emitBr(const std::string& label);
    void emitCondBr(const std::string& cond, const std::string& trueL,
                    const std::string& falseL);
    void emitRetVoid();
    void emitRet(const std::string& ty, const std::string& val);
    void emitUnreachable();

    // ---- 运算 / PHI / 转换 / 寻址 ----
    std::string emitBinOp(const std::string& op, const std::string& ty,
                          const std::string& lhs, const std::string& rhs);
    std::string emitICmp(const std::string& pred, const std::string& ty,
                         const std::string& lhs, const std::string& rhs);
    std::string emitFCmp(const std::string& pred, const std::string& ty,
                         const std::string& lhs, const std::string& rhs);
    std::string emitPhi(const std::string& ty, const std::string& incomingsIr);
    std::string emitSelect(const std::string& cond, const std::string& ty,
                           const std::string& tVal, const std::string& fVal);
    std::string emitCast(const std::string& op, const std::string& fromTy,
                         const std::string& val, const std::string& toTy);
    std::string emitGep(const std::string& pointeeTy, const std::string& ptr,
                        const std::string& idxTy, const std::string& idx);
    std::string emitPtrToInt(const std::string& intTy, const std::string& ptr);
    std::string emitIntToPtr(const std::string& intTy, const std::string& val);
    std::string emitExtractValue(const std::string& aggTy,
                                 const std::string& agg, int index);
    std::string emitFNeg(const std::string& ty, const std::string& val);

    IREmitter& emitter() { return em_; }
    const IREmitter& emitter() const { return em_; }

private:
    void emitInst(const std::string& inst);

    IREmitter& em_;
    SourceLoc cur_;
    bool debugEnabled_ = false;
};

}  // namespace hao
