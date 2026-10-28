# VX 语言语法参考

本文档描述 C++ 重写工具链（`vxc` → `vlnk`）**实际支持**的 VX 语法，
与 `src/compiler/lexer.cpp` / `src/compiler/parser.cpp` / `src/compiler/codegen.cpp`
保持一致。自举参考实现位于 `compiler/*.vx`。

## 1. 词法

### 1.1 缩进与块

- Python 风格缩进敏感：空格计 1 列、Tab 计 4 列；编译期产出 INDENT/DEDENT token。
- 括号 `(` `[` `{` 内的换行与续行不产生换行/缩进 token。
- 空行与整行注释（`#` 起始）不参与缩进比较。
- 缩进不一致时报词法错误。

### 1.2 注释与语句分隔

```vx
# 整行注释
var x = 1    # 行尾注释
a(); b()     # ; 分隔单行多语句（括号内忽略）
```

### 1.3 字符串

- `"..."` 与 `'...'` 两种引号，支持转义 `\n` `\t` `\\` `\"` `\'`；
  未知转义保留原字符。
- 字符串可跨行；未闭合报 `E2002`。
- 标识符允许 UTF-8 非 ASCII 字节（可直接用中文变量名）。

### 1.4 数字

- 整数 `123`；浮点 `1.5`（`1.` 视为整数 `1` 后跟 `.` 运算符；
  `.5` 不构成浮点）。

### 1.5 关键字

```
if elif else for while break continue func return true false nil
var mut import struct class new extern int float bool void
and or not as in const export
```

软关键字（保留但在表达式位置可作变量名）：
`loop match enum union macro move`。

### 1.6 运算符

```
+  -  *  /  %  ^  ==  !=  <  >  <=  >=  =  +=  -=  *=  /=  ->
&&  ||  !  @  (  )  [  ]  {  }  :  ,  .  #
```

`&&`/`||` 词法上等价于 `and`/`or`。`@` 用于导入注解（`@imp` / `@cppimp`）。

## 2. 类型

内建类型：`int`（i32）、`float`（double）、`bool`、`string`、`void`、
`var`（未标注/泛型槽位）。类名/结构名可作类型标注。

## 3. 声明

### 3.1 变量

```vx
var x = 1          # 类型由初始化表达式推断
var y: int = 2     # 显式标注
mut z = 0          # mut：可变语义（当前与 var 等价）
const PI = 3       # 顶层 const 记入全局表
```

### 3.2 函数

```vx
func add(a: int, b: int) -> int:
    return a + b

func greet(name: string):      # 无 -> 视为 void
    out("hi " + name)
```

- 函数名允许关键字（如 `func new(...)`）。
- 无 `return` 的非 void 函数末尾自动补 `return 0`。
- 顶层 `var`/`const` 暂不生成运行时代码（记录为全局名）。

### 3.3 extern（外部符号）

```vx
extern out(msg: string)            # 无返回
extern func add(a: int, b: int) -> int   # func 关键字可选
```

内置运行时符号（无需 `extern` 声明，直接调用）：

| 符号 | 签名 | 说明 |
| --- | --- | --- |
| `out` | `out(msg: string)` | 输出字符串（不追加换行） |
| `out_int` | `out_int(v: int)` | 输出整数 |
| `out_bool` | `out_bool(v: bool)` | 输出布尔值 |
| `out_float` | `out_float(v: float)` | 输出浮点数（`%g`） |
| `puts` | `puts(s: string) -> int` | libc `puts` |
| `exit` | `exit(code: int)` | 以退出码结束进程 |
| `malloc` / `free` | `malloc(size: int) -> ptr` / `free(p: ptr)` | libc 内存管理 |
| `sys_argv_count` | `sys_argv_count() -> int` | 命令行参数个数（含可执行文件名） |
| `sys_argv_get` | `sys_argv_get(i: int) -> string` | 取第 i 个命令行参数（越界返回空串） |
| `file_read` | `file_read(path: string) -> string` | 读取整个文件；失败返回空串 |
| `file_write` | `file_write(path: string, content: string) -> int` | 覆写写入，返回字节数；失败 `-1` |
| `file_exists` | `file_exists(path: string) -> bool` | 文件是否存在（可读） |
| `system` | `system(cmd: string) -> int` | 执行 shell 命令，返回退出码 |

> 文件与参数能力由内置运行时（`vxrt`）提供，链接期并入 `libvxrt.a`。

### 3.4 类与结构

```vx
class Point:
    x: int
    y: int
    func new(x: int, y: int):    # 方法名可为关键字
        self.x = x
        self.y = y
    func get_x() -> int:
        return self.x

struct Pair:
    a: int
    b: int
```

- 方法生成为 `Class.method` 具名函数，隐式 `self` 占槽 0。
- 属性访问 `p.x` 编译为 FIELD_LOAD；属性赋值 `p.x = v` → FIELD_STORE。
- `new Point(1, 2)` → NEWZ + 构造调用。

