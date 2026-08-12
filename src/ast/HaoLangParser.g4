/*
 * HaoLang 语法定义 v0.1
 * ------------------------------------------------------------
 * 设计取舍：
 *   - 工具链/编译模型      -> Go
 *   - 包 = 目录层级         -> Java
 *   - 类型与成员声明语法    -> C#
 *   - val/var、空安全、when -> Kotlin
 *
 * 词法在 HaoLangLexer.g4 中单独定义（模板字符串需要词法模式）。
 */
parser grammar HaoLangParser;

options { tokenVocab = HaoLangLexer; }

// ============================================================
//  编译单元
// ============================================================

compilationUnit
    : packageDecl? importDecl* topLevelDecl* EOF
    ;

packageDecl
    : PACKAGE qualifiedName SEMI?
    ;

importDecl
    : IMPORT qualifiedName (DOT STAR)? (AS IDENT)? SEMI?
    ;

qualifiedName
    : IDENT (DOT IDENT)*
    ;

topLevelDecl
    : funcDecl
    | classDecl
    | interfaceDecl
    | enumDecl
    | fieldDecl
    | delegateDecl
    | annotationDecl
    ;

// C# 风格委托：给函数类型起一个命名别名，如
//   delegate (Int)->String MyConverter;
// 之后 MyConverter 可直接作为类型标注使用（可赋 lambda / 方法组 / 函数名）。
delegateDecl
    : DELEGATE type IDENT SEMI
    ;

// Java 风格枚举：常量集合 + 名称/序数。
// 编译成普通类：每个常量是一个静态字段（Color.RED），构造时以 name/ordinal
// 创建实例；提供 name()/ordinal()/toString()。new Color(...) 被拒绝。
enumDecl
    : modifier* ENUM IDENT LBRACE (enumConstant (COMMA enumConstant)*)? RBRACE
    ;

enumConstant
    : IDENT
    ;

// ============================================================
//  函数
// ============================================================

// 函数声明。
// 抽象方法（abstract）与接口方法没有实现体，故 block 可省略、以 ';' 结尾。
// 是否允许省略由语义分析校验：只有 abstract 方法可以无体。
// 函数声明。
// 抽象方法（abstract）与接口方法没有实现体，故 block 可省略、以 ';' 结尾。
// extern 函数声明外部 C 函数（运行时/系统库），用 = "linkName" 指定其
// 在 LLVM IR 中的符号名（如 "hao_println_int"），同样以 ';' 结尾、无函数体。
// 是否允许省略由语义分析校验：只有 abstract / interface / extern 方法可以无体。
// 可选 @link("ws2_32", ...) 声明该符号所在的外部链接库（追加到链接命令）。
funcDecl
    : annotationUse* modifier* FUNC IDENT typeParams? LPAREN paramList? RPAREN returnType?
      whereClause? (ASSIGN STRING_LIT)? linkDecl? (block | SEMI)
    ;

linkDecl
    : AT IDENT LPAREN (STRING_LIT (COMMA STRING_LIT)*)? RPAREN
    ;

returnType
    : COLON type
    ;

paramList
    : param (COMMA param)*
    ;

param
    : IDENT COLON type (ASSIGN expr)?
    ;

typeParams
    : LT IDENT (COMMA IDENT)* GT
    ;

// 泛型约束：where T : Comparable, U : Speaker（每绑定一个上界；同参可重复 AND）
whereClause
    : WHERE whereBinding (COMMA whereBinding)*
    ;

whereBinding
    : IDENT COLON type
    ;

modifier
    : PUBLIC | PRIVATE | PROTECTED | INTERNAL
    | STATIC | ABSTRACT | OVERRIDE | VIRTUAL | ASYNC | EXTERN
    ;

// ============================================================
//  类 / 接口
// ============================================================

classDecl
    : annotationUse* modifier* CLASS IDENT typeParams? classBase? whereClause? LBRACE classMember* RBRACE
    ;

// 类的"根基"两种写法任选其一：
//   - C#/Kotlin 风格 `class Dog : Animal, Shape`（继承/接口靠类型自动分流）
//   - Java 风格 `class Dog extends Animal implements Shape`（显式区分）
// 均解析进 baseName（继承）与 interfaceNames（接口）。
classBase
    : COLON typeList                           # colonBase
    | EXTENDS type (IMPLEMENTS typeList)?      # extendsBase
    | IMPLEMENTS typeList                      # implementsBase
    ;

interfaceDecl
    : modifier* INTERFACE IDENT typeParams? (COLON typeList)? whereClause? LBRACE interfaceMember* RBRACE
    ;

typeList
    : type (COMMA type)*
    ;

