# HaoLang 语言语法与语义说明

> **读者**：语言使用者、从 Go / Java / C# 迁移的开发者  
> **版本**：随根目录 `VERSION`；能力面见 [`../记忆文档.md`](../记忆文档.md) 第 3 章；GC 详文 [`IR与GC契约.md`](IR与GC契约.md)  
> **从哪进来**：项目首页 [`../README.md`](../README.md) 第四节（摘要）链到本文；命令 / 包管理见 [`hao命令.md`](hao命令.md)。  
> **本文侧重**：规则、原理、限制与对标 Go / Java / C#（**不写**完整 CLI）。  
> **类型生命周期属性表**（GcManaged / IsReference / …）：[`类型属性.md`](类型属性.md)。  
> **反向门禁**：`script/win/negcheck.ps1`（须 28/28）。设计决策见 [`记忆文档.md`](../记忆文档.md) 第 5 章；特性/库速查见第 3～4 章。

---

## 1. 语言定位

HaoLang（好语言）是**静态类型、编译至原生机器码**的语言：

| 维度 | 选择 |
|------|------|
| 执行模型 | AOT → LLVM IR → clang/lld → 单文件原生可执行文件 |
| 内存 | 自带 GC（可达性主路径 + 精确根/spill/皮带 + 混合屏障 + 软 STW + mark worker；详文 [`IR与GC契约.md`](IR与GC契约.md)；能力面随 `VERSION`） |
| 包模型 | **目录即包**（Go 风格），清单用 `haoproject.json` |
| 并发关键字 | **`haoroutine`**（禁止称 goroutine） |
| 泛型 | **单态化**（C++/Rust 路线，非 JVM 擦除） |
| 空安全 | 一等 `T?` + `?.` / `??` / `!!` + smart cast（Kotlin 味） |
| OOP | 类/接口/继承/虚表 + `Object` 根（Java/C# 味） |
| 函数式 | lambda / 闭包 / `Func`·`Action`（C# 味） |

**不是**：解释型语言、字节码 VM、或 Go 的 CSP 全套（channel 载荷目前固定 Long）。隐式装箱/拆箱对标 Java（仅匹配对；`T?` 可空与包装类正交）。

---

## 2. 程序结构

### 2.1 编译单元

- 源文件后缀 **`.hao`**。
- 每个文件以 `package <名>;` 开头（建议）。
- **同一目录**内所有 `.hao` 的 `package` 名必须一致 —— **目录即包**。
- 整包（及 import 闭包）一次编译生成一份 IR（非逐文件独立目标再链，增量编译属后续）。

### 2.2 入口

```hao
package main;

func main() { }
// 或
func main(args: [String]): Int { return 0; }
```

| 规则 | 说明 |
|------|------|
| `main` 包 | 可执行程序入口包名习惯为 `main` |
| 参数 | `args` **不含**程序名（对齐 Java `main(String[])` / C# `Main(string[])`；**不同于** C `argv[0]`） |
| 返回 | `Unit` 或 `Int`；`Int` 作为进程退出码 |
| `init` | 每包至多一个无参 `func init()`；在 `main` 前按依赖顺序执行（对齐 Go） |

### 2.3 与对标语言对照（结构）

| Hao | Go | Java | C# |
|-----|-----|------|-----|
| `package x;` + 目录 | `package x` + 目录 | `package x;` + 目录树 ≠ 强制同名目录策略不同 | `namespace`（逻辑）+ 项目文件 |
| `func main` | `func main` | `static void main` | `static void Main` |
| `func init()` | `func init()` | 静态块 / 无直接对应 | 静态构造器（类型级） |
| `import a;` / `a.*` / `as` | `import` / `.` | `import` / `.*` | `using` / 别名 |

---

## 3. 词法与书写约定

