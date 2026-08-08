# 06 · haopkg 依赖（迷你本地仓）

同时出现两种清单：

- **`app/haoproject.json`**：工程清单（声明 `dependencies`）
- **`packages/.../haopkg.json`**：发布包元数据（仓内某一版本）

二者区别见 [`docs/hao命令.md` §4.0](../../docs/hao命令.md#40-haoprojectjson-与-haopkgjson-的区别)。  
本目录自带迷你 `packages/`，**不必**用仓库里的 `repo/RegisterRepo`。

## 目录

```text
06-project-haopkg/
  packages/example.com/demo/greetpkg/1.0.0/
    haopkg.json
    greetpkg/greetpkg.hao
  app/
    haoproject.json
    main.hao
```

## 运行（开发树 · 仓库根）

```powershell
cd D:\buildLang
$env:HAO_REPO = (Resolve-Path examples\06-project-haopkg\packages).Path
hao mod tidy examples\06-project-haopkg\app
hao run examples\06-project-haopkg\app\main.hao
```

## 运行（发行包 · 解压后的包根）

注意：

1. 示例在 **`examples\`**（复数），不是 `example\`，也不是 `haolang-example\`。
2. 请先 **`cd` 到发行包根**（含 `bin`、`examples`、`stdlib` 的那一层），不要停在 `bin\` 里再写 `../example\...`。
3. PowerShell 下若未把 `bin` 加入 PATH，要用 **`.\bin\hao.exe`**（必须带 `.\`）。

```powershell
# 例如已解压到 D:\buildLang\target\win-amd64-haolang-0.48.0
cd D:\buildLang\target\win-amd64-haolang-0.48.0

$env:HAO_REPO = (Resolve-Path examples\06-project-haopkg\packages).Path
.\bin\hao.exe mod tidy examples\06-project-haopkg\app
.\bin\hao.exe run examples\06-project-haopkg\app\main.hao
```

预期输出：

```text
greetpkg-1.0.0
```

说明：

- `HAO_REPO` = 编译读取的本地仓；布局为 `<module路径>/<version>/haopkg.json` + 源码目录。
- 本示例关闭了默认远程源（`registry.includeDefault: false`），只吃本地已有包。
- 生成的 `haoproject.lock.json` 可提交；演示完可删。