// 属性须先于字段尝试：两者前缀相同，靠 '{' 区分
classMember
    : funcDecl
    | constructorDecl
    | staticCtorDecl
    | propertyDecl
    | fieldDecl
    ;

// C# 风格静态构造器：类首次被引用（访问静态成员 / 首次 new）前自动执行一次。
// 形式 `static ClassName() { ... }`，IDENT 须为类名（语义层校验）。
staticCtorDecl
    : STATIC IDENT LPAREN paramList? RPAREN block
    ;

interfaceMember
    : modifier* FUNC IDENT typeParams? LPAREN paramList? RPAREN returnType? whereClause? (block | SEMI?)
    | propertyDecl
    ;

constructorDecl
    : modifier* CONSTRUCTOR LPAREN paramList? RPAREN block
    ;

// C# 风格自动属性： public var name: String { get; set; }
propertyDecl
    : modifier* (VAL | VAR) IDENT COLON type LBRACE accessor+ RBRACE
    ;

// get / set 作为"上下文关键字"：仅在属性块内有特殊含义。
// 若在词法层定义为关键字，就无法再用作普通方法名（如 func get()），
// 这与 C# 的处理一致。此处语法上接受任意标识符，
// 是否为合法访问器名由语义分析阶段校验。
accessor
    : IDENT (block | SEMI)
    ;

fieldDecl
    : annotationUse* modifier* (VAL | VAR) IDENT (COLON type)? (ASSIGN expr)? SEMI?
    ;

// 注解使用：@Name 或 @Name(args)，标记在类/方法/字段前。
annotationUse
    : AT qualifiedName (LPAREN annotationArgs? RPAREN)?
    ;

annotationArgs
    : annotationArg (COMMA annotationArg)*
    ;

annotationArg
    : (IDENT ASSIGN)? expr
    ;

// 注解类型定义（Java 风格 @interface）：@interface Name { val value: String; }
// 编译成普通类，注解字段用 val 声明，供反射读取。
// 语法 AT INTERFACE：`@` 后跟 `interface` 关键字即注解声明；
// 若是 `@Name(...)`（annotationUse）则是注解使用（标记类/方法/字段前）。
annotationDecl
    : AT INTERFACE IDENT LBRACE annotationMember* RBRACE
    ;

annotationMember
    : modifier* (VAL | VAR) IDENT (COLON type)? (ASSIGN expr)? SEMI?
    ;

// ============================================================
//  类型（后缀 ? 表示可空）
// ============================================================

type
    : baseType QUESTION?
    ;

baseType
    : qualifiedName typeArgs?              # namedType
    | LBRACK type RBRACK                   # arrayType
    | LPAREN typeList? RPAREN ARROW type   # funcType
    ;

// typeArg：具体类型，或 ? / ? extends T / ? super T（可空仍是 type 后缀 T?）
typeArg
    : type
    | QUESTION (EXTENDS type | SUPER type)?
    ;

typeArgs
    : LT typeArg (COMMA typeArg)* GT
    ;

// ============================================================
//  语句
// ============================================================

block
    : LBRACE statement* RBRACE
    ;

statement
    : varDecl
    | ifStmt
    | whileStmt
    | forStmt
    | whenStmt
    | tryStmt
    | throwStmt
    | haoroutineStmt
    | selectStmt
    | returnStmt
    | breakStmt
    | continueStmt
    | block
    | exprStmt
    | SEMI
    ;

// 并发：haoroutine { ... } —— 体为无参 lambda，启动 OS 线程火即忘
haoroutineStmt
    : HAOROUTINE lambda
    ;

// 多路 channel：select { case x = ch.recv(): ... case ch.send(v): ... default: ... }
selectStmt
    : SELECT LBRACE selectCase* RBRACE
    ;

selectCase
    : CASE selectComm COLON statement*
    | DEFAULT COLON statement*
    ;

// recv：x = ch.recv() / recvInt() / recvStr()
// send：ch.send(e) / sendInt(e) / sendStr(e)
selectComm
    : IDENT ASSIGN expr DOT IDENT LPAREN RPAREN
    | expr DOT IDENT LPAREN expr RPAREN
    ;

varDecl
    : (VAL | VAR) IDENT (COLON type)? (ASSIGN expr)? SEMI?
    ;

ifStmt
    : IF LPAREN expr RPAREN statement (ELSE statement)?
    ;

whileStmt
    : WHILE LPAREN expr RPAREN statement
    ;

forStmt
    : FOR LPAREN IDENT IN expr RPAREN statement
    ;

// Kotlin 风格 when，可作语句也可作表达式
whenStmt
    : WHEN (LPAREN expr RPAREN)? LBRACE whenBranch* RBRACE
    ;

whenBranch
    : exprList ARROW (block | expr SEMI?)
    | ELSE ARROW (block | expr SEMI?)
    ;