| 项 | 规则 |
|----|------|
| 注释 | `//` 行注释；`/* */` 块注释（以文法为准） |
| 标识符 | 字母/下划线开头；**用户标识符勿用 `$`**（编译器单态化名用 `$`） |
| 语句结束 | 常见处分号 `;`（类 Java/C#；不同于 Go 的自动插入习惯） |
| 字符串 | `"…"` 转义；`$"…{expr}…"` 模板；`@"…"` 逐字；`@$` / `$@` 逐字+插值 |
| 字符 | `'好'` → **`Char`**（Unicode **码点**，底层 i32） |
| 关键字摘录 | `val` `var` `func` `class` `interface` `enum` `haoroutine` `when` `try`… |

---

## 4. 类型系统

### 4.0 类型生命周期分类（值 / 引用 / 裸指针）

语言在**生命周期与 GC**上固定三类（详表见 [`类型属性.md`](类型属性.md)）：

| 分类 | 语言例子 | 要点 |
|------|----------|------|
| **值类型** | `Int`/`Long`/`Bool`/`Char`/`Float`… | 复制即独立；赋值不写屏障；非堆指针 |
| **引用类型** | `String`、`[T]`、`class`、`interface`、`Func` | 语言值是托管堆指针；局部挂根；堆写须屏障 |
| **原生不安全指针** | （无正式一等类型；目标为句柄内藏针） | **不**参与 GC；禁止出 FFI 桥；见类型属性表 §4～§5 |

补充（易混点）：

- **`String`** 是引用类型，不是值类型；`String?` 的 null 是指针 0。  
- **`[T]`**：数组**对象**永远是引用；元素是值还是引用取决于 `T`（对标 Java `int[]` / `String[]`）。  
- **值类型 `T?`（如 `Int?`）**：语义是可空值，实现装箱进堆（GcManaged），**语言分类上仍不是引用类型**。  
- **FFI**：C 字符串须拷贝为 Hao `String`；C 资源指针须 **`NativeHandle`**（已落地：fs/net/regex）——见 [`方法论/FFI与运行时分层隔离.md`](方法论/FFI与运行时分层隔离.md)。

完整对照 `TypeKind`、隐式判定 vs 一等属性、去 C 化路线：**仅** [`类型属性.md`](类型属性.md)。  
装箱 / 转型 / 通配正式语义：**仅** [`RFC/`](RFC/)（0001～0003）。

### 4.1 内建数值与逻辑

| 类型 | 位宽/表示 | 有无符号 | 备注 |
|------|-----------|----------|------|
| `SByte` | i8 | 有 | |
| `Byte` | u8 | 无 | 0～255 |
| `Short` / `UShort` | i16 / u16 | | |
| `Int` / `UInt` | i32 / u32 | | **默认整数字面量常落到 Int** |
| `Long` / `ULong` | i64 / u64 | | |
| `UIntPtr` | u64 | 无 | 本宿主固定 64；指针宽算术 |
| `Float` / `Double` | f32 / f64 | | |
| `Bool` | **存 i8（0/1）** | | 条件用 i1；**勿当 C 的 1-bit 存储模型理解** |
| `Char` | i32 码点 | | 与 Int 同宽但语义不同；`println` 有专用重载 |
| `String` | ptr → 堆头 | | 头+`[Byte]` UTF-8；`.length`/`[i]` 码点；`byteLength`/`getBytes` |
| `Unit` | （无值） | | 类似 void / Go 的无返回 |

**原理要点**

- LLVM 同宽有符号/无符号底层相同；差异在 zext/sext、`icmp` u*、udiv/urem、字面量域。
- **提升**：同宽有符号+无符号 → 更宽有符号（如 `UInt+Int`→`Long`）；已 64 位有符号与无符号**禁止隐式混合**（须显式转换；negcheck）。
- **移位**：结果类型 = 左操作数一元提升；32 位掩码 `&31`，64 位 `&63`；`>>` 有符号 ashr / 无符号 lshr。
- **有符号除/%**：除零 panic；`MIN/-1` 有防毒值处理。

### 4.2 包装类（`lang`，隐式预导入）

三种形态必须分清：

| 形态 | 示例 | 用途 |
|------|------|------|
| 内建值 | `Int`、`String` | 日常计算 |
| 可空装箱 | `Int?` | 空安全；编译器 `hao_box_*` |
| 包装对象 | `Integer`、`lang.String` | 需要 `Object` / 内容 equals 时 |

