
// Generated from D:/buildLang/src/ast/HaoLangLexer.g4 by ANTLR 4.13.2

#pragma once


#include "antlr4-runtime.h"




class  HaoLangLexer : public antlr4::Lexer {
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
    QQ_ASSIGN = 48, QQ = 49, ARROW = 50, PLUS_ASSIGN = 51, MINUS_ASSIGN = 52, 
    STAR_ASSIGN = 53, SLASH_ASSIGN = 54, PCT_ASSIGN = 55, AMP_ASSIGN = 56, 
    PIPE_ASSIGN = 57, CARET_ASSIGN = 58, LSHIFT_ASSIGN = 59, RSHIFT_ASSIGN = 60, 
    INCR = 61, DECR = 62, EQ = 63, NEQ = 64, LE = 65, GE = 66, LSHIFT = 67, 
    AND_AND = 68, OR_OR = 69, ASSIGN = 70, PLUS = 71, MINUS = 72, STAR = 73, 
    SLASH = 74, PCT = 75, BANG = 76, TILDE = 77, AMP = 78, PIPE = 79, CARET = 80, 
    LT = 81, GT = 82, QUESTION = 83, COLON = 84, SEMI = 85, COMMA = 86, 
    ELLIPSIS = 87, DOT = 88, VERBATIM_TEMPLATE_START = 89, VERBATIM_STRING = 90, 
    AT = 91, LPAREN = 92, RPAREN = 93, LBRACK = 94, RBRACK = 95, LBRACE = 96, 
    RBRACE = 97, TEMPLATE_START = 98, STRING_LIT = 99, CHAR_LIT = 100, FLOAT_LIT = 101, 
    INT_LIT = 102, IDENT = 103, LINE_COMMENT = 104, BLOCK_COMMENT = 105, 
    WS = 106, UNKNOWN_CHAR = 107, TEMPLATE_END = 108, TEMPLATE_INTERP_START = 109, 
    TEMPLATE_TEXT = 110
  };

  enum {
    TEMPLATE = 1, VERBATIM_TEMPLATE = 2
  };

  explicit HaoLangLexer(antlr4::CharStream *input);

  ~HaoLangLexer() override;


  std::string getGrammarFileName() const override;

  const std::vector<std::string>& getRuleNames() const override;

  const std::vector<std::string>& getChannelNames() const override;

  const std::vector<std::string>& getModeNames() const override;

  const antlr4::dfa::Vocabulary& getVocabulary() const override;

  antlr4::atn::SerializedATNView getSerializedATN() const override;

  const antlr4::atn::ATN& getATN() const override;

  // By default the static state used to implement the lexer is lazily initialized during the first
  // call to the constructor. You can call this function if you wish to initialize the static state
  // ahead of time.
  static void initialize();

private:

  // Individual action functions triggered by action() above.

  // Individual semantic predicate functions triggered by sempred() above.

};