returnStmt
    : RETURN expr? SEMI?
    ;

breakStmt
    : BREAK SEMI?
    ;

continueStmt
    : CONTINUE SEMI?
    ;

// try { ... } catch (e: Type) { ... } ... finally { ... }
tryStmt
    : TRY block catchClause* finallyClause?
    ;

catchClause
    : CATCH LPAREN IDENT COLON type RPAREN block
    ;

finallyClause
    : FINALLY block
    ;

throwStmt
    : THROW expr SEMI?
    ;

exprStmt
    : expr SEMI?
    ;

exprList
    : expr (COMMA expr)*
    ;

// ============================================================
//  表达式（自低到高优先级逐层展开）
// ============================================================

expr
    : assignExpr
    ;

assignExpr
    : ternaryExpr (assignOp assignExpr)?
    ;

assignOp
    : ASSIGN | PLUS_ASSIGN | MINUS_ASSIGN | STAR_ASSIGN
    | SLASH_ASSIGN | PCT_ASSIGN | QQ_ASSIGN
    | AMP_ASSIGN | PIPE_ASSIGN | CARET_ASSIGN
    | LSHIFT_ASSIGN | RSHIFT_ASSIGN
    ;

ternaryExpr
    : nullCoalesceExpr (QUESTION expr COLON expr)?
    ;

nullCoalesceExpr
    : orExpr (QQ orExpr)*
    ;

orExpr
    : andExpr (OR_OR andExpr)*
    ;

andExpr
    : bitOrExpr (AND_AND bitOrExpr)*
    ;

bitOrExpr
    : bitXorExpr (PIPE bitXorExpr)*
    ;

bitXorExpr
    : bitAndExpr (CARET bitAndExpr)*
    ;

bitAndExpr
    : equalityExpr (AMP equalityExpr)*
    ;

equalityExpr
    : relationalExpr ((EQ | NEQ) relationalExpr)*
    ;

relationalExpr
    : shiftExpr ((LT | GT | LE | GE) shiftExpr)*
    | shiftExpr ((IS | AS) type)                       // 类型判定/转换右侧是类型
    ;

shiftExpr
    : additiveExpr ((LSHIFT | GT GT) additiveExpr)*
    ;

additiveExpr
    : multiplicativeExpr ((PLUS | MINUS) multiplicativeExpr)*
    ;

multiplicativeExpr
    : unaryExpr ((STAR | SLASH | PCT) unaryExpr)*
    ;

unaryExpr
    : (BANG | TILDE | MINUS | PLUS | INCR | DECR) unaryExpr
    | postfixExpr
    ;

postfixExpr
    : primary postfixOp*
    ;

postfixOp
    : DOT IDENT                            # memberAccess
    | QUESTION DOT IDENT                   # safeMemberAccess
    | BANG BANG                            # notNullAssert
    | LPAREN argList? RPAREN               # callOp
    | LBRACK expr RBRACK                   # indexOp
    | INCR                                 # postIncr
    | DECR                                 # postDecr
    ;

argList
    : arg (COMMA arg)*
    ;

arg
    : (IDENT COLON)? expr                  // 支持具名实参 foo(name: "hao")
    ;

primary
    : literal                                      # litPrimary
    | IDENT                                        # identPrimary
    | THIS                                         # thisPrimary
    | SUPER                                        # superPrimary
    | whenStmt                                     # whenPrimary
    | LPAREN expr RPAREN                           # parenPrimary
    | NEW type LPAREN argList? RPAREN              # newPrimary
    | arrayLiteral                                 # arrayPrimary
    | lambda                                       # lambdaPrimary
    ;

// { x, y -> x + y }  /  { it * 2 }
lambda
    : LBRACE (lambdaParams ARROW)? statement* RBRACE
    ;

lambdaParams
    : IDENT (COLON type)? (COMMA IDENT (COLON type)?)*
    ;

// 数组字面量：带初值 [1,2,3]；展开 [...a, 9, ...b]（v0.32）
arrayLiteral
    : LBRACK arrayElementList? RBRACK
    ;

arrayElementList
    : arrayElement (COMMA arrayElement)*
    ;

arrayElement
    : ELLIPSIS expr
    | expr
    ;

literal
    : INT_LIT
    | FLOAT_LIT
    | STRING_LIT
    | VERBATIM_STRING
    | CHAR_LIT
    | templateString
    | TRUE
    | FALSE
    | NULL_LIT
    ;

// $"..." / @$"..." / $@"..."
templateString
    : (TEMPLATE_START | VERBATIM_TEMPLATE_START) templatePart* TEMPLATE_END
    ;

templatePart
    : TEMPLATE_TEXT                            # tmplText
    | TEMPLATE_INTERP_START expr RBRACE        # tmplInterp
    ;