- **有** Java 式隐式自动装箱/拆箱（仅匹配对）：`Int`↔`Integer`、`Long`↔`lang.Long` 等；`Object o = 10` ≡ 装箱到对应包装再向上。显式仍可用 `Integer.valueOf` / `x as Integer` / `box as Int`。
- **禁止** 数值拓宽与装箱合并（`Int` 不能一步赋给 `lang.Long`）；`Object o = 10; o as Long` 运行时 panic（实际是 Integer）。
- `T?`（`hao_box_*`）仍是可空值形态，与包装类正交。
- `new Int(1)` 非法。
- 数值包装提供 `SIZE` / `BYTES` / `MAX_VALUE` / `MIN_VALUE` 等。
- **`Bit`**：位模式工具（≠ `Boolean`）；`getBit`→`Bool`；静态 API 对 Int/Long 重载。

### 4.3 复合类型

| 写法 | 含义 |
|------|------|
| `[T]` | 数组（定长或动态扩容语义见 §7） |
| `[T]?` | 可空数组引用；**禁止**展开 `[...nullableArr]` |
| `T?` | 可空 |
| `(T)->R` | 函数类型 |
| `Func<…T, R>` / `Action<…>` | 预置别名（C# 风格；最后一参为返回类型） |
| `ClassName` / `pkg.Class` | 名义类型；跨包用限定名 |

### 4.4 可空与空安全（核心语义）

```hao
var s: String? = null;
fmt.println(s?.length ?? -1);
val forced = s!!;          // null → panic
```

| 规则 | 说明 |
|------|------|
| 引用类型 null | 指针 0，零开销 |
| 值类型 `T?` | 装箱为指针 |
| 直接 `.` | 对可空接收者**编译拒绝**；须 `?.` 或 `!!` |
| 条件 / 下标 | **禁止**可空直接用于 `if/while/when` 条件与下标 |
| 算术 / 模板 / `+=` | 可空须先 `!!` / `??` / smart cast |
| `??` | 右侧须与左侧底层兼容并 coerce |
| 异宽可空赋值 | 如 `Int?`→`Long?` **拒绝** |
| 装箱 `==` | 同 kind 比载荷；`String?` 走字符串相等；异宽可空比较拒绝 |
| smart cast | `if (x != null)` / `while` / `if (x == null) return` 后局部收窄；`var` 再赋值使收窄失效 |
| `Func?` / `Action?` | **字段禁止**；用非空委托 + 标志位 |

### 4.5 与对标语言对照（类型）

| 概念 | Hao | Go | Java | C# |
|------|-----|-----|------|-----|
| 默认整数 | `Int` (32) | `int` (平台)/显式 | `int` | `int` |
| 值 / 引用 | 值类型 vs 引用（§4.0）；无正式裸指针类型 | 值语义为主 + 指针显式 | 原始类型 vs 引用类型 | 值类型 vs 引用类型 |
| `int` / `String` / `int[]` | `Int` / `String` / `[Int]`（数组对象是引用） | `int` / `string` / `[]int` | `int` / `String` / `int[]` | `int` / `string` / `int[]` |
| 字符串长度 | **码点** | 字节（`len`）/ rune | UTF-16 code unit | UTF-16 |
| 可空 | `T?` 一等 | 指针 / 多返回 | 引用默认可 null + Optional | `T?`（可空值类型/引用注解） |
| 装箱 | 自动装箱+显式 / `T?` | 少用装箱 | 自动装箱 | 装箱+可空 |
| 泛型实现 | 单态化 | 近似字典/特例 | 擦除 | 再化/共享码（值类型特例） |
| 根类型 | `Object` | 无统一根 | `Object` | `object` |

---

## 5. 变量、常量与推断

```hao
val name = "Hao";       // 不可变绑定
var count: Int = 3;     // 可变；可显式标注
val ratio = count + 0.5; // 推断为 Double（提升）
```

