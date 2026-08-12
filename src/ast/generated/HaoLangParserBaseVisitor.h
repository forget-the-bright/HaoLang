
// Generated from D:/buildLang/src/ast/HaoLangParser.g4 by ANTLR 4.13.2

#pragma once


#include "antlr4-runtime.h"
#include "HaoLangParserVisitor.h"


/**
 * This class provides an empty implementation of HaoLangParserVisitor, which can be
 * extended to create a visitor which only needs to handle a subset of the available methods.
 */
class  HaoLangParserBaseVisitor : public HaoLangParserVisitor {
public:

  virtual std::any visitCompilationUnit(HaoLangParser::CompilationUnitContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPackageDecl(HaoLangParser::PackageDeclContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitImportDecl(HaoLangParser::ImportDeclContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitQualifiedName(HaoLangParser::QualifiedNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTopLevelDecl(HaoLangParser::TopLevelDeclContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDelegateDecl(HaoLangParser::DelegateDeclContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEnumDecl(HaoLangParser::EnumDeclContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEnumConstant(HaoLangParser::EnumConstantContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFuncDecl(HaoLangParser::FuncDeclContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLinkDecl(HaoLangParser::LinkDeclContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitReturnType(HaoLangParser::ReturnTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitParamList(HaoLangParser::ParamListContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitParam(HaoLangParser::ParamContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTypeParams(HaoLangParser::TypeParamsContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitWhereClause(HaoLangParser::WhereClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitWhereBinding(HaoLangParser::WhereBindingContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitModifier(HaoLangParser::ModifierContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitClassDecl(HaoLangParser::ClassDeclContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitColonBase(HaoLangParser::ColonBaseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitExtendsBase(HaoLangParser::ExtendsBaseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitImplementsBase(HaoLangParser::ImplementsBaseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitInterfaceDecl(HaoLangParser::InterfaceDeclContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTypeList(HaoLangParser::TypeListContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitClassMember(HaoLangParser::ClassMemberContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitStaticCtorDecl(HaoLangParser::StaticCtorDeclContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitInterfaceMember(HaoLangParser::InterfaceMemberContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitConstructorDecl(HaoLangParser::ConstructorDeclContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPropertyDecl(HaoLangParser::PropertyDeclContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitAccessor(HaoLangParser::AccessorContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFieldDecl(HaoLangParser::FieldDeclContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitAnnotationUse(HaoLangParser::AnnotationUseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitAnnotationArgs(HaoLangParser::AnnotationArgsContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitAnnotationArg(HaoLangParser::AnnotationArgContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitAnnotationDecl(HaoLangParser::AnnotationDeclContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitAnnotationMember(HaoLangParser::AnnotationMemberContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitType(HaoLangParser::TypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNamedType(HaoLangParser::NamedTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitArrayType(HaoLangParser::ArrayTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFuncType(HaoLangParser::FuncTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTypeArg(HaoLangParser::TypeArgContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTypeArgs(HaoLangParser::TypeArgsContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitBlock(HaoLangParser::BlockContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitStatement(HaoLangParser::StatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitHaoroutineStmt(HaoLangParser::HaoroutineStmtContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSelectStmt(HaoLangParser::SelectStmtContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSelectCase(HaoLangParser::SelectCaseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSelectComm(HaoLangParser::SelectCommContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitVarDecl(HaoLangParser::VarDeclContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitIfStmt(HaoLangParser::IfStmtContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitWhileStmt(HaoLangParser::WhileStmtContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitForStmt(HaoLangParser::ForStmtContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitWhenStmt(HaoLangParser::WhenStmtContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitWhenBranch(HaoLangParser::WhenBranchContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitReturnStmt(HaoLangParser::ReturnStmtContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitBreakStmt(HaoLangParser::BreakStmtContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitContinueStmt(HaoLangParser::ContinueStmtContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTryStmt(HaoLangParser::TryStmtContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCatchClause(HaoLangParser::CatchClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFinallyClause(HaoLangParser::FinallyClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitThrowStmt(HaoLangParser::ThrowStmtContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitExprStmt(HaoLangParser::ExprStmtContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitExprList(HaoLangParser::ExprListContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitExpr(HaoLangParser::ExprContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitAssignExpr(HaoLangParser::AssignExprContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitAssignOp(HaoLangParser::AssignOpContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTernaryExpr(HaoLangParser::TernaryExprContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNullCoalesceExpr(HaoLangParser::NullCoalesceExprContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitOrExpr(HaoLangParser::OrExprContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitAndExpr(HaoLangParser::AndExprContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitBitOrExpr(HaoLangParser::BitOrExprContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitBitXorExpr(HaoLangParser::BitXorExprContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitBitAndExpr(HaoLangParser::BitAndExprContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEqualityExpr(HaoLangParser::EqualityExprContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitRelationalExpr(HaoLangParser::RelationalExprContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitShiftExpr(HaoLangParser::ShiftExprContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitAdditiveExpr(HaoLangParser::AdditiveExprContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitMultiplicativeExpr(HaoLangParser::MultiplicativeExprContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitUnaryExpr(HaoLangParser::UnaryExprContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPostfixExpr(HaoLangParser::PostfixExprContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitMemberAccess(HaoLangParser::MemberAccessContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSafeMemberAccess(HaoLangParser::SafeMemberAccessContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNotNullAssert(HaoLangParser::NotNullAssertContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCallOp(HaoLangParser::CallOpContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitIndexOp(HaoLangParser::IndexOpContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPostIncr(HaoLangParser::PostIncrContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPostDecr(HaoLangParser::PostDecrContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitArgList(HaoLangParser::ArgListContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitArg(HaoLangParser::ArgContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLitPrimary(HaoLangParser::LitPrimaryContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitIdentPrimary(HaoLangParser::IdentPrimaryContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitThisPrimary(HaoLangParser::ThisPrimaryContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSuperPrimary(HaoLangParser::SuperPrimaryContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitWhenPrimary(HaoLangParser::WhenPrimaryContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitParenPrimary(HaoLangParser::ParenPrimaryContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNewPrimary(HaoLangParser::NewPrimaryContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNewArrayInitPrimary(HaoLangParser::NewArrayInitPrimaryContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLambdaPrimary(HaoLangParser::LambdaPrimaryContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLambda(HaoLangParser::LambdaContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLambdaParams(HaoLangParser::LambdaParamsContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitArrayElementList(HaoLangParser::ArrayElementListContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitArrayElement(HaoLangParser::ArrayElementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLiteral(HaoLangParser::LiteralContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTemplateString(HaoLangParser::TemplateStringContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTmplText(HaoLangParser::TmplTextContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTmplInterp(HaoLangParser::TmplInterpContext *ctx) override {
    return visitChildren(ctx);
  }


};

