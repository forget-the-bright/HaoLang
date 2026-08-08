
// Generated from D:/buildLang/src/ast/HaoLangParser.g4 by ANTLR 4.13.2

#pragma once


#include "antlr4-runtime.h"
#include "HaoLangParser.h"



/**
 * This class defines an abstract visitor for a parse tree
 * produced by HaoLangParser.
 */
class  HaoLangParserVisitor : public antlr4::tree::AbstractParseTreeVisitor {
public:

  /**
   * Visit parse trees produced by HaoLangParser.
   */
    virtual std::any visitCompilationUnit(HaoLangParser::CompilationUnitContext *context) = 0;

    virtual std::any visitPackageDecl(HaoLangParser::PackageDeclContext *context) = 0;

    virtual std::any visitImportDecl(HaoLangParser::ImportDeclContext *context) = 0;

    virtual std::any visitQualifiedName(HaoLangParser::QualifiedNameContext *context) = 0;

    virtual std::any visitTopLevelDecl(HaoLangParser::TopLevelDeclContext *context) = 0;

    virtual std::any visitDelegateDecl(HaoLangParser::DelegateDeclContext *context) = 0;

    virtual std::any visitEnumDecl(HaoLangParser::EnumDeclContext *context) = 0;

    virtual std::any visitEnumConstant(HaoLangParser::EnumConstantContext *context) = 0;

    virtual std::any visitFuncDecl(HaoLangParser::FuncDeclContext *context) = 0;

    virtual std::any visitLinkDecl(HaoLangParser::LinkDeclContext *context) = 0;

    virtual std::any visitReturnType(HaoLangParser::ReturnTypeContext *context) = 0;

    virtual std::any visitParamList(HaoLangParser::ParamListContext *context) = 0;

    virtual std::any visitParam(HaoLangParser::ParamContext *context) = 0;

    virtual std::any visitTypeParams(HaoLangParser::TypeParamsContext *context) = 0;

    virtual std::any visitModifier(HaoLangParser::ModifierContext *context) = 0;

    virtual std::any visitClassDecl(HaoLangParser::ClassDeclContext *context) = 0;

    virtual std::any visitColonBase(HaoLangParser::ColonBaseContext *context) = 0;

    virtual std::any visitExtendsBase(HaoLangParser::ExtendsBaseContext *context) = 0;

    virtual std::any visitImplementsBase(HaoLangParser::ImplementsBaseContext *context) = 0;

    virtual std::any visitInterfaceDecl(HaoLangParser::InterfaceDeclContext *context) = 0;

    virtual std::any visitTypeList(HaoLangParser::TypeListContext *context) = 0;

    virtual std::any visitClassMember(HaoLangParser::ClassMemberContext *context) = 0;

    virtual std::any visitStaticCtorDecl(HaoLangParser::StaticCtorDeclContext *context) = 0;

    virtual std::any visitInterfaceMember(HaoLangParser::InterfaceMemberContext *context) = 0;

    virtual std::any visitConstructorDecl(HaoLangParser::ConstructorDeclContext *context) = 0;

    virtual std::any visitPropertyDecl(HaoLangParser::PropertyDeclContext *context) = 0;

    virtual std::any visitAccessor(HaoLangParser::AccessorContext *context) = 0;

    virtual std::any visitFieldDecl(HaoLangParser::FieldDeclContext *context) = 0;

    virtual std::any visitAnnotationUse(HaoLangParser::AnnotationUseContext *context) = 0;

    virtual std::any visitAnnotationArgs(HaoLangParser::AnnotationArgsContext *context) = 0;

    virtual std::any visitAnnotationArg(HaoLangParser::AnnotationArgContext *context) = 0;

    virtual std::any visitAnnotationDecl(HaoLangParser::AnnotationDeclContext *context) = 0;

    virtual std::any visitAnnotationMember(HaoLangParser::AnnotationMemberContext *context) = 0;

    virtual std::any visitType(HaoLangParser::TypeContext *context) = 0;

    virtual std::any visitNamedType(HaoLangParser::NamedTypeContext *context) = 0;

    virtual std::any visitArrayType(HaoLangParser::ArrayTypeContext *context) = 0;

    virtual std::any visitFuncType(HaoLangParser::FuncTypeContext *context) = 0;

    virtual std::any visitTypeArgs(HaoLangParser::TypeArgsContext *context) = 0;

    virtual std::any visitBlock(HaoLangParser::BlockContext *context) = 0;

    virtual std::any visitStatement(HaoLangParser::StatementContext *context) = 0;

    virtual std::any visitHaoroutineStmt(HaoLangParser::HaoroutineStmtContext *context) = 0;

    virtual std::any visitSelectStmt(HaoLangParser::SelectStmtContext *context) = 0;

    virtual std::any visitSelectCase(HaoLangParser::SelectCaseContext *context) = 0;

    virtual std::any visitSelectComm(HaoLangParser::SelectCommContext *context) = 0;

    virtual std::any visitVarDecl(HaoLangParser::VarDeclContext *context) = 0;

    virtual std::any visitIfStmt(HaoLangParser::IfStmtContext *context) = 0;

    virtual std::any visitWhileStmt(HaoLangParser::WhileStmtContext *context) = 0;

    virtual std::any visitForStmt(HaoLangParser::ForStmtContext *context) = 0;

    virtual std::any visitWhenStmt(HaoLangParser::WhenStmtContext *context) = 0;

