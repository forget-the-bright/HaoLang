# 07 · 单元测试（hao test）

`*_test.hao` 与被测文件同包；`hao test` 发现 `Test*` 并注入 `testing.T`。

## 目录

```text
07-testing/
  add.hao
  add_test.hao
```

## 运行

```powershell
# 跑测试
hao test haolang-example\07-testing

# 普通运行（不含 *_test.hao）
hao run haolang-example\07-testing\add.hao
```

预期：`hao test` 通过；`hao run` 打印 `3`。
