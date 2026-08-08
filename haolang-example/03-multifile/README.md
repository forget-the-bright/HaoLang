# 03 · 多文件本地包

入口 `main.hao` 与同级目录包 `calc/`、`shapes/`。`hao` 以入口文件所在目录为搜索根解析 `import`。

## 目录

```text
03-multifile/
  main.hao
  calc/calc.hao      # package calc
  shapes/shapes.hao  # package shapes
```

## 运行

```powershell
hao run haolang-example\03-multifile\main.hao
```

预期输出（约）：

```text
5
20
3.14159
圆
15.14159
```
