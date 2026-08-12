
// Generated from D:/buildLang/src/ast/HaoLangParser.g4 by ANTLR 4.13.2

#pragma once


#include "antlr4-runtime.h"




class  HaoLangParser : public antlr4::Parser {
public:
  enum {
    PACKAGE = 1, IMPORT = 2, EXTERN = 3, FUNC = 4, CLASS = 5, INTERFACE = 6, 
    ENUM = 7, EXTENDS = 8, IMPLEMENTS = 9, CONSTRUCTOR = 10, VAL = 11, VAR = 12, 
    NEW = 13, PUBLIC = 14, PRIVATE = 15, PROTECTED = 16, INTERNAL = 17, 
    STATIC = 18, ABSTRACT = 19, OVERRIDE = 20, VIRTUAL = 21, ASYNC = 22, 
    DELEGATE = 23, IF = 24, ELSE = 25, WHILE = 26, FOR = 27, IN = 28, WHEN = 29, 
    RETURN = 30, BREAK = 31, CONTINUE = 32, TRY = 33, CATCH = 34, FINALLY = 35, 
    THROW = 36, HAOROUTINE = 37, SELECT = 38, CASE = 39, DEFAULT = 40, THIS = 41, 
    SUPER = 42, IS = 43, AS = 44, TRUE = 45, FALSE = 46, NULL_LIT = 47, 
    WHERE = 48, QQ_ASSIGN = 49, QQ = 50, ARROW = 51, PLUS_ASSIGN = 52, MINUS_ASSIGN = 53, 
    STAR_ASSIGN = 54, SLASH_ASSIGN = 55, PCT_ASSIGN = 56, AMP_ASSIGN = 57, 
    PIPE_ASSIGN = 58, CARET_ASSIGN = 59, LSHIFT_ASSIGN = 60, RSHIFT_ASSIGN = 61, 
    INCR = 62, DECR = 63, EQ = 64, NEQ = 65, LE = 66, GE = 67, LSHIFT = 68, 
    AND_AND = 69, OR_OR = 70, ASSIGN = 71, PLUS = 72, MINUS = 73, STAR = 74, 
    SLASH = 75, PCT = 76, BANG = 77, TILDE = 78, AMP = 79, PIPE = 80, CARET = 81, 
    LT = 82, GT = 83, QUESTION = 84, COLON = 85, SEMI = 86, COMMA = 87, 
    ELLIPSIS = 88, DOT = 89, VERBATIM_TEMPLATE_START = 90, VERBATIM_STRING = 91, 
    AT = 92, LPAREN = 93, RPAREN = 94, LBRACK = 95, RBRACK = 96, LBRACE = 97, 
    RBRACE = 98, TEMPLATE_START = 99, STRING_LIT = 100, CHAR_LIT = 101, 
    FLOAT_LIT = 102, INT_LIT = 103, IDENT = 104, LINE_COMMENT = 105, BLOCK_COMMENT = 106, 
    WS = 107, UNKNOWN_CHAR = 108, TEMPLATE_END = 109, TEMPLATE_INTERP_START = 110, 
    TEMPLATE_TEXT = 111
  };

  enum {
    RuleCompilationUnit = 0, RulePackageDecl = 1, RuleImportDecl = 2, RuleQualifiedName = 3, 
    RuleTopLevelDecl = 4, RuleDelegateDecl = 5, RuleEnumDecl = 6, RuleEnumConstant = 7, 
    RuleFuncDecl = 8, RuleLinkDecl = 9, RuleReturnType = 10, RuleParamList = 11, 
    RuleParam = 12, RuleTypeParams = 13, RuleWhereClause = 14, RuleWhereBinding = 15, 
    RuleModifier = 16, RuleClassDecl = 17, RuleClassBase = 18, RuleInterfaceDecl = 19, 
    RuleTypeList = 20, RuleClassMember = 21, RuleStaticCtorDecl = 22, RuleInterfaceMember = 23, 
    RuleConstructorDecl = 24, RulePropertyDecl = 25, RuleAccessor = 26, 
    RuleFieldDecl = 27, RuleAnnotationUse = 28, RuleAnnotationArgs = 29, 
    RuleAnnotationArg = 30, RuleAnnotationDecl = 31, RuleAnnotationMember = 32, 
    RuleType = 33, RuleBaseType = 34, RuleTypeArg = 35, RuleTypeArgs = 36, 
    RuleBlock = 37, RuleStatement = 38, RuleHaoroutineStmt = 39, RuleSelectStmt = 40, 
    RuleSelectCase = 41, RuleSelectComm = 42, RuleVarDecl = 43, RuleIfStmt = 44, 
    RuleWhileStmt = 45, RuleForStmt = 46, RuleWhenStmt = 47, RuleWhenBranch = 48, 
    RuleReturnStmt = 49, RuleBreakStmt = 50, RuleContinueStmt = 51, RuleTryStmt = 52, 
    RuleCatchClause = 53, RuleFinallyClause = 54, RuleThrowStmt = 55, RuleExprStmt = 56, 
    RuleExprList = 57, RuleExpr = 58, RuleAssignExpr = 59, RuleAssignOp = 60, 
    RuleTernaryExpr = 61, RuleNullCoalesceExpr = 62, RuleOrExpr = 63, RuleAndExpr = 64, 
    RuleBitOrExpr = 65, RuleBitXorExpr = 66, RuleBitAndExpr = 67, RuleEqualityExpr = 68, 
    RuleRelationalExpr = 69, RuleShiftExpr = 70, RuleAdditiveExpr = 71, 
    RuleMultiplicativeExpr = 72, RuleUnaryExpr = 73, RulePostfixExpr = 74, 
    RulePostfixOp = 75, RuleArgList = 76, RuleArg = 77, RulePrimary = 78, 
    RuleLambda = 79, RuleLambdaParams = 80, RuleArrayElementList = 81, RuleArrayElement = 82, 
    RuleLiteral = 83, RuleTemplateString = 84, RuleTemplatePart = 85
  };

  explicit HaoLangParser(antlr4::TokenStream *input);

  HaoLangParser(antlr4::TokenStream *input, const antlr4::atn::ParserATNSimulatorOptions &options);

  ~HaoLangParser() override;

  std::string getGrammarFileName() const override;

  const antlr4::atn::ATN& getATN() const override;

  const std::vector<std::string>& getRuleNames() const override;

  const antlr4::dfa::Vocabulary& getVocabulary() const override;

  antlr4::atn::SerializedATNView getSerializedATN() const override;


