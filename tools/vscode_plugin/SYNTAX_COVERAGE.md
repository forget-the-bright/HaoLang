# HaoLang TextMate 覆盖对照（Lexer → scope）

权威：[`src/ast/HaoLangLexer.g4`](../../src/ast/HaoLangLexer.g4) + [`docs/hao语法.md`](../../docs/hao语法.md)。  
目标：打开 `test/syntax.hao` / `test/suite/collections.hao` 时无「大片白字关键字」。

## 关键字

| Lexer token | 字面 | TextMate scope（约） |
|-------------|------|-------------------|
| PACKAGE/IMPORT/EXTERN | package import extern | `keyword.other.package.hao` |
| FUNC/CLASS/INTERFACE/ENUM/CONSTRUCTOR/VAL/VAR/DELEGATE | … | `storage.type.hao` |
| EXTENDS/IMPLEMENTS/WHERE | … | `storage.modifier.inheritance.hao` |
| NEW | new | `keyword.operator.new.hao` |
| PUBLIC…ASYNC | public private … async | `storage.modifier.hao` |
| IF…DEFAULT / HAOROUTINE | if else … haoroutine select case default | `keyword.control.hao` |
| THIS/SUPER | this super | `variable.language.hao` |
| IS/AS/IN | is as in | `keyword.operator.expression.hao` |
| TRUE/FALSE/NULL_LIT | true false null | `constant.language.*.hao` |

## 运算符（多字符优先）

| Lexer / 计划 | 字面 | 备注 |
|--------------|------|------|
| QQ_ASSIGN / QQ | `??=` `??` | 已覆盖 |
| ARROW | `->` | 已覆盖 |
| *_ASSIGN | `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` | 已覆盖 |
| INCR/DECR | `++` `--` | 已覆盖 |
| EQ/NEQ/LE/GE/LSHIFT/AND_AND/OR_OR | `==` `!=` `<=` `>=` `<<` `&&` `\|\|` | 已覆盖 |
| ELLIPSIS | `...` | 可变参数 / 展开 |
| 单字符 | `= + - * / % ! ~ & \| ^ < > ? : ; , .` | 已覆盖 |
| 计划增补（当前 Lexer 无独立 token） | `?.` `!!` `>>` `>>>` | TextMate 仍着色，便于文档/未来语法；`>>` 在 Lexer 中常为两个 `GT` |

## 字面量与字符串

| Lexer | 覆盖 |
|-------|------|
| FLOAT_LIT / INT_LIT（十进制/hex/`0b`） | ✅ |
| CHAR_LIT | ✅ |
| STRING_LIT | ✅ |
| VERBATIM_STRING `@"` | ✅ |
| TEMPLATE `$"` + `{…}` | ✅（插值切 `$self`） |
| VERBATIM_TEMPLATE `@$"` / `$@"` | ✅ |
| LINE_COMMENT / `///` / BLOCK_COMMENT | ✅ |

## 结构形态（repository）

| 形态 | 规则 |
|------|------|
| `[T]` 数组类型 | `#types-and-generics` |
| `T?` 可空 | nullable / `?` |
| `Foo<Bar>` / `? extends` / `? super` | generic begin |
| `Action` / `Func` | builtin-types |
| 具名实参 `name:` | `#named-args` |
| 注解 `@Name` / `@Name(...)` | `#annotations` |
| `{ get; set; }` / `OnChange` | `#auto-props` |
| `T...` | varargs 捕获 |
| `func name` / 调用 `name(` | `#functions` |
| 内置类型名 | Int Long String Bool Byte Array Object Float Double Char Unit … |

## 验收样例

- `test/syntax.hao`
- `test/suite/collections.hao`
- 含 `$"…"`、`when`、`haoroutine`、`new [Int]{}` 的源码
