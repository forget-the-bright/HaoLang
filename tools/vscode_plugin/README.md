# HaoLang VSCode 插件（0.1.0）

Phase **0+1**：语法高亮（TextMate 全覆盖）+ language-configuration + snippets + `hao` CLI 集成。  
**不含**独立 LSP / Debug Adapter。

## 安装（开发）

```powershell
cd tools/vscode_plugin
npm install
npm run compile
```

在 VS Code / Cursor 中：**Extensions: Install from Location…** 选本目录，或按 `F5` 打开 Extension Development Host。

建议设置：

```json
{
  "haolang.executablePath": "D:/buildLang/output/hao.exe",
  "haolang.format.enableOnSave": false
}
```

工程清单为 **`haoproject.json` / `haopkg.json`**（不是 `hao.toml`）。

## 能力

| 能力 | 说明 |
|------|------|
| `.hao` 语言 ID `haolang` | 高亮、括号、折叠、缩进 |
| TextMate | 对照 Lexer；见 [SYNTAX_COVERAGE.md](./SYNTAX_COVERAGE.md) |
| Snippets | `package` / `func` / `class` / `when` / `newarr` / `try` / `test` …（真语法，无 `fn`/`struct`） |
| 命令 | Run / Build / Test / Fmt / Clean / Mod Tidy / Version |
| ProblemMatcher `$hao` | `路径:行:列: 错误:` / `警告:` |
| 格式化 | Document Formatting → `hao fmt`（stdout）；可选保存时 `hao fmt -w` |
| 状态栏 | 显示 `hao version` |
| `.hao` 文件图标 | 无底板；拟人「比好」拇指手势（渐变玻璃质感 + 蒸汽/爪环/水滴/星芒点缀）；仅语言 icon |

> 若资源管理器里 `.hao` 仍是普通文档图标：命令面板 → `Preferences: File Icon Theme`，选用支持语言图标的主题（如 **Seti (Visual Studio Code)** / **Minimal**）。切勿再装整套「只含 Hao」的主题，否则会盖掉其它语言图标。


## 语法权威

- [`src/ast/HaoLangLexer.g4`](../../src/ast/HaoLangLexer.g4)
- [`docs/hao语法.md`](../../docs/hao语法.md)
- 功能清单：[`docs/开发工具/…`](../../docs/开发工具/)

## 明确不做（本版）

- 独立 LSP / DAP
- 假 `hao check` / `hao doc`
- Test Explorer / 语义高亮 / Rename
