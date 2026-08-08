# 05 · 项目本地引用（localReferences）

本示例只有 **工程清单** `haoproject.json`（没有仓里的 `haopkg.json`）。  
`localReferences` 对标 C# ProjectReference：本地目录互引，不下载、不进本地仓。  
与发布包元数据的区别见 [`docs/hao命令.md` §4.0](../../docs/hao命令.md#40-haoprojectjson-与-haopkgjson-的区别)。

## 目录

```text
05-project-local/
  app/haoproject.json   # localReferences: ../greeter
  app/main.hao
  greeter/hi/hi.hao     # package hi
```

## 运行

在仓库根：

```powershell
hao run haolang-example\05-project-local\app\main.hao
```

或进入 `app` 目录后 `hao run .` / `hao build .`（以工具是否支持目录入口为准；推荐带清单的入口文件路径）。

预期输出：

```text
hello-from-greeter
```