| 规则 | 说明 |
|------|------|
| `val` / `var` | 对齐 Kotlin；类似 C# `var`（仅局部推断）+ 可变性区分 |
| 类型标注 | `name: Type` |
| 无隐式窄化写回 | 提升后的值写回窄类型需合法转换路径 |

---

## 6. 运算符与表达式

### 6.1 算术 / 位 / 逻辑

- 算术：`+ - * / %`（字符串 `+` 为拼接）
- 位：`& | ^ << >>`、一元 `~`；复合赋值 `&= |= ^= <<= >>= += …`
- 逻辑：`&& || !` 短路
- 比较：`== != < > <= >=`；异型（如 `Bool==Int`）编译拒绝
- 条件：`cond ? a : b`

### 6.2 字符串

- `"a" + x` 拼接（可空参与前须非空）
- `$"两倍={n * 2}"` 模板插值
- `"你好"[0]` → `Char`；`.length` 码点数
- FFI 用底层 `data`（nul 结尾）时走运行时约定，勿假设与码点下标相同

### 6.3 `when`

```hao
when (k) {
    1, 2 -> fmt.println("一二");
    else -> fmt.println("其他");
}
val s = when (k) { 1 -> "一"; else -> "多" };
```

| 规则 | 说明 |
|------|------|
| 语句 when | `else` 非强制 |
| **表达式 when** | **必须有 else**（否则 alloca 未写 → 毒值） |
| 主体类型 | 跨宽度整数须提升后再比较；浮点用 `fcmp` |
| 能力 | 多值、无主体、字符串主体、可嵌模板串 |

对标：Kotlin `when` / C# `switch` 表达式；强于传统 Java `switch`（至现代 switch 表达式前）。

---

## 7. 数组

```hao
val a = new [Int]{ 1, 2, 3 };   // 必须 new（v0.60.3）
var xs: [Int] = new [Int]{};
xs += 10;                       // 动态 push
fmt.println(xs.pop());
val b = new [Int](n);           // 定长
val c = new [Int](n, fill);
val d = new [Int]{ ...a, ...b }; // 展开须在 new [T]{…} 内
Array arr = a;                  // [T] <: Array <: Object
fmt.println(arr.length);
fmt.println(arr.capacity);
val e = a.clone();
```

**规则（RFC-0005）**

- 裸 `[1,2,3]` / `[]` **非法**；实例必须经 `new`。
- `[T] → Array → Object`；`Array` 无下标/`pop`；索引仅 `[T]`。
- 算法工具：`collections.*`（泛型 `[T]`）；布局与扩容仍为运行时 C。

可变参数见 [RFC-0006](RFC/0006-可变参数.md)：`func Sum(values: Int...)`。

---

## 8. 控制流

```hao
if (x > 0) { ... } else { ... }
while (x < 10) { x += 1; }
for (x in arr) { ... }      // 数组或 Iterable<T>
break; continue;
```

- `for-in`：数组直接迭代；集合走 `collections.Iterable` / `Iterator` 虚分派。
- 循环条件与**函数/lambda 入口** IR 插入 **`hao_gc_safepoint`**（协作 GC；详文 [`IR与GC契约.md`](IR与GC契约.md)）。
- 条件禁止可空类型直接充当 Bool。

---

## 9. 函数

```hao
func fib(n: Int): Int {
    return n <= 1 ? n : fib(n - 1) + fib(n - 2);
}

func area(w: Int): Int { return w * w; }
func area(w: Int, h: Int): Int { return w * h; }  // 重载
```

| 能力 | 状态 |
|------|------|
| 重载 | ✅ 按参数个数/类型；IR 名带签名后缀 |
| 提升匹配 | ✅ 如 Int→Double 计分 |
| 函数值 / 高阶 | ✅ |
| 泛型函数 | ✅ 实参推断；单态化 |
| 具名实参 | ✅ `name: expr`；位置前缀+具名后缀；见 [RFC-0004](RFC/0004-具名实参.md) |
| 默认参数 | 以文法/实现为准；勿假设完整 |

**可见性（顶层）**：默认 public；`private` 包内。

---

## 10. 面向对象

### 10.1 类与接口

