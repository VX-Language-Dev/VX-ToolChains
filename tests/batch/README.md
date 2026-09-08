# VX 批量测试套件（tests/batch）

用 VX 语言编写、用 Shell 脚本统一运行的批量测试。覆盖**简单实例**、**复杂算法**、**特殊情况**三大类，共 27 个自校验用例。每个用例编译 → 链接 → 运行，并打印 `PASS`/`FAIL`；Shell 运行器汇总结果并以退出码反映整体成败。

## 目录结构

```
tests/batch/
├── run_tests.sh        # 批量运行器（编译 + 链接 + 运行 + 汇总）
├── vxrt.c              # C 运行时：out_raw / out_int / out_bool（无换行）
├── vxrt.o              # 由 vxrt.c 编译（被 .gitignore 忽略，运行器会按需重建）
├── vx_assert.vx        # 共享自校验库（assert_int / assert_bool / assert_true / assert_false / assert_array_sorted）
├── simple/             # 简单实例（7 个）
├── algorithms/         # 复杂算法（12 个）
└── edge_cases/         # 特殊情况（8 个）
```

## 运行

```bash
cd tests/batch
./run_tests.sh                 # 运行全部 27 个用例
./run_tests.sh simple          # 仅运行某一类（simple / algorithms / edge_cases）
./run_tests.sh edge_cases/05_deep_recursion.vx   # 运行单个用例
VX_BIN=/path/to/bin ./run_tests.sh                # 指定 vxc/vlnk 所在目录
```

运行器会自动用 `cc` 把 `vxrt.c` 编译为 `vxrt.o`，把 `vx_assert.vx` 前置拼接到每个用例后统一编译，再用 `vlnk` 链接 `vxrt.o` 生成可执行文件并运行。

## 用例清单

### 简单实例（simple/）
| 文件 | 内容 |
| --- | --- |
| `01_hello.vx` | 基础输出、`out` 与 `out_raw` 协同、字符串+整数拼接 |
| `02_arithmetic.vx` | `+ - * / %` 与运算符优先级 |
| `03_control_flow.vx` | `if/elif/else`、嵌套分支、最大值函数 |
| `04_loops.vx` | `while`、`break`、`continue`、嵌套循环 |
| `05_functions.vx` | 多函数、参数、返回值、相互调用 |
| `06_bool_logic.vx` | `and/or/not`、比较链 |
| `07_string_concat.vx` | `string + string`、`string + int` 拼接 |

### 复杂算法（algorithms/）
斐波那契（迭代+递归）、阶乘、欧几里得 GCD、埃氏筛、二分查找、冒泡/快速/归并排序、快速幂、汉诺塔、回文数、矩阵乘法。

### 特殊情况（edge_cases/）
零与负数、空循环/空区间、整数边界（i64 安全范围内的大数）、除法与取模、深度递归（栈深度压力）、嵌套循环、字符串边界、变量交换。

## 设计要点与已知编译器限制

编写本套件过程中确认了 VX 工具链的若干行为，测试已据此规避：

1. **注释用 `#` 而非 `//`** —— 解析器不支持 `//` 注释。
2. **不支持负整数字面量**（如 `-7`）；一律用 `0 - n` 表达负数。直接写 `-7` 会崩溃。
3. **函数参数不可变** —— 需先在函数体内复制到局部变量再修改（如 GCD 复制 `a,b`）。
4. **`x = x + f()` 这类“复合赋值内调用函数”的结果不可靠** —— 改用线程式累加：`x = f(...)` 后另起一行 `x = x + r`。
5. **带内存分配的函数，其结果不可直接作为另一函数的实参**（`assert_int("...", exp, malloc_fn(...), fails)` 会崩溃）。需先存入局部变量再传参。
6. **变量与负数的函数返回值比较有缺陷**（`var == 函数返回的负数` 会判错）。测试改用正数编码或布尔性质校验，避免负数比较。
7. **字符串不可直接比较/拼接为可断言的值** —— 用例中对拼接正确性改为交叉校验参与拼接的整数分量。
8. **`out()` 是内建（包装 `puts`，自动追加换行）**；要无换行输出需用 C 运行时 `out_raw`。`out_int`/`out_bool` 也由 `vxrt.o` 提供。
9. **多语句必须分多行** —— 解析器不支持 `;` 作为语句分隔符。

> 注：为支持 `out("x=" + 变量)` 这类“变量参与字符串拼接”，已修复 `go/internal/translator/platform.go` 中 `@putchar` 声明缺失的问题（变量拼接会触发 `itoa` 内建，进而引入需要 `@putchar` 的 `@len` 实现）。