    virtual std::any visitWhenBranch(HaoLangParser::WhenBranchContext *context) = 0;

    virtual std::any visitReturnStmt(HaoLangParser::ReturnStmtContext *context) = 0;

    virtual std::any visitBreakStmt(HaoLangParser::BreakStmtContext *context) = 0;

    virtual std::any visitContinueStmt(HaoLangParser::ContinueStmtContext *context) = 0;

    virtual std::any visitTryStmt(HaoLangParser::TryStmtContext *context) = 0;

    virtual std::any visitCatchClause(HaoLangParser::CatchClauseContext *context) = 0;

    virtual std::any visitFinallyClause(HaoLangParser::FinallyClauseContext *context) = 0;

    virtual std::any visitThrowStmt(HaoLangParser::ThrowStmtContext *context) = 0;

    virtual std::any visitExprStmt(HaoLangParser::ExprStmtContext *context) = 0;

    virtual std::any visitExprList(HaoLangParser::ExprListContext *context) = 0;

    virtual std::any visitExpr(HaoLangParser::ExprContext *context) = 0;

    virtual std::any visitAssignExpr(HaoLangParser::AssignExprContext *context) = 0;

    virtual std::any visitAssignOp(HaoLangParser::AssignOpContext *context) = 0;

    virtual std::any visitTernaryExpr(HaoLangParser::TernaryExprContext *context) = 0;

    virtual std::any visitNullCoalesceExpr(HaoLangParser::NullCoalesceExprContext *context) = 0;

    virtual std::any visitOrExpr(HaoLangParser::OrExprContext *context) = 0;

    virtual std::any visitAndExpr(HaoLangParser::AndExprContext *context) = 0;

    virtual std::any visitBitOrExpr(HaoLangParser::BitOrExprContext *context) = 0;

    virtual std::any visitBitXorExpr(HaoLangParser::BitXorExprContext *context) = 0;

    virtual std::any visitBitAndExpr(HaoLangParser::BitAndExprContext *context) = 0;

    virtual std::any visitEqualityExpr(HaoLangParser::EqualityExprContext *context) = 0;

    virtual std::any visitRelationalExpr(HaoLangParser::RelationalExprContext *context) = 0;

    virtual std::any visitShiftExpr(HaoLangParser::ShiftExprContext *context) = 0;

    virtual std::any visitAdditiveExpr(HaoLangParser::AdditiveExprContext *context) = 0;

    virtual std::any visitMultiplicativeExpr(HaoLangParser::MultiplicativeExprContext *context) = 0;

    virtual std::any visitUnaryExpr(HaoLangParser::UnaryExprContext *context) = 0;

    virtual std::any visitPostfixExpr(HaoLangParser::PostfixExprContext *context) = 0;

    virtual std::any visitMemberAccess(HaoLangParser::MemberAccessContext *context) = 0;

    virtual std::any visitSafeMemberAccess(HaoLangParser::SafeMemberAccessContext *context) = 0;

    virtual std::any visitNotNullAssert(HaoLangParser::NotNullAssertContext *context) = 0;

    virtual std::any visitCallOp(HaoLangParser::CallOpContext *context) = 0;

    virtual std::any visitIndexOp(HaoLangParser::IndexOpContext *context) = 0;

    virtual std::any visitPostIncr(HaoLangParser::PostIncrContext *context) = 0;

    virtual std::any visitPostDecr(HaoLangParser::PostDecrContext *context) = 0;

    virtual std::any visitArgList(HaoLangParser::ArgListContext *context) = 0;

    virtual std::any visitArg(HaoLangParser::ArgContext *context) = 0;

    virtual std::any visitLitPrimary(HaoLangParser::LitPrimaryContext *context) = 0;

    virtual std::any visitIdentPrimary(HaoLangParser::IdentPrimaryContext *context) = 0;

    virtual std::any visitThisPrimary(HaoLangParser::ThisPrimaryContext *context) = 0;

    virtual std::any visitSuperPrimary(HaoLangParser::SuperPrimaryContext *context) = 0;

    virtual std::any visitWhenPrimary(HaoLangParser::WhenPrimaryContext *context) = 0;

    virtual std::any visitParenPrimary(HaoLangParser::ParenPrimaryContext *context) = 0;

    virtual std::any visitNewPrimary(HaoLangParser::NewPrimaryContext *context) = 0;

    virtual std::any visitArrayPrimary(HaoLangParser::ArrayPrimaryContext *context) = 0;

    virtual std::any visitLambdaPrimary(HaoLangParser::LambdaPrimaryContext *context) = 0;

    virtual std::any visitLambda(HaoLangParser::LambdaContext *context) = 0;

    virtual std::any visitLambdaParams(HaoLangParser::LambdaParamsContext *context) = 0;

    virtual std::any visitArrayLiteral(HaoLangParser::ArrayLiteralContext *context) = 0;

    virtual std::any visitArrayElementList(HaoLangParser::ArrayElementListContext *context) = 0;

    virtual std::any visitArrayElement(HaoLangParser::ArrayElementContext *context) = 0;

    virtual std::any visitLiteral(HaoLangParser::LiteralContext *context) = 0;

    virtual std::any visitTemplateString(HaoLangParser::TemplateStringContext *context) = 0;

    virtual std::any visitTmplText(HaoLangParser::TmplTextContext *context) = 0;

    virtual std::any visitTmplInterp(HaoLangParser::TmplInterpContext *context) = 0;


};