```hao
interface Shape { func area(): Double; }
abstract class Animal {
    var name: String = "";
    abstract func speak(): String;
}
class Dog extends Animal implements Shape {
    constructor(name: String) { super(name); }
    override func speak(): String { return this.name + ": 汪"; }
    override func area(): Double { return 0.5; }
}
```

| 规则 | 说明 |
|------|------|
| 根类 | 默认继承 `object.Object`（`hashCode`/`equals`/`toString` 身份语义，可覆写） |
| 继承语法 | 推荐 `extends` / `implements`；旧 `class D : A, I` 仍兼容 |
| 分派 | 虚表；接口多实现 |
| `is` / `as` | 运行时类型判定；`as` 失败 panic |
| 可见性 | `private` 类内 / `protected` 子类 / `internal` **同 importPath** / `public` 默认 |
| `super` | 方法与构造链 |

### 10.2 静态成员与枚举

```hao
class Counter {
    static var count: Int = 0;
    static Counter() { Counter.count = 42; }  // 惰性一次
}
enum Color { RED, GREEN, BLUE }
```

- 静态构造器：类首次被引用前执行（对齐 C# 静态构造思想）。
- `enum`：常量是静态实例；`name()` / `ordinal()`；禁止 `new Color()`。

### 10.3 注解

```hao
@interface Info { val author: String; }
@Info("alice")
class Person { }
```

- 定义用 Java 风格 `@interface`；使用 `@Name(args)`。
- 反射可读；MVC 使用 `@Controller` / `@GetMapping` 等。
- **v0.50**：注解身份用 `GetMapping.Class` 等令牌比对（`isAnnotationPresent`），勿再靠短名字符串后缀。

### 10.3b Class 令牌（v0.50～v0.50.2，对标 Java `Class`）

```hao
import reflect;
val c = Foo.Class;                 // 每类型合成的 static Class 常量
val d = (new Foo()).getClass();  // Object.getClass()
fmt.println(c == d);               // 同类型同一 Class 单例（指针相等）
fmt.println(c.isAnnotationPresent(Controller.Class));
```

- 编译器为每个类合成 `static Class: reflect.Class`（对标 Java 类字面量；属 Object 类型体系 API，**不是**共用 `Object` 上一个 static）。
- `TypeName.Class` / `getClass` / `typeOf` / `typeAt` / `classForName` 返回**每类型单例**。勿手搓 `new Class()` 当令牌。
- 实例方法可按参数签名重载（与 static/顶层一致）；完全相同签名才报重复定义。

### 10.4 对标

| Hao | Java | C# | Go |
|-----|------|-----|-----|
| 单继承+多接口 | 同 | 同 | 无类继承；接口组合 |
| `Object` 根 | 同 | `object` | 无 |
| `override` | `@Override` | `override` | 无 |
| `internal` | 包可见近似 | `internal` 程序集 | 小写导出规则不同 |

---

## 11. 泛型

```hao
class Box<T> { var value: T; constructor(v: T) { this.value = v; } }
func first<T>(a: [T]): T { return a[0]; }
interface Iterable<T> { func iterator(): Iterator<T>; }
```

**原理**：单态化 —— 每个实参组合生成专用代码（`Box$Int`），零虚表擦除开销。

| 支持 | 限制 |
|------|------|
| 泛型类/函数/方法/接口 | 支持 `where T : Constraint`（类/接口/函数；无界约束） |
| 嵌套泛型 | 泛型函数当值需先实例化 |
| 接口单态化 `Iterable$Int` | 无 Java 式原始类型 |
| 通配 `?` / `? extends T` / `? super T` | PECS 赋值与 `add`/`get`；不做声明处 `in`/`out`；完整 capture conversion 边角见坑债 |

---

## 12. Lambda、委托与方法组

```hao
val f: Func<Int, Int> = { x -> x * 3 };
val g: Action<Int> = { x -> fmt.println(x); };
val h: Func<String, Int> = calc.add;   // 方法组：绑定 this
delegate (Int)->Int IntOp;
```