  class CompilationUnitContext;
  class PackageDeclContext;
  class ImportDeclContext;
  class QualifiedNameContext;
  class TopLevelDeclContext;
  class DelegateDeclContext;
  class EnumDeclContext;
  class EnumConstantContext;
  class FuncDeclContext;
  class LinkDeclContext;
  class ReturnTypeContext;
  class ParamListContext;
  class ParamContext;
  class TypeParamsContext;
  class WhereClauseContext;
  class WhereBindingContext;
  class ModifierContext;
  class ClassDeclContext;
  class ClassBaseContext;
  class InterfaceDeclContext;
  class TypeListContext;
  class ClassMemberContext;
  class StaticCtorDeclContext;
  class InterfaceMemberContext;
  class ConstructorDeclContext;
  class PropertyDeclContext;
  class AccessorContext;
  class FieldDeclContext;
  class AnnotationUseContext;
  class AnnotationArgsContext;
  class AnnotationArgContext;
  class AnnotationDeclContext;
  class AnnotationMemberContext;
  class TypeContext;
  class BaseTypeContext;
  class TypeArgContext;
  class TypeArgsContext;
  class BlockContext;
  class StatementContext;
  class HaoroutineStmtContext;
  class SelectStmtContext;
  class SelectCaseContext;
  class SelectCommContext;
  class VarDeclContext;
  class IfStmtContext;
  class WhileStmtContext;
  class ForStmtContext;
  class WhenStmtContext;
  class WhenBranchContext;
  class ReturnStmtContext;
  class BreakStmtContext;
  class ContinueStmtContext;
  class TryStmtContext;
  class CatchClauseContext;
  class FinallyClauseContext;
  class ThrowStmtContext;
  class ExprStmtContext;
  class ExprListContext;
  class ExprContext;
  class AssignExprContext;
  class AssignOpContext;
  class TernaryExprContext;
  class NullCoalesceExprContext;
  class OrExprContext;
  class AndExprContext;
  class BitOrExprContext;
  class BitXorExprContext;
  class BitAndExprContext;
  class EqualityExprContext;
  class RelationalExprContext;
  class ShiftExprContext;
  class AdditiveExprContext;
  class MultiplicativeExprContext;
  class UnaryExprContext;
  class PostfixExprContext;
  class PostfixOpContext;
  class ArgListContext;
  class ArgContext;
  class PrimaryContext;
  class LambdaContext;
  class LambdaParamsContext;
  class ArrayElementListContext;
  class ArrayElementContext;
  class LiteralContext;
  class TemplateStringContext;
  class TemplatePartContext; 

