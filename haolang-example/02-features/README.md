# 02 · 语言与标准库特性

单文件示例，展示常用语法与 stdlib。

| 文件 | 内容 |
|------|------|
| `basics.hao` | 插值、可空、`when`、接口/类 |
| `collections.hao` | `List` / `Map` |
| `concurrency.hao` | `haoroutine` + `channel` |

## 运行

```powershell
hao run haolang-example\02-features\basics.hao
hao run haolang-example\02-features\collections.hao
hao run haolang-example\02-features\concurrency.hao
```

`basics` 预期含 `Hello HaoLang!`、`Point(1, 2)` 等行；`concurrency` 依次打印 `11` `22` `hi-chan` `99`。
