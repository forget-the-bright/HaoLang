# test/suite — 多文件集成测试套件

本目录是一个**多文件项目**（`package main`，一个 `main()`），覆盖 HaoLang 全部语言特性
与标准库/系统库。`script/test.sh` 只编译并运行这一个项目，输出总行数即测试基线。

## 工作流（做新特性时）

1. 开发特性时写**单文件**临时验证（如 `test/x.hao`）。
2. 验证通过后，把用例**合并进对应的模块文件**（见下表），命名加模块前缀防同包碰撞。
3. 单文件移入 `test/oldcase/`（不再测试，仅归档）。
4. 跑 `bash script/test.sh --rebuild-all`，确认基线行数不减少、退出码 0。

> 注意：`package main` 每包只允许一个 `main()` 和一个 `init()`。跨包 init 顺序用
> `lib/` 子包验证（lib.init 先于 main.init）。

## 文件 → 覆盖范围

| 文件 | 覆盖（原单文件） |
|------|------------------|
| features.hao | 基础/变量/算术/字符串/if/while/Unicode（hello, features, zh） |
| functions.hao | 函数/递归/重载/函数作值（overload） |
| arrays.hao | 数组/动态扩容/pop/when（arrays, dynarr） |
| classes.hao | 类/字段/构造/方法（classes） |
| oop.hao | 继承/接口/多态/super/is-as（interfaces, inherit, typecheck, extends_impl, super_ctor） |
| generics.hao | 泛型类/函数/嵌套（generic） |
| lambdas.hao | 闭包/高阶函数/hof（lambda, hof） |
| collections.hao | List/Map/Set/Queue/Stack/for-in/对象key/泛型方法（collections, map, set_queue, stack, foreach_collection, objectkey, generic_method） |
| object.hao | Object 根/hashCode/equals/toString（object_root） |
| nullsafe.hao | T?/?.?/!!（nullsafe） |
| exceptions.hao | try/catch/finally/throw + Exception（except, exception） |
| statics.hao | static 成员/静态构造器/enum（static, static_ctor, enum） |
| extern.hao | extern C 函数声明（extern_fn） |
| gcv3.hao | GC v3 正确性：静态根 / remset 跨 minor / by-ref 屏障（v0.35.1） |
| haoroutine.hao | haoroutine + channel（v0.36） |
| system.hao | os/sync/net（os, sync, net） |
| init.hao | 包级 init()（init_func, initpkg） |
| packages.hao | 跨包 import/通配/别名/init 顺序（pkgdemo, pkgshapes, initpkg） |
| calc/ | import + 通配子包 |
| shapes/ | import as + 跨包类/接口子包 |
| lib/ | 跨包 init 顺序子包 |

## 已归档（test/oldcase/）

gc.hao（GC 压力测试，做 GC 特性时加回）、以及全部旧单文件测试。