
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