### 3.5 模块与导入

```vx
import std.error            # 点分路径
import a.b.c as ab          # 别名（记录，暂未链接）
export lexer                # 导出声明
```

## 4. 库导入注解（@imp / @cppimp）

```vx
@imp(math)                    # headers 根目录库
@imp(/collections/vec)        # 子目录库，库名取末段 vec
@imp(./mylib)                 # 相对源文件目录
@cppimp(cpp/greet.hpp)        # C++ 头文件（链接期生成 C++→VX ABI shim）
@imp(math) as m               # 可选别名

func main() -> int:
    return math.add(1, 2)     # 导入符号以库名前缀限定访问
```

- 库查找根：`vxc` 同级 `headers/`（`VX_HEADERS` 环境变量可覆盖）。
- 库文件 = `.vxh` 声明头 + 同名 `.c`/`.a`/`.dylib`/`.cpp` 实现；
  `@cppimp` 的签名取同目录同主名 `.vxh` 伴生文件。
- 符号命名 VX ABI：`_vx_<库名>_<符号名>`。
- 详见 README「库导入与 VX ABI」。

## 5. 语句

### 5.1 条件

```vx
if cond:
    ...
elif other:
    ...
else:
    ...
```

### 5.2 循环

```vx
while i < 10:
    i = i + 1

for x in xs:               # 迭代（降级为 __for_iter/__for_idx 计数器）
    out(x)

break / continue           # 仅循环内合法
```

### 5.3 赋值

```vx
x = 1                      # 简单赋值
x += 2                     # 复合：+= -= *= /=
a[0] = 5                   # 下标赋值 → INDEX_SET
p.x = 1                    # 属性赋值 → FIELD_STORE
```

## 6. 表达式

### 6.1 字面量

```vx
42          3.14        "text"      'text'
true        false       nil
[1, 2, 3]               {"k": 1, "v": 2}    # 列表/字典字面量
```

### 6.2 运算符优先级（低 → 高）

| 优先级 | 运算符 |
| ------ | ------ |
| 1 | `or` / `\|\|` |
| 2 | `and` / `&&` |
| 3 | `==` `!=` |
| 4 | `<` `>` `<=` `>=` |
| 5 | `+` `-` |
| 6 | `*` `/` `%` |
| 7 | `^`（整数幂，右结合） |

一元：`-`（取负）、`not` / `!`。`^` 由运行时 `vx_pow_i32` 提供（负指数按 0）。

### 6.3 后缀链

```vx
f(1, 2)             # 调用
obj.method(x)       # 方法调用：接收者入栈 + CALL m;method
xs[0]               # 下标 → INDEX_GET
obj.field           # 属性 → FIELD_LOAD
new ClassName(a)    # 构造
vec.push(x)         # 导入库调用 → CALL f;_vx_<lib>_<name>
```

### 6.4 逻辑运算与比较语义

- `and` / `or` **短路求值**：右操作数仅在左操作数需要时求值（结果经隐藏
  临时槽位 `__sc_N` 合流，规范化为 0/1）。
- 二元运算按语法推断选族：浮点算术/比较（`is_float_expr`，含变量类型传播：
  `var x = 1.5` 后 `x * 2.0` 发 F64 族）发 F64，其余按 I32 族；
  int 与 float 混用在发射期报类型诊断。
- 字符串 `==` / `!=` 为 `strcmp` **内容比较**（非地址比较）。
- `^` 为整数快速幂（内置 `vx_pow_i32`），负指数结果为 0。

## 7. 编译流水线与产物

```
.vx → Lexer → Parser → 导入表(@imp/@cppimp) → Codegen(TypeIR) → VXOBJ v4
VXOBJ → vlnk（读回 → LLVM IR → clang 链接）→ 原生可执行文件
```

```bash
vxc hello.vx                # → hello.vxobj（--check 仅检查，--dump 打印模块摘要）
vlnk hello.vxobj -o hello   # → 可执行文件
```

## 8. 已知限制

- 顶层 `var`/`const` 不生成运行时代码。
- 浮点幂（`2.0 ^ 3.0`）暂不支持，仅整数 `^`。
- TypeIR 的 `OP_SYS_ARGV` / `OP_FILE_*` / `OP_SYSTEM` 操作码尚未发射；
  对应的系统能力通过内置运行时函数提供（见 §3.3 的 `sys_argv_*` / `file_*` / `system`）。
- 后端已覆盖 ARRAY/MAP/INDEX/FIELD 基础发射（P4 MVP：8 字节统一槽，字符串/整数键映射）；浮点数组精度与复杂布局仍受限。
- `std/` 标准库为设计参考（v1.0 按已废弃的 VM 路线编写，未接入编译器构建），
  语法缺口清单见 `std/README.md`。