| 写法 | 含义 |
|------|------|
| `{ stmt; }` | 无参 |
| `{ it * 2 }` | 单参隐式 `it` |
| `{ x, y -> x + y }` | 多参 |
| 捕获 | `val` 按值；`var`/可变 → 堆 cell 共享 |

**禁止**：`Func?`/`Action?` 作字段类型。

对标：C# `Func`/`Action`/委托；Java `Function`；Go 函数值（无方法组语法糖）。

---

## 13. 异常

```hao
try {
    throw new exception.Exception("boom");
} catch (e: exception.Exception) {
    fmt.println(e.getMessage());
} finally {
    // 总会执行
}
```

| 规则 | 说明 |
|------|------|
| 实现 | setjmp/longjmp 帧（非 DWARF EH） |
| 匹配 | 按虚表类型；子类可被父类型 catch |
| `throw` | **拒可空**异常值 |
| 未捕获 | 打印 panic，退出码 1 |
| `return`/`break`/`continue` | 穿过 `finally` 时先执行 finally |

对标：Java/C# 的 try/catch/finally 模型；**不是** Go 的 `error` 多返回值惯例（Hao 亦可自行返回可空/结果类型，但语言级是异常）。

---

## 14. 包与 import

```hao
import calc;
import calc.*;
import shapes as s;
```

| 规则 | 说明 |
|------|------|
| 目录 | `import a/b` ↔ 子目录 `a/b`；**目录即包**，同目录 `package` 名须一致 |
| 查找顺序 | 入口树 → `localReferences` → **依赖 cache**（`hao mod tidy`）→ `stdlib/src` |
| 顶层符号 | 默认导出；`private` 包私有 |
| 跨包静态 | `pkg.Class.member` / `pkg.Class.m()` |
| `internal` | **同 importPath**，跨包编译拒绝 |
| 预导入 | `fmt`、`lang.*`、`object` 相关等无需手写 import |

### 14.1 项目清单与依赖（语言侧要点）

源码里的 `import utilpkg` **短名**由包目录 / `package` 声明决定；清单里的依赖键是**完整模块路径**（如 `example.com/demo/utilpkg`）。二者是两套概念。

```json
{
  "project": { "name": "app", "module": "example/app", "main": "main.hao" },
  "dependencies": {
    "example.com/demo/utilpkg": "^1.0.0"
  },
  "localReferences": ["../common"]
}
```

| 概念 | 说明 |
|------|------|
| `haoproject.json` | **工程清单**（应用根；**不用** `hao.mod`） |
| `haopkg.json` | **发布包元数据**（仓内 `<module>/<version>/`；与工程清单不同文件） |
| `dependencies` | 写在工程清单上；精确版或 `^` / `~` / `>=` / `=`；tidy 解析为精确版写入 lock |
| 传递依赖 | 被依赖包的 `haopkg.json` → `dependencies` 一并解析；冲突硬失败 |
| `localReferences` | 本地工程互引（对标 ProjectReference），不进 cache，**不要求**对方有 `haopkg.json` |
| `exclude` | 从图中排除某模块（勿 exclude 仍被 `import` 的包） |
| stdlib | 永远随工具链，**不走** registry |

二者区别详见 [`hao命令.md`](hao命令.md) **§4.0**。命令、仓库协议、本地私服见同文档 §4。

---

## 15. 并发：`haoroutine` 与 `channel`

```hao
import channel;
val ch = channel.make(4);
haoroutine {
    ch.sendInt(42);
}
select {
    case n = ch.recvInt():
        fmt.println(n);
    default:
        fmt.println(0);
}
ch.trySendInt(1);
```

| 规则 / 限制 | 说明 |
|-------------|------|
| `haoroutine { }` | **无参**体；火即忘；带参写法编译拒绝 |
| channel 载荷 | 当前以 **Long** 为主；`sendStr`/`recvStr(): String?` |
| `trySend`/`tryRecv` | 非阻塞 |
| `select` | `case` 接收/发送 + 可选 `default`；**真等待**：先 `try_*`；有 `default` 全失败立即 default；无 `default` 登记各 chan 后 park（`hao_chan_select` / os_block），对端 send/recv/close 唤醒 |
| 公平 | TLS 轮转起始下标 |
| 未实现 | 泛型 channel 载荷；完整 Go memory model |

