/*
 * HaoLang 词法定义 v0.1
 * ------------------------------------------------------------
 * 必须独立成 lexer grammar：组合语法（combined grammar）不允许使用
 * 词法模式（lexical modes），而 C# 风格模板字符串 $"a{expr}b"
 * 需要 pushMode/popMode 才能正确切换"文本"与"表达式"两种上下文。
 */
lexer grammar HaoLangLexer;

// ============================================================
//  关键字（必须放在 IDENT 之前，否则会被 IDENT 抢先匹配）
// ============================================================

// 声明
PACKAGE     : 'package' ;
IMPORT      : 'import' ;
EXTERN      : 'extern' ;
FUNC        : 'func' ;
CLASS       : 'class' ;
INTERFACE   : 'interface' ;
ENUM        : 'enum' ;
EXTENDS     : 'extends' ;
IMPLEMENTS  : 'implements' ;
CONSTRUCTOR : 'constructor' ;
VAL         : 'val' ;
VAR         : 'var' ;
NEW         : 'new' ;

// 修饰符
PUBLIC      : 'public' ;
PRIVATE     : 'private' ;
PROTECTED   : 'protected' ;
INTERNAL    : 'internal' ;
STATIC      : 'static' ;
ABSTRACT    : 'abstract' ;
OVERRIDE    : 'override' ;
VIRTUAL     : 'virtual' ;
ASYNC       : 'async' ;
DELEGATE    : 'delegate' ;

// 控制流
IF          : 'if' ;
ELSE        : 'else' ;
WHILE       : 'while' ;
FOR         : 'for' ;
IN          : 'in' ;
WHEN        : 'when' ;
RETURN      : 'return' ;
BREAK       : 'break' ;
CONTINUE    : 'continue' ;
TRY         : 'try' ;
CATCH       : 'catch' ;
FINALLY     : 'finally' ;
THROW       : 'throw' ;
HAOROUTINE  : 'haoroutine' ;
SELECT      : 'select' ;
CASE        : 'case' ;
DEFAULT     : 'default' ;

// 表达式关键字
THIS        : 'this' ;
SUPER       : 'super' ;
IS          : 'is' ;
AS          : 'as' ;
TRUE        : 'true' ;
FALSE       : 'false' ;
NULL_LIT    : 'null' ;

// ============================================================
//  运算符与分隔符
//  注意：多字符运算符必须排在其前缀之前
//  ('??=' 先于 '??'，'++' 先于 '+'，'->' 先于 '-')
// ============================================================

QQ_ASSIGN   : '??=' ;
QQ          : '??' ;
ARROW       : '->' ;

PLUS_ASSIGN : '+=' ;
MINUS_ASSIGN: '-=' ;
STAR_ASSIGN : '*=' ;
SLASH_ASSIGN: '/=' ;
PCT_ASSIGN  : '%=' ;
AMP_ASSIGN  : '&=' ;
PIPE_ASSIGN : '|=' ;
CARET_ASSIGN: '^=' ;
LSHIFT_ASSIGN : '<<=' ;
RSHIFT_ASSIGN : '>>=' ;

INCR        : '++' ;
DECR        : '--' ;

EQ          : '==' ;
NEQ         : '!=' ;
LE          : '<=' ;
GE          : '>=' ;
LSHIFT      : '<<' ;
AND_AND     : '&&' ;
OR_OR       : '||' ;

ASSIGN      : '=' ;
PLUS        : '+' ;
MINUS       : '-' ;
STAR        : '*' ;
SLASH       : '/' ;
PCT         : '%' ;
BANG        : '!' ;
TILDE       : '~' ;
AMP         : '&' ;
PIPE        : '|' ;
CARET       : '^' ;
LT          : '<' ;
GT          : '>' ;
QUESTION    : '?' ;
COLON       : ':' ;
SEMI        : ';' ;
COMMA       : ',' ;
ELLIPSIS    : '...' ;   // 数组展开 [...arr]（须在 DOT 之前，最长匹配）
DOT         : '.' ;