  class  CompilationUnitContext : public antlr4::ParserRuleContext {
  public:
    CompilationUnitContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *EOF();
    PackageDeclContext *packageDecl();
    std::vector<ImportDeclContext *> importDecl();
    ImportDeclContext* importDecl(size_t i);
    std::vector<TopLevelDeclContext *> topLevelDecl();
    TopLevelDeclContext* topLevelDecl(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  CompilationUnitContext* compilationUnit();

  class  PackageDeclContext : public antlr4::ParserRuleContext {
  public:
    PackageDeclContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *PACKAGE();
    QualifiedNameContext *qualifiedName();
    antlr4::tree::TerminalNode *SEMI();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  PackageDeclContext* packageDecl();

  class  ImportDeclContext : public antlr4::ParserRuleContext {
  public:
    ImportDeclContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *IMPORT();
    QualifiedNameContext *qualifiedName();
    antlr4::tree::TerminalNode *DOT();
    antlr4::tree::TerminalNode *STAR();
    antlr4::tree::TerminalNode *AS();
    antlr4::tree::TerminalNode *IDENT();
    antlr4::tree::TerminalNode *SEMI();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ImportDeclContext* importDecl();

  class  QualifiedNameContext : public antlr4::ParserRuleContext {
  public:
    QualifiedNameContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<antlr4::tree::TerminalNode *> IDENT();
    antlr4::tree::TerminalNode* IDENT(size_t i);
    std::vector<antlr4::tree::TerminalNode *> DOT();
    antlr4::tree::TerminalNode* DOT(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  QualifiedNameContext* qualifiedName();

  class  TopLevelDeclContext : public antlr4::ParserRuleContext {
  public:
    TopLevelDeclContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    FuncDeclContext *funcDecl();
    ClassDeclContext *classDecl();
    InterfaceDeclContext *interfaceDecl();
    EnumDeclContext *enumDecl();
    FieldDeclContext *fieldDecl();
    DelegateDeclContext *delegateDecl();
    AnnotationDeclContext *annotationDecl();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  TopLevelDeclContext* topLevelDecl();

  class  DelegateDeclContext : public antlr4::ParserRuleContext {
  public:
    DelegateDeclContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *DELEGATE();
    TypeContext *type();
    antlr4::tree::TerminalNode *IDENT();
    antlr4::tree::TerminalNode *SEMI();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  DelegateDeclContext* delegateDecl();

  class  EnumDeclContext : public antlr4::ParserRuleContext {
  public:
    EnumDeclContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *ENUM();
    antlr4::tree::TerminalNode *IDENT();
    antlr4::tree::TerminalNode *LBRACE();
    antlr4::tree::TerminalNode *RBRACE();
    std::vector<ModifierContext *> modifier();
    ModifierContext* modifier(size_t i);
    std::vector<EnumConstantContext *> enumConstant();
    EnumConstantContext* enumConstant(size_t i);
    std::vector<antlr4::tree::TerminalNode *> COMMA();
    antlr4::tree::TerminalNode* COMMA(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  EnumDeclContext* enumDecl();

  class  EnumConstantContext : public antlr4::ParserRuleContext {
  public:
    EnumConstantContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *IDENT();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  EnumConstantContext* enumConstant();

  class  FuncDeclContext : public antlr4::ParserRuleContext {
  public:
    FuncDeclContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *FUNC();
    antlr4::tree::TerminalNode *IDENT();
    antlr4::tree::TerminalNode *LPAREN();
    antlr4::tree::TerminalNode *RPAREN();
    BlockContext *block();
    antlr4::tree::TerminalNode *SEMI();
    std::vector<AnnotationUseContext *> annotationUse();
    AnnotationUseContext* annotationUse(size_t i);
    std::vector<ModifierContext *> modifier();
    ModifierContext* modifier(size_t i);
    TypeParamsContext *typeParams();
    ParamListContext *paramList();
    ReturnTypeContext *returnType();
    WhereClauseContext *whereClause();
    antlr4::tree::TerminalNode *ASSIGN();
    antlr4::tree::TerminalNode *STRING_LIT();
    LinkDeclContext *linkDecl();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  FuncDeclContext* funcDecl();

  class  LinkDeclContext : public antlr4::ParserRuleContext {
  public:
    LinkDeclContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *AT();
    antlr4::tree::TerminalNode *IDENT();
    antlr4::tree::TerminalNode *LPAREN();
    antlr4::tree::TerminalNode *RPAREN();
    std::vector<antlr4::tree::TerminalNode *> STRING_LIT();
    antlr4::tree::TerminalNode* STRING_LIT(size_t i);
    std::vector<antlr4::tree::TerminalNode *> COMMA();
    antlr4::tree::TerminalNode* COMMA(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  LinkDeclContext* linkDecl();

  class  ReturnTypeContext : public antlr4::ParserRuleContext {
  public:
    ReturnTypeContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *COLON();
    TypeContext *type();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ReturnTypeContext* returnType();

  class  ParamListContext : public antlr4::ParserRuleContext {
  public:
    ParamListContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<ParamContext *> param();
    ParamContext* param(size_t i);
    std::vector<antlr4::tree::TerminalNode *> COMMA();
    antlr4::tree::TerminalNode* COMMA(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ParamListContext* paramList();

  class  ParamContext : public antlr4::ParserRuleContext {
  public:
    ParamContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *IDENT();
    antlr4::tree::TerminalNode *COLON();
    TypeContext *type();
    antlr4::tree::TerminalNode *ELLIPSIS();
    antlr4::tree::TerminalNode *ASSIGN();
    ExprContext *expr();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ParamContext* param();

  class  TypeParamsContext : public antlr4::ParserRuleContext {
  public:
    TypeParamsContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *LT();
    std::vector<antlr4::tree::TerminalNode *> IDENT();
    antlr4::tree::TerminalNode* IDENT(size_t i);
    antlr4::tree::TerminalNode *GT();
    std::vector<antlr4::tree::TerminalNode *> COMMA();
    antlr4::tree::TerminalNode* COMMA(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  TypeParamsContext* typeParams();

  class  WhereClauseContext : public antlr4::ParserRuleContext {
  public:
    WhereClauseContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *WHERE();
    std::vector<WhereBindingContext *> whereBinding();
    WhereBindingContext* whereBinding(size_t i);
    std::vector<antlr4::tree::TerminalNode *> COMMA();
    antlr4::tree::TerminalNode* COMMA(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  WhereClauseContext* whereClause();

  class  WhereBindingContext : public antlr4::ParserRuleContext {
  public:
    WhereBindingContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *IDENT();
    antlr4::tree::TerminalNode *COLON();
    TypeContext *type();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  WhereBindingContext* whereBinding();

  class  ModifierContext : public antlr4::ParserRuleContext {
  public:
    ModifierContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *PUBLIC();
    antlr4::tree::TerminalNode *PRIVATE();
    antlr4::tree::TerminalNode *PROTECTED();
    antlr4::tree::TerminalNode *INTERNAL();
    antlr4::tree::TerminalNode *STATIC();
    antlr4::tree::TerminalNode *ABSTRACT();
    antlr4::tree::TerminalNode *OVERRIDE();
    antlr4::tree::TerminalNode *VIRTUAL();
    antlr4::tree::TerminalNode *ASYNC();
    antlr4::tree::TerminalNode *EXTERN();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ModifierContext* modifier();

  class  ClassDeclContext : public antlr4::ParserRuleContext {
  public:
    ClassDeclContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *CLASS();
    antlr4::tree::TerminalNode *IDENT();
    antlr4::tree::TerminalNode *LBRACE();
    antlr4::tree::TerminalNode *RBRACE();
    std::vector<AnnotationUseContext *> annotationUse();
    AnnotationUseContext* annotationUse(size_t i);
    std::vector<ModifierContext *> modifier();
    ModifierContext* modifier(size_t i);
    TypeParamsContext *typeParams();
    ClassBaseContext *classBase();
    WhereClauseContext *whereClause();
    std::vector<ClassMemberContext *> classMember();
    ClassMemberContext* classMember(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ClassDeclContext* classDecl();

  class  ClassBaseContext : public antlr4::ParserRuleContext {
  public:
    ClassBaseContext(antlr4::ParserRuleContext *parent, size_t invokingState);
   
    ClassBaseContext() = default;
    void copyFrom(ClassBaseContext *context);
    using antlr4::ParserRuleContext::copyFrom;

    virtual size_t getRuleIndex() const override;

   
  };

  class  ColonBaseContext : public ClassBaseContext {
  public:
    ColonBaseContext(ClassBaseContext *ctx);

    antlr4::tree::TerminalNode *COLON();
    TypeListContext *typeList();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  class  ExtendsBaseContext : public ClassBaseContext {
  public:
    ExtendsBaseContext(ClassBaseContext *ctx);

    antlr4::tree::TerminalNode *EXTENDS();
    TypeContext *type();
    antlr4::tree::TerminalNode *IMPLEMENTS();
    TypeListContext *typeList();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  class  ImplementsBaseContext : public ClassBaseContext {
  public:
    ImplementsBaseContext(ClassBaseContext *ctx);

    antlr4::tree::TerminalNode *IMPLEMENTS();
    TypeListContext *typeList();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  ClassBaseContext* classBase();

  class  InterfaceDeclContext : public antlr4::ParserRuleContext {
  public:
    InterfaceDeclContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *INTERFACE();
    antlr4::tree::TerminalNode *IDENT();
    antlr4::tree::TerminalNode *LBRACE();
    antlr4::tree::TerminalNode *RBRACE();
    std::vector<ModifierContext *> modifier();
    ModifierContext* modifier(size_t i);
    TypeParamsContext *typeParams();
    antlr4::tree::TerminalNode *COLON();
    TypeListContext *typeList();
    WhereClauseContext *whereClause();
    std::vector<InterfaceMemberContext *> interfaceMember();
    InterfaceMemberContext* interfaceMember(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  InterfaceDeclContext* interfaceDecl();

  class  TypeListContext : public antlr4::ParserRuleContext {
  public:
    TypeListContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<TypeContext *> type();
    TypeContext* type(size_t i);
    std::vector<antlr4::tree::TerminalNode *> COMMA();
    antlr4::tree::TerminalNode* COMMA(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  TypeListContext* typeList();

  class  ClassMemberContext : public antlr4::ParserRuleContext {
  public:
    ClassMemberContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    FuncDeclContext *funcDecl();
    ConstructorDeclContext *constructorDecl();
    StaticCtorDeclContext *staticCtorDecl();
    PropertyDeclContext *propertyDecl();
    FieldDeclContext *fieldDecl();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ClassMemberContext* classMember();

  class  StaticCtorDeclContext : public antlr4::ParserRuleContext {
  public:
    StaticCtorDeclContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *STATIC();
    antlr4::tree::TerminalNode *IDENT();
    antlr4::tree::TerminalNode *LPAREN();
    antlr4::tree::TerminalNode *RPAREN();
    BlockContext *block();
    ParamListContext *paramList();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  StaticCtorDeclContext* staticCtorDecl();

  class  InterfaceMemberContext : public antlr4::ParserRuleContext {
  public:
    InterfaceMemberContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *FUNC();
    antlr4::tree::TerminalNode *IDENT();
    antlr4::tree::TerminalNode *LPAREN();
    antlr4::tree::TerminalNode *RPAREN();
    BlockContext *block();
    std::vector<ModifierContext *> modifier();
    ModifierContext* modifier(size_t i);
    TypeParamsContext *typeParams();
    ParamListContext *paramList();
    ReturnTypeContext *returnType();
    WhereClauseContext *whereClause();
    antlr4::tree::TerminalNode *SEMI();
    PropertyDeclContext *propertyDecl();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  InterfaceMemberContext* interfaceMember();

  class  ConstructorDeclContext : public antlr4::ParserRuleContext {
  public:
    ConstructorDeclContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *CONSTRUCTOR();
    antlr4::tree::TerminalNode *LPAREN();
    antlr4::tree::TerminalNode *RPAREN();
    BlockContext *block();
    std::vector<ModifierContext *> modifier();
    ModifierContext* modifier(size_t i);
    ParamListContext *paramList();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ConstructorDeclContext* constructorDecl();

  class  PropertyDeclContext : public antlr4::ParserRuleContext {
  public:
    PropertyDeclContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<antlr4::tree::TerminalNode *> IDENT();
    antlr4::tree::TerminalNode* IDENT(size_t i);
    antlr4::tree::TerminalNode *COLON();
    TypeContext *type();
    antlr4::tree::TerminalNode *LBRACE();
    antlr4::tree::TerminalNode *RBRACE();
    antlr4::tree::TerminalNode *VAL();
    antlr4::tree::TerminalNode *VAR();
    std::vector<ModifierContext *> modifier();
    ModifierContext* modifier(size_t i);
    std::vector<AccessorContext *> accessor();
    AccessorContext* accessor(size_t i);
    antlr4::tree::TerminalNode *ASSIGN();
    ExprContext *expr();
    antlr4::tree::TerminalNode *LPAREN();
    antlr4::tree::TerminalNode *RPAREN();
    antlr4::tree::TerminalNode *SEMI();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  PropertyDeclContext* propertyDecl();

  class  AccessorContext : public antlr4::ParserRuleContext {
  public:
    AccessorContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *IDENT();
    BlockContext *block();
    antlr4::tree::TerminalNode *SEMI();
    std::vector<ModifierContext *> modifier();
    ModifierContext* modifier(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  AccessorContext* accessor();

  class  FieldDeclContext : public antlr4::ParserRuleContext {
  public:
    FieldDeclContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *IDENT();
    antlr4::tree::TerminalNode *VAL();
    antlr4::tree::TerminalNode *VAR();
    std::vector<AnnotationUseContext *> annotationUse();
    AnnotationUseContext* annotationUse(size_t i);
    std::vector<ModifierContext *> modifier();
    ModifierContext* modifier(size_t i);
    antlr4::tree::TerminalNode *COLON();
    TypeContext *type();
    antlr4::tree::TerminalNode *ASSIGN();
    ExprContext *expr();
    antlr4::tree::TerminalNode *SEMI();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  FieldDeclContext* fieldDecl();

  class  AnnotationUseContext : public antlr4::ParserRuleContext {
  public:
    AnnotationUseContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *AT();
    QualifiedNameContext *qualifiedName();
    antlr4::tree::TerminalNode *LPAREN();
    antlr4::tree::TerminalNode *RPAREN();
    AnnotationArgsContext *annotationArgs();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  AnnotationUseContext* annotationUse();

  class  AnnotationArgsContext : public antlr4::ParserRuleContext {
  public:
    AnnotationArgsContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<AnnotationArgContext *> annotationArg();
    AnnotationArgContext* annotationArg(size_t i);
    std::vector<antlr4::tree::TerminalNode *> COMMA();
    antlr4::tree::TerminalNode* COMMA(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  AnnotationArgsContext* annotationArgs();

  class  AnnotationArgContext : public antlr4::ParserRuleContext {
  public:
    AnnotationArgContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    ExprContext *expr();
    antlr4::tree::TerminalNode *IDENT();
    antlr4::tree::TerminalNode *ASSIGN();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  AnnotationArgContext* annotationArg();

  class  AnnotationDeclContext : public antlr4::ParserRuleContext {
  public:
    AnnotationDeclContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *AT();
    antlr4::tree::TerminalNode *INTERFACE();
    antlr4::tree::TerminalNode *IDENT();
    antlr4::tree::TerminalNode *LBRACE();
    antlr4::tree::TerminalNode *RBRACE();
    std::vector<AnnotationMemberContext *> annotationMember();
    AnnotationMemberContext* annotationMember(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  AnnotationDeclContext* annotationDecl();

  class  AnnotationMemberContext : public antlr4::ParserRuleContext {
  public:
    AnnotationMemberContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *IDENT();
    antlr4::tree::TerminalNode *VAL();
    antlr4::tree::TerminalNode *VAR();
    std::vector<ModifierContext *> modifier();
    ModifierContext* modifier(size_t i);
    antlr4::tree::TerminalNode *COLON();
    TypeContext *type();
    antlr4::tree::TerminalNode *ASSIGN();
    ExprContext *expr();
    antlr4::tree::TerminalNode *SEMI();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  AnnotationMemberContext* annotationMember();

  class  TypeContext : public antlr4::ParserRuleContext {
  public:
    TypeContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    BaseTypeContext *baseType();
    antlr4::tree::TerminalNode *QUESTION();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  TypeContext* type();

  class  BaseTypeContext : public antlr4::ParserRuleContext {
  public:
    BaseTypeContext(antlr4::ParserRuleContext *parent, size_t invokingState);
   
    BaseTypeContext() = default;
    void copyFrom(BaseTypeContext *context);
    using antlr4::ParserRuleContext::copyFrom;

    virtual size_t getRuleIndex() const override;

   
  };

  class  NamedTypeContext : public BaseTypeContext {
  public:
    NamedTypeContext(BaseTypeContext *ctx);

    QualifiedNameContext *qualifiedName();
    TypeArgsContext *typeArgs();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  class  ArrayTypeContext : public BaseTypeContext {
  public:
    ArrayTypeContext(BaseTypeContext *ctx);

    antlr4::tree::TerminalNode *LBRACK();
    TypeContext *type();
    antlr4::tree::TerminalNode *RBRACK();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  class  FuncTypeContext : public BaseTypeContext {
  public:
    FuncTypeContext(BaseTypeContext *ctx);

    antlr4::tree::TerminalNode *LPAREN();
    antlr4::tree::TerminalNode *RPAREN();
    antlr4::tree::TerminalNode *ARROW();
    TypeContext *type();
    TypeListContext *typeList();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  BaseTypeContext* baseType();

  class  TypeArgContext : public antlr4::ParserRuleContext {
  public:
    TypeArgContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    TypeContext *type();
    antlr4::tree::TerminalNode *QUESTION();
    antlr4::tree::TerminalNode *EXTENDS();
    antlr4::tree::TerminalNode *SUPER();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  TypeArgContext* typeArg();

  class  TypeArgsContext : public antlr4::ParserRuleContext {
  public:
    TypeArgsContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *LT();
    std::vector<TypeArgContext *> typeArg();
    TypeArgContext* typeArg(size_t i);
    antlr4::tree::TerminalNode *GT();
    std::vector<antlr4::tree::TerminalNode *> COMMA();
    antlr4::tree::TerminalNode* COMMA(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  TypeArgsContext* typeArgs();

  class  BlockContext : public antlr4::ParserRuleContext {
  public:
    BlockContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *LBRACE();
    antlr4::tree::TerminalNode *RBRACE();
    std::vector<StatementContext *> statement();
    StatementContext* statement(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  BlockContext* block();

  class  StatementContext : public antlr4::ParserRuleContext {
  public:
    StatementContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    VarDeclContext *varDecl();
    IfStmtContext *ifStmt();
    WhileStmtContext *whileStmt();
    ForStmtContext *forStmt();
    WhenStmtContext *whenStmt();
    TryStmtContext *tryStmt();
    ThrowStmtContext *throwStmt();
    HaoroutineStmtContext *haoroutineStmt();
    SelectStmtContext *selectStmt();
    ReturnStmtContext *returnStmt();
    BreakStmtContext *breakStmt();
    ContinueStmtContext *continueStmt();
    BlockContext *block();
    ExprStmtContext *exprStmt();
    antlr4::tree::TerminalNode *SEMI();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  StatementContext* statement();

  class  HaoroutineStmtContext : public antlr4::ParserRuleContext {
  public:
    HaoroutineStmtContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *HAOROUTINE();
    LambdaContext *lambda();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  HaoroutineStmtContext* haoroutineStmt();

  class  SelectStmtContext : public antlr4::ParserRuleContext {
  public:
    SelectStmtContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *SELECT();
    antlr4::tree::TerminalNode *LBRACE();
    antlr4::tree::TerminalNode *RBRACE();
    std::vector<SelectCaseContext *> selectCase();
    SelectCaseContext* selectCase(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  SelectStmtContext* selectStmt();

  class  SelectCaseContext : public antlr4::ParserRuleContext {
  public:
    SelectCaseContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *CASE();
    SelectCommContext *selectComm();
    antlr4::tree::TerminalNode *COLON();
    std::vector<StatementContext *> statement();
    StatementContext* statement(size_t i);
    antlr4::tree::TerminalNode *DEFAULT();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  SelectCaseContext* selectCase();

  class  SelectCommContext : public antlr4::ParserRuleContext {
  public:
    SelectCommContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<antlr4::tree::TerminalNode *> IDENT();
    antlr4::tree::TerminalNode* IDENT(size_t i);
    antlr4::tree::TerminalNode *ASSIGN();
    std::vector<ExprContext *> expr();
    ExprContext* expr(size_t i);
    antlr4::tree::TerminalNode *DOT();
    antlr4::tree::TerminalNode *LPAREN();
    antlr4::tree::TerminalNode *RPAREN();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  SelectCommContext* selectComm();

  class  VarDeclContext : public antlr4::ParserRuleContext {
  public:
    VarDeclContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *IDENT();
    antlr4::tree::TerminalNode *VAL();
    antlr4::tree::TerminalNode *VAR();
    antlr4::tree::TerminalNode *COLON();
    TypeContext *type();
    antlr4::tree::TerminalNode *ASSIGN();
    ExprContext *expr();
    antlr4::tree::TerminalNode *SEMI();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  VarDeclContext* varDecl();

  class  IfStmtContext : public antlr4::ParserRuleContext {
  public:
    IfStmtContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *IF();
    antlr4::tree::TerminalNode *LPAREN();
    ExprContext *expr();
    antlr4::tree::TerminalNode *RPAREN();
    std::vector<StatementContext *> statement();
    StatementContext* statement(size_t i);
    antlr4::tree::TerminalNode *ELSE();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  IfStmtContext* ifStmt();

  class  WhileStmtContext : public antlr4::ParserRuleContext {
  public:
    WhileStmtContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *WHILE();
    antlr4::tree::TerminalNode *LPAREN();
    ExprContext *expr();
    antlr4::tree::TerminalNode *RPAREN();
    StatementContext *statement();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  WhileStmtContext* whileStmt();

  class  ForStmtContext : public antlr4::ParserRuleContext {
  public:
    ForStmtContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *FOR();
    antlr4::tree::TerminalNode *LPAREN();
    antlr4::tree::TerminalNode *IDENT();
    antlr4::tree::TerminalNode *IN();
    ExprContext *expr();
    antlr4::tree::TerminalNode *RPAREN();
    StatementContext *statement();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ForStmtContext* forStmt();

  class  WhenStmtContext : public antlr4::ParserRuleContext {
  public:
    WhenStmtContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *WHEN();
    antlr4::tree::TerminalNode *LBRACE();
    antlr4::tree::TerminalNode *RBRACE();
    antlr4::tree::TerminalNode *LPAREN();
    ExprContext *expr();
    antlr4::tree::TerminalNode *RPAREN();
    std::vector<WhenBranchContext *> whenBranch();
    WhenBranchContext* whenBranch(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  WhenStmtContext* whenStmt();

  class  WhenBranchContext : public antlr4::ParserRuleContext {
  public:
    WhenBranchContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    ExprListContext *exprList();
    antlr4::tree::TerminalNode *ARROW();
    BlockContext *block();
    ExprContext *expr();
    antlr4::tree::TerminalNode *SEMI();
    antlr4::tree::TerminalNode *ELSE();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  WhenBranchContext* whenBranch();

  class  ReturnStmtContext : public antlr4::ParserRuleContext {
  public:
    ReturnStmtContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *RETURN();
    ExprContext *expr();
    antlr4::tree::TerminalNode *SEMI();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ReturnStmtContext* returnStmt();

  class  BreakStmtContext : public antlr4::ParserRuleContext {
  public:
    BreakStmtContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *BREAK();
    antlr4::tree::TerminalNode *SEMI();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  BreakStmtContext* breakStmt();

  class  ContinueStmtContext : public antlr4::ParserRuleContext {
  public:
    ContinueStmtContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *CONTINUE();
    antlr4::tree::TerminalNode *SEMI();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ContinueStmtContext* continueStmt();

  class  TryStmtContext : public antlr4::ParserRuleContext {
  public:
    TryStmtContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *TRY();
    BlockContext *block();
    std::vector<CatchClauseContext *> catchClause();
    CatchClauseContext* catchClause(size_t i);
    FinallyClauseContext *finallyClause();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  TryStmtContext* tryStmt();

  class  CatchClauseContext : public antlr4::ParserRuleContext {
  public:
    CatchClauseContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *CATCH();
    antlr4::tree::TerminalNode *LPAREN();
    antlr4::tree::TerminalNode *IDENT();
    antlr4::tree::TerminalNode *COLON();
    TypeContext *type();
    antlr4::tree::TerminalNode *RPAREN();
    BlockContext *block();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  CatchClauseContext* catchClause();

  class  FinallyClauseContext : public antlr4::ParserRuleContext {
  public:
    FinallyClauseContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *FINALLY();
    BlockContext *block();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  FinallyClauseContext* finallyClause();

  class  ThrowStmtContext : public antlr4::ParserRuleContext {
  public:
    ThrowStmtContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *THROW();
    ExprContext *expr();
    antlr4::tree::TerminalNode *SEMI();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ThrowStmtContext* throwStmt();

  class  ExprStmtContext : public antlr4::ParserRuleContext {
  public:
    ExprStmtContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    ExprContext *expr();
    antlr4::tree::TerminalNode *SEMI();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ExprStmtContext* exprStmt();

  class  ExprListContext : public antlr4::ParserRuleContext {
  public:
    ExprListContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<ExprContext *> expr();
    ExprContext* expr(size_t i);
    std::vector<antlr4::tree::TerminalNode *> COMMA();
    antlr4::tree::TerminalNode* COMMA(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ExprListContext* exprList();

  class  ExprContext : public antlr4::ParserRuleContext {
  public:
    ExprContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    AssignExprContext *assignExpr();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ExprContext* expr();

  class  AssignExprContext : public antlr4::ParserRuleContext {
  public:
    AssignExprContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    TernaryExprContext *ternaryExpr();
    AssignOpContext *assignOp();
    AssignExprContext *assignExpr();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  AssignExprContext* assignExpr();

  class  AssignOpContext : public antlr4::ParserRuleContext {
  public:
    AssignOpContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *ASSIGN();
    antlr4::tree::TerminalNode *PLUS_ASSIGN();
    antlr4::tree::TerminalNode *MINUS_ASSIGN();
    antlr4::tree::TerminalNode *STAR_ASSIGN();
    antlr4::tree::TerminalNode *SLASH_ASSIGN();
    antlr4::tree::TerminalNode *PCT_ASSIGN();
    antlr4::tree::TerminalNode *QQ_ASSIGN();
    antlr4::tree::TerminalNode *AMP_ASSIGN();
    antlr4::tree::TerminalNode *PIPE_ASSIGN();
    antlr4::tree::TerminalNode *CARET_ASSIGN();
    antlr4::tree::TerminalNode *LSHIFT_ASSIGN();
    antlr4::tree::TerminalNode *RSHIFT_ASSIGN();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  AssignOpContext* assignOp();

  class  TernaryExprContext : public antlr4::ParserRuleContext {
  public:
    TernaryExprContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    NullCoalesceExprContext *nullCoalesceExpr();
    antlr4::tree::TerminalNode *QUESTION();
    std::vector<ExprContext *> expr();
    ExprContext* expr(size_t i);
    antlr4::tree::TerminalNode *COLON();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  TernaryExprContext* ternaryExpr();

  class  NullCoalesceExprContext : public antlr4::ParserRuleContext {
  public:
    NullCoalesceExprContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<OrExprContext *> orExpr();
    OrExprContext* orExpr(size_t i);
    std::vector<antlr4::tree::TerminalNode *> QQ();
    antlr4::tree::TerminalNode* QQ(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  NullCoalesceExprContext* nullCoalesceExpr();

  class  OrExprContext : public antlr4::ParserRuleContext {
  public:
    OrExprContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<AndExprContext *> andExpr();
    AndExprContext* andExpr(size_t i);
    std::vector<antlr4::tree::TerminalNode *> OR_OR();
    antlr4::tree::TerminalNode* OR_OR(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  OrExprContext* orExpr();

  class  AndExprContext : public antlr4::ParserRuleContext {
  public:
    AndExprContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<BitOrExprContext *> bitOrExpr();
    BitOrExprContext* bitOrExpr(size_t i);
    std::vector<antlr4::tree::TerminalNode *> AND_AND();
    antlr4::tree::TerminalNode* AND_AND(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  AndExprContext* andExpr();

  class  BitOrExprContext : public antlr4::ParserRuleContext {
  public:
    BitOrExprContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<BitXorExprContext *> bitXorExpr();
    BitXorExprContext* bitXorExpr(size_t i);
    std::vector<antlr4::tree::TerminalNode *> PIPE();
    antlr4::tree::TerminalNode* PIPE(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  BitOrExprContext* bitOrExpr();

  class  BitXorExprContext : public antlr4::ParserRuleContext {
  public:
    BitXorExprContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<BitAndExprContext *> bitAndExpr();
    BitAndExprContext* bitAndExpr(size_t i);
    std::vector<antlr4::tree::TerminalNode *> CARET();
    antlr4::tree::TerminalNode* CARET(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  BitXorExprContext* bitXorExpr();

  class  BitAndExprContext : public antlr4::ParserRuleContext {
  public:
    BitAndExprContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<EqualityExprContext *> equalityExpr();
    EqualityExprContext* equalityExpr(size_t i);
    std::vector<antlr4::tree::TerminalNode *> AMP();
    antlr4::tree::TerminalNode* AMP(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  BitAndExprContext* bitAndExpr();

  class  EqualityExprContext : public antlr4::ParserRuleContext {
  public:
    EqualityExprContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<RelationalExprContext *> relationalExpr();
    RelationalExprContext* relationalExpr(size_t i);
    std::vector<antlr4::tree::TerminalNode *> EQ();
    antlr4::tree::TerminalNode* EQ(size_t i);
    std::vector<antlr4::tree::TerminalNode *> NEQ();
    antlr4::tree::TerminalNode* NEQ(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  EqualityExprContext* equalityExpr();

  class  RelationalExprContext : public antlr4::ParserRuleContext {
  public:
    RelationalExprContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<ShiftExprContext *> shiftExpr();
    ShiftExprContext* shiftExpr(size_t i);
    std::vector<antlr4::tree::TerminalNode *> LT();
    antlr4::tree::TerminalNode* LT(size_t i);
    std::vector<antlr4::tree::TerminalNode *> GT();
    antlr4::tree::TerminalNode* GT(size_t i);
    std::vector<antlr4::tree::TerminalNode *> LE();
    antlr4::tree::TerminalNode* LE(size_t i);
    std::vector<antlr4::tree::TerminalNode *> GE();
    antlr4::tree::TerminalNode* GE(size_t i);
    TypeContext *type();
    antlr4::tree::TerminalNode *IS();
    antlr4::tree::TerminalNode *AS();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  RelationalExprContext* relationalExpr();

  class  ShiftExprContext : public antlr4::ParserRuleContext {
  public:
    ShiftExprContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<AdditiveExprContext *> additiveExpr();
    AdditiveExprContext* additiveExpr(size_t i);
    std::vector<antlr4::tree::TerminalNode *> LSHIFT();
    antlr4::tree::TerminalNode* LSHIFT(size_t i);
    std::vector<antlr4::tree::TerminalNode *> GT();
    antlr4::tree::TerminalNode* GT(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ShiftExprContext* shiftExpr();

  class  AdditiveExprContext : public antlr4::ParserRuleContext {
  public:
    AdditiveExprContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<MultiplicativeExprContext *> multiplicativeExpr();
    MultiplicativeExprContext* multiplicativeExpr(size_t i);
    std::vector<antlr4::tree::TerminalNode *> PLUS();
    antlr4::tree::TerminalNode* PLUS(size_t i);
    std::vector<antlr4::tree::TerminalNode *> MINUS();
    antlr4::tree::TerminalNode* MINUS(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  AdditiveExprContext* additiveExpr();

  class  MultiplicativeExprContext : public antlr4::ParserRuleContext {
  public:
    MultiplicativeExprContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<UnaryExprContext *> unaryExpr();
    UnaryExprContext* unaryExpr(size_t i);
    std::vector<antlr4::tree::TerminalNode *> STAR();
    antlr4::tree::TerminalNode* STAR(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SLASH();
    antlr4::tree::TerminalNode* SLASH(size_t i);
    std::vector<antlr4::tree::TerminalNode *> PCT();
    antlr4::tree::TerminalNode* PCT(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  MultiplicativeExprContext* multiplicativeExpr();

  class  UnaryExprContext : public antlr4::ParserRuleContext {
  public:
    UnaryExprContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    UnaryExprContext *unaryExpr();
    antlr4::tree::TerminalNode *BANG();
    antlr4::tree::TerminalNode *TILDE();
    antlr4::tree::TerminalNode *MINUS();
    antlr4::tree::TerminalNode *PLUS();
    antlr4::tree::TerminalNode *INCR();
    antlr4::tree::TerminalNode *DECR();
    PostfixExprContext *postfixExpr();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  UnaryExprContext* unaryExpr();

  class  PostfixExprContext : public antlr4::ParserRuleContext {
  public:
    PostfixExprContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    PrimaryContext *primary();
    std::vector<PostfixOpContext *> postfixOp();
    PostfixOpContext* postfixOp(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  PostfixExprContext* postfixExpr();

  class  PostfixOpContext : public antlr4::ParserRuleContext {
  public:
    PostfixOpContext(antlr4::ParserRuleContext *parent, size_t invokingState);
   
    PostfixOpContext() = default;
    void copyFrom(PostfixOpContext *context);
    using antlr4::ParserRuleContext::copyFrom;

    virtual size_t getRuleIndex() const override;

   
  };

  class  NotNullAssertContext : public PostfixOpContext {
  public:
    NotNullAssertContext(PostfixOpContext *ctx);

    std::vector<antlr4::tree::TerminalNode *> BANG();
    antlr4::tree::TerminalNode* BANG(size_t i);

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  class  MemberAccessContext : public PostfixOpContext {
  public:
    MemberAccessContext(PostfixOpContext *ctx);

    antlr4::tree::TerminalNode *DOT();
    antlr4::tree::TerminalNode *IDENT();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  class  CallOpContext : public PostfixOpContext {
  public:
    CallOpContext(PostfixOpContext *ctx);

    antlr4::tree::TerminalNode *LPAREN();
    antlr4::tree::TerminalNode *RPAREN();
    ArgListContext *argList();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  class  IndexOpContext : public PostfixOpContext {
  public:
    IndexOpContext(PostfixOpContext *ctx);

    antlr4::tree::TerminalNode *LBRACK();
    ExprContext *expr();
    antlr4::tree::TerminalNode *RBRACK();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  class  PostDecrContext : public PostfixOpContext {
  public:
    PostDecrContext(PostfixOpContext *ctx);

    antlr4::tree::TerminalNode *DECR();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  class  PostIncrContext : public PostfixOpContext {
  public:
    PostIncrContext(PostfixOpContext *ctx);

    antlr4::tree::TerminalNode *INCR();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  class  SafeMemberAccessContext : public PostfixOpContext {
  public:
    SafeMemberAccessContext(PostfixOpContext *ctx);

    antlr4::tree::TerminalNode *QUESTION();
    antlr4::tree::TerminalNode *DOT();
    antlr4::tree::TerminalNode *IDENT();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  PostfixOpContext* postfixOp();

  class  ArgListContext : public antlr4::ParserRuleContext {
  public:
    ArgListContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<ArgContext *> arg();
    ArgContext* arg(size_t i);
    std::vector<antlr4::tree::TerminalNode *> COMMA();
    antlr4::tree::TerminalNode* COMMA(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ArgListContext* argList();

  class  ArgContext : public antlr4::ParserRuleContext {
  public:
    ArgContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    ExprContext *expr();
    antlr4::tree::TerminalNode *IDENT();
    antlr4::tree::TerminalNode *COLON();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ArgContext* arg();

  class  PrimaryContext : public antlr4::ParserRuleContext {
  public:
    PrimaryContext(antlr4::ParserRuleContext *parent, size_t invokingState);
   
    PrimaryContext() = default;
    void copyFrom(PrimaryContext *context);
    using antlr4::ParserRuleContext::copyFrom;

    virtual size_t getRuleIndex() const override;

   
  };

  class  LitPrimaryContext : public PrimaryContext {
  public:
    LitPrimaryContext(PrimaryContext *ctx);

    LiteralContext *literal();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  class  ThisPrimaryContext : public PrimaryContext {
  public:
    ThisPrimaryContext(PrimaryContext *ctx);

    antlr4::tree::TerminalNode *THIS();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  class  IdentPrimaryContext : public PrimaryContext {
  public:
    IdentPrimaryContext(PrimaryContext *ctx);

    antlr4::tree::TerminalNode *IDENT();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  class  WhenPrimaryContext : public PrimaryContext {
  public:
    WhenPrimaryContext(PrimaryContext *ctx);

    WhenStmtContext *whenStmt();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  class  ParenPrimaryContext : public PrimaryContext {
  public:
    ParenPrimaryContext(PrimaryContext *ctx);

    antlr4::tree::TerminalNode *LPAREN();
    ExprContext *expr();
    antlr4::tree::TerminalNode *RPAREN();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  class  NewArrayInitPrimaryContext : public PrimaryContext {
  public:
    NewArrayInitPrimaryContext(PrimaryContext *ctx);

    antlr4::tree::TerminalNode *NEW();
    TypeContext *type();
    antlr4::tree::TerminalNode *LBRACE();
    antlr4::tree::TerminalNode *RBRACE();
    ArrayElementListContext *arrayElementList();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  class  SuperPrimaryContext : public PrimaryContext {
  public:
    SuperPrimaryContext(PrimaryContext *ctx);

    antlr4::tree::TerminalNode *SUPER();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  class  NewPrimaryContext : public PrimaryContext {
  public:
    NewPrimaryContext(PrimaryContext *ctx);

    antlr4::tree::TerminalNode *NEW();
    TypeContext *type();
    antlr4::tree::TerminalNode *LPAREN();
    antlr4::tree::TerminalNode *RPAREN();
    ArgListContext *argList();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  class  LambdaPrimaryContext : public PrimaryContext {
  public:
    LambdaPrimaryContext(PrimaryContext *ctx);

    LambdaContext *lambda();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  PrimaryContext* primary();

  class  LambdaContext : public antlr4::ParserRuleContext {
  public:
    LambdaContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *LBRACE();
    antlr4::tree::TerminalNode *RBRACE();
    LambdaParamsContext *lambdaParams();
    antlr4::tree::TerminalNode *ARROW();
    std::vector<StatementContext *> statement();
    StatementContext* statement(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  LambdaContext* lambda();

  class  LambdaParamsContext : public antlr4::ParserRuleContext {
  public:
    LambdaParamsContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<antlr4::tree::TerminalNode *> IDENT();
    antlr4::tree::TerminalNode* IDENT(size_t i);
    std::vector<antlr4::tree::TerminalNode *> COLON();
    antlr4::tree::TerminalNode* COLON(size_t i);
    std::vector<TypeContext *> type();
    TypeContext* type(size_t i);
    std::vector<antlr4::tree::TerminalNode *> COMMA();
    antlr4::tree::TerminalNode* COMMA(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  LambdaParamsContext* lambdaParams();

  class  ArrayElementListContext : public antlr4::ParserRuleContext {
  public:
    ArrayElementListContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<ArrayElementContext *> arrayElement();
    ArrayElementContext* arrayElement(size_t i);
    std::vector<antlr4::tree::TerminalNode *> COMMA();
    antlr4::tree::TerminalNode* COMMA(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ArrayElementListContext* arrayElementList();

  class  ArrayElementContext : public antlr4::ParserRuleContext {
  public:
    ArrayElementContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *ELLIPSIS();
    ExprContext *expr();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ArrayElementContext* arrayElement();

  class  LiteralContext : public antlr4::ParserRuleContext {
  public:
    LiteralContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *INT_LIT();
    antlr4::tree::TerminalNode *FLOAT_LIT();
    antlr4::tree::TerminalNode *STRING_LIT();
    antlr4::tree::TerminalNode *VERBATIM_STRING();
    antlr4::tree::TerminalNode *CHAR_LIT();
    TemplateStringContext *templateString();
    antlr4::tree::TerminalNode *TRUE();
    antlr4::tree::TerminalNode *FALSE();
    antlr4::tree::TerminalNode *NULL_LIT();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  LiteralContext* literal();

  class  TemplateStringContext : public antlr4::ParserRuleContext {
  public:
    TemplateStringContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *TEMPLATE_END();
    antlr4::tree::TerminalNode *TEMPLATE_START();
    antlr4::tree::TerminalNode *VERBATIM_TEMPLATE_START();
    std::vector<TemplatePartContext *> templatePart();
    TemplatePartContext* templatePart(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  TemplateStringContext* templateString();

  class  TemplatePartContext : public antlr4::ParserRuleContext {
  public:
    TemplatePartContext(antlr4::ParserRuleContext *parent, size_t invokingState);
   
    TemplatePartContext() = default;
    void copyFrom(TemplatePartContext *context);
    using antlr4::ParserRuleContext::copyFrom;

    virtual size_t getRuleIndex() const override;

   
  };

  class  TmplTextContext : public TemplatePartContext {
  public:
    TmplTextContext(TemplatePartContext *ctx);

    antlr4::tree::TerminalNode *TEMPLATE_TEXT();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  class  TmplInterpContext : public TemplatePartContext {
  public:
    TmplInterpContext(TemplatePartContext *ctx);

    antlr4::tree::TerminalNode *TEMPLATE_INTERP_START();
    ExprContext *expr();
    antlr4::tree::TerminalNode *RBRACE();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  TemplatePartContext* templatePart();


  // By default the static state used to implement the parser is lazily initialized during the first
  // call to the constructor. You can call this function if you wish to initialize the static state
  // ahead of time.
  static void initialize();

private:
};