对标：Go `go` / `chan` / `select` —— **语义子集 + 不同关键字与载荷模型**。勿假设与 Go 内存模型/调度器一一对应。

---

## 16. 反射与动态

```hao
import reflect;
val t = Foo.Class;                 // 或 obj.getClass() / typeOf(obj)
t.getField(obj, "age");
// invoke：参数槽 [Long]，按签名编组
t.isAnnotationPresent(GetMapping.Class);
```

| 能力 | 状态 |
|------|------|
| 类型名/父类/接口/字段/方法/注解 | ✅ |
| `TypeName.Class` / `getClass` / Class 单例 | ✅（v0.50.1） |
| 注解按 Class 令牌匹配 | ✅（v0.50） |
| 字段读写（基本类型与 String） | ✅ |
| 通用 `invoke` / `invokeFloat` | ✅ |
| 运行时动态定义类 | ⬜ 不做将就 |

对标：Java Reflection / .NET Reflection；能力弱于二者完整 API 面。

---

## 17. 与 C 互操作

```hao
extern func printInt(x: Int) = "hao_println_int";
extern func ntohs(x: Int): Int = "ntohs" @link("ws2_32");
```

- 无函数体；`= "c_name"` 指定链接符号。
- 链接：`@link` / `-l` `-L` `--link` / `HAO_LDFLAGS`。
- 未解析符号在**链接期**报错（预期）。
- extern 不宜泛型。

---

## 18. 特性矩阵（相对 Go / Java / C#）

| 特性 | Hao | Go | Java | C# |
|------|:---:|:--:|:----:|:--:|
| 静态类型 + AOT 原生 | ✅ | ✅ | 🔸(JIT/AOT可选) | 🔸 |
| GC | ✅ | ✅ | ✅ | ✅ |
| 目录包 + init | ✅ | ✅ | 🔸 | 🔸 |
| 空安全 `T?` | ✅ | 🔸 | 🔸 | ✅ |
| 类/接口/虚表 | ✅ | 🔸(接口) | ✅ | ✅ |
| 泛型单态化 | ✅ | 🔸 | ❌擦除 | 🔸 |
| 异常 try/catch | ✅ | ❌惯例 | ✅ | ✅ |
| CSP channel | 🔸子集 | ✅ | ❌ | 🔸 |
| 异步 async/await | ⬜ | 🔸 | 🔸 | ✅ |
| 宏/代码生成 | ⬜ | ✅ go gen | 🔸 | 🔸源生成 |
| 隐式装箱 | ✅ | — | ✅ | ✅ |

---

## 19. 文法已有、语义未完成（勿依赖）

| 特性 | 状态 |
|------|------|
| 具名实参 | ✅ |
| 强制 `new` 数组 / `Array` 基类 | ✅ v0.60.3 · [RFC-0005](RFC/0005-数组统一基类.md) |
| 可变参数 `T...` | ✅ v0.60.3 · [RFC-0006](RFC/0006-可变参数.md) |
| 自动属性默认值 / 自定义体 / OnChange | ✅ v0.60.3 · [RFC-0007](RFC/0007-自动属性增强.md) |
| 声明处变型 `in`/`out`（C#） | ❌ 明确不做（选 Java `? extends`/`? super`） |

**近期已交付（勿当缺口）**：v0.58 OOP；v0.59 装箱+PECS；v0.60 Arrays；v0.60.1 具名实参；v0.60.2 List API；**v0.60.3 Array/`T...`/自动属性**——详记忆文档第 3 章 / 时间线合订。

以记忆文档第 3 章特性表为准；上表未交付项实现前当作不存在。

---

## 20. 已知限制与注意事项（实战）