// 逐字/插值前缀必须先于单独 AT，避免 @" 被拆成 AT + STRING
VERBATIM_TEMPLATE_START
    : ('@$"' | '$@"') -> pushMode(VERBATIM_TEMPLATE)
    ;
VERBATIM_STRING
    : '@"' ( '""' | ~["] )* '"'
    ;

AT          : '@' ;   // extern @link / 注解

LPAREN      : '(' ;
RPAREN      : ')' ;
LBRACK      : '[' ;
RBRACK      : ']' ;

// '{' 与 '}' 需参与模式栈维护，见下方说明
LBRACE      : '{' -> pushMode(DEFAULT_MODE) ;
RBRACE      : '}' -> popMode ;

// ============================================================
//  字面量
// ============================================================

// 模板字符串起始，切换到 TEMPLATE 模式
TEMPLATE_START : '$"' -> pushMode(TEMPLATE) ;

STRING_LIT  : '"' (~["\\\r\n] | EscapeSeq)* '"' ;
CHAR_LIT    : '\'' (~['\\\r\n] | EscapeSeq) '\'' ;

fragment EscapeSeq
    : '\\' ['"?abfnrtv\\$]
    | '\\' 'u' HexDigit HexDigit HexDigit HexDigit
    ;

// 浮点必须先于整数，否则 '1.5' 会被切成 INT '1' + DOT + INT '5'
FLOAT_LIT
    : Digit ('_'? Digit)* '.' Digit ('_'? Digit)* Exponent?
    | Digit ('_'? Digit)* Exponent
    ;

INT_LIT
    : '0' [xX] HexDigit ('_'? HexDigit)*
    | '0' [bB] [01] ('_'? [01])*
    | Digit ('_'? Digit)*
    ;

fragment Exponent : [eE] [+-]? Digit+ ;
fragment Digit    : [0-9] ;
fragment HexDigit : [0-9a-fA-F] ;

IDENT : [a-zA-Z_] [a-zA-Z_0-9]* ;

// ============================================================
//  注释与空白
// ============================================================

LINE_COMMENT  : '//' ~[\r\n]*  -> channel(HIDDEN) ;
BLOCK_COMMENT : '/*' .*? '*/'  -> channel(HIDDEN) ;
WS            : [ \t\r\n]+     -> channel(HIDDEN) ;

// 兜底规则：任何未识别字符都产出一个 token，
// 交由语法层报出清晰的错误位置，而不是让词法器静默丢弃。
UNKNOWN_CHAR : . ;

// ============================================================
//  TEMPLATE 模式： $"Hello {name}, total={a + b}"
// ------------------------------------------------------------
//  模式栈工作原理：
//    $"        -> pushMode(TEMPLATE)
//    文本       -> TEMPLATE_TEXT（留在 TEMPLATE）
//    {         -> pushMode(DEFAULT_MODE)，插值表达式按普通语法解析
//    }         -> popMode，回到 TEMPLATE（由 DEFAULT_MODE 的 RBRACE 完成）
//    "         -> popMode，模板结束
// ============================================================
mode TEMPLATE;

TEMPLATE_END          : '"' -> popMode ;
TEMPLATE_INTERP_START : '{' -> pushMode(DEFAULT_MODE) ;

// 转义 {{ 和 }} 表示字面花括号
TEMPLATE_TEXT
    : ( ~["{}\\]
      | '\\' .
      | '{{'
      | '}}'
      )+
    ;

// ============================================================
//  VERBATIM_TEMPLATE：@$"..." / $@"..."（不转义，"" 为引号）
// ============================================================
mode VERBATIM_TEMPLATE;

VTEMPLATE_END
    : '"' -> type(TEMPLATE_END), popMode
    ;
VTEMPLATE_INTERP_START
    : '{' -> type(TEMPLATE_INTERP_START), pushMode(DEFAULT_MODE)
    ;
VTEMPLATE_TEXT
    : ( '""' | '{{' | '}}' | ~["{}] )+ -> type(TEMPLATE_TEXT)
    ;
