# VX 标准库（设计参考）

> **当前定位：前瞻设计稿，不可被当前工具链编译执行。**
>
> 本目录 v1.0 版本为早期 VM 执行路线设计（源码 → VM 字节码 → 虚拟机解释）。
> 工具链已转向原生路线（`vxc` → VXOBJ → `vlnk` → clang 链接为原生可执行），
> 原「VM 内建指令」前提整体作废，本文末尾的 VM 指令清单不再代表演进方向。
> 本目录保留作为标准库 API 设计参考，待编译器能力补齐后逐步原生化。

## 当前真实可用的能力

标准库的职能当前由两套机制承担：

| 机制 | 内容 | 位置 |
|------|------|------|
| 内置运行时（无需声明） | `out` / `out_int` / `out_bool` / `out_float` / `sys_argv_count` / `sys_argv_get` / `file_read` / `file_write` / `file_exists` / `puts` / `exit` / `malloc` / `free` | `src/runtime/vxrt.c` |
| 导入库（`@imp` / `@cppimp`） | math / text / fmath / collections/vec / cpp/greet / sys（`system(cmd)`）（VX ABI：`_vx_<lib>_<name>`） | `headers/` |

文件 I/O 与命令行参数等本目录中标记 `not implemented` 的能力，
在上述内置运行时中已有原生实现，可直接使用（见 README §2.1 / docs/SYNTAX.md §3.3）。

## 与编译器现状的差距（v1.12.0 实测）

47 个 `.vx` 文件中 36 个可被当前编译器解析，11 个失败，对照如下：

| 文件 | 依赖的不支持语法 |
|------|------------------|
| `error.vx`、`sys/coroutine.vx` | `enum` 声明 |
| `bytes.vx` | 泛型参数 `Vec<int>` |
| `reflect/mod.vx` | 类内常量字段（无类型注解赋值字段） |
| `macros.vx` | `macro` 声明 |
| `conv.vx` | 类型转换调用 `int(v)` |
| `char.vx` | 十六进制字面量 `0x4E00` |
| `collections/linked_list.vx` | 函数体内 `import` |
| `mem/arena.vx`、`mem/page.vx` | 空函数体（仅注释，无语句） |
| `tests/test_string_io.vx` | 字符串方法调用 `s.split(...)` |

> 可解析 ≠ 可执行：`import` 语法、全局状态、字符串方法等运行时支撑
> 同样未落地。`std/tests/` 从未接入 CTest，不代表有效回归覆盖。

## 编译器能力补齐后的演进路径

1. 解析缺口（上表）逐项支持后，std/ 源码先达到「全部可解析」；
2. `import` / 全局 `var`/`const` 代码生成落地后，std/ 以 `@imp` 或模块
   编译单元形式接入工具链；
3. VM 前提的功能（协程、同步原语等）按原生语义重新设计或裁剪。

## 目录结构（设计稿）

```
std/
├── mod.vx                 # std 总入口
├── prelude.vx             # 预导入模块
├── error.vx               # 错误处理（零依赖）
├── macros.vx              # 编译时宏工具（零依赖）
├── collections/           # 核心数据结构（vec / hashmap / linked_list / set）
├── string/                # 字符串处理（util / pattern / codec / iter / core）
├── math/                  # 基础数学（math / random）
├── io/                    # 输入输出与文件系统（file / stream / path / dir）
├── mem/                   # 内存管理（allocator / arena / general / page）
├── sys/                   # 系统与并发（time / duration / datetime / coroutine / sync / env）
├── iter.vx / option.vx / conv.vx / bytes.vx / char.vx / reflect/
└── tests/                 # 设计期手写用例（未接入 CTest）
```

## 模块依赖关系（设计）

```
error (零依赖)
macros (零依赖)
  ↓
collections → error
  ↓
string → collections, error
  ↓
math → (弱依赖 collections)
  ↓
io → collections, string, error, sys
sys → collections, error
```

## 语法约束（当前 VX 编译器限制）

v1.0 标准库基于设计期编译器能力编写：

- 使用 `class` 替代 `struct + impl`（VX 无 `impl` 块）
- 使用 `if/elif/else` 替代 `match` 表达式
- 使用函数返回值替代 `const` 常量
- 所有定义默认公开（VX 无 `pub` 关键字）
- 以 `var` 类型字段模拟泛型（`Option.value: var`）