1. **用户程序默认 `-O2`**：Hao 帧靠 shadow 精确根；`-O2` 主要服务 os_block C 叶/无 shadow 路径。  
2. **表达式 `when` 必须 `else`**。  
3. **可空不参与算术/条件/下标** —— 用 negcheck 思维写代码。  
4. **`haoroutine` 无参**；channel 非泛型载荷。  
5. **`select` 真等待已交付**（无 default park；有 default 非阻塞）；泛型 channel 载荷仍未做。  
6. **无** `i128`/`u128`；对象字段槽仍偏宽（非紧凑打包）。  
7. **Map 满表**可抛异常；注意 `growthEnabled`。  
8. **跨包 `internal` 必拒**。  
9. **String 下标/长度是码点**；字节数用 `byteLength()` / `getBytes()`；HTTP `Content-Length` 等按 UTF-8 **字节**（库内已区分）。  
10. **发行**须带齐 stdlib 与 llvm/sysroot，不能只拷编译器。

完整限制摘要：README「已知限制」+ 记忆文档第 9 章；踩坑：[`坑债.md`](坑债.md)。

---

## 21. 单元测试（`testing` · v0.42）

对标 Go：`*_test.hao` + `func TestXxx(t: testing.T)`；由 `hao test` 发现并合成入口。  
详见 [`hao命令.md`](hao命令.md) §5 与记忆文档 **5.16**。

```hao
package main;
import testing;

func TestAnswer(t: testing.T) {
    t.eqInt(40 + 2, 42, "answer");
}
```

- 普通 `hao build` **不编译** `*_test.hao`。
- 业务 `main` 不是测试入口；集成基线仍用 `test/suite` + `script/test.sh`。

## 22. 推荐阅读顺序

1. 本文 §1～4、§14 —— 建立心智模型  
2. [`hao命令.md`](hao命令.md) —— 工具链与包管理、`hao test`  
3. README 第四节 —— 可运行示例  
4. `test/suite/` —— 行为规格级样例  
5. 记忆文档第 5 章 —— 若要改编译器/runtime  

---

## 附录 A · 最小可运行程序

```hao
package main;

func main() {
    fmt.println("你好, HaoLang");
}
```

```powershell
hao run hello.hao
```

## 附录 B · 常见拒识（编译期）

| 错误写法 | 原因 |
|----------|------|
| `var n: Int? = 1; n + 1` | 可空未解包 |
| `if (n) { }`（n 为 `Int?`/`Bool?`） | 条件禁可空 |
| `val y = when (k) { 1 -> 1 };` | 表达式缺 else |
| `haoroutine(1) { }` | 禁止带参 |
| `throw maybeEx`（可空） | throw 拒可空 |
| `[...arr?]` | 禁展开可空数组 |
| `Func?` 字段 | 禁止 |
| `Int?` 赋给 `Long?` | 异宽可空 |

## Socket 读契约（v0.49.4，对齐 Go / Java）

| 结果 | HaoLang | Go | Java |
|------|---------|----|------|
| 有数据 | `recv` → 非空 `String` | `n>0` | `read>0` |
| EOF | `recv` → 空串 | `io.EOF` | `-1` |
| 超时/错误 | `recv` → `null` | `error` | `IOException` / `SocketTimeoutException` |

另：`recvExact(n)` 对标 `io.ReadFull` / `readNBytes`；`ServerSocket.accept(): Socket?` 与出参版 `accept(Socket)` 重载并存；`setReadTimeout` / `setWriteTimeout`（`setTimeout` 仍同时设读写）。`lang.String.indexOf(sub, from)` 为码点起搜（v0.50.1）。

## HTTP / Netty OIO 子集（v0.50.3）

| 组件 | 作用 |
|------|------|
| `net.ByteBuf` | 连接入站累积；粘包时余量保留 |
| `Http.readOneMessage(sock, buf, …)` | 取出**一条**完整 HTTP 消息 |
| `Channel` / `ChannelPipeline` / `ChannelHandler` | 入站链；`fireChannelRead` 产出 `HttpResponse` |
| `EventLoopGroup` | worker 线程池 |
| `HttpServer.serveBossWorkers` / `HttpApp.serveBossWorkers` | boss 只 accept，worker 处理连接 |

对标 Netty **OIO**（阻塞 socket + boss/worker）。**NIO poll EventLoop** 尚未交付，勿假称全量 Netty。

