# VX 标准库（设计参考）

> **当前定位：前瞻设计稿，大部分不可被当前工具链编译执行。**
>
> 本目录 v1.0 版本为早期 VM 执行路线设计（源码 → VM 字节码 → 虚拟机解释）。
> 工具链已转向原生路线（`vxc` → VXOBJ → `vlnk` → clang 链接为原生可执行），
> 原「VM 内建指令」前提整体作废，本文末尾的 VM 指令清单不再代表演进方向。
> 本目录保留作为标准库 API 设计参考，待编译器能力补齐后逐步原生化。
>
> **例外（v1.19.0 起）**：`sys/sync.vx` 已按原生路线重写，基于 `@imp(proc)`
> 的 pthread 原语（Mutex / Condvar / Semaphore / Barrier / Channel，无忙等待），
> 可被当前 `vxc` 编译并由 `vlnk` 链接执行；回归覆盖见
> `tests/import_e2e_test.cpp` 的 `synclib` 用例（库源码拼接 main 后
> 编译→链接→运行断言）。跨编译单元的类引用尚受模块系统未落地限制，
> 当前以同单元包含（拼接/复制）方式使用。

## 当前真实可用的能力

标准库的职能当前由两套机制承担：

| 机制 | 内容 | 位置 |
|------|------|------|
| 内置运行时（无需声明） | `out` / `out_int` / `out_bool` / `out_float` / `sys_argv_count` / `sys_argv_get` / `file_read` / `file_write` / `file_exists` / `puts` / `exit` / `malloc` / `free` | `src/runtime/vxrt.c` |
| 导入库（`@imp` / `@cppimp`） | math / text（查找、截取、trim、数值转换、行切分）/ fmath / fs（目录创建与列举、文件信息、路径运算）/ collections/vec / cpp/greet / sys（`system(cmd)`）/ proc（进程原语 `fork`/`waitpid`、线程/任务池 `spawnN`/`join_*`/`pool_*`、管道 `pipe`/`read_str`/`capture`）/ ptr（`add`/`to_int`/`from_int`）（math/text/fmath/fs 等为 `.vxh` + `.c`；proc/ptr 仅 `.vxh`，直连 libc / 后端生成 IR）（VX ABI：`_vx_<lib>_<name>`） | `headers/` |

文件 I/O 与命令行参数等本目录中标记 `not implemented` 的能力，
在上述内置运行时中已有原生实现，可直接使用（见 README §2.1 / docs/SYNTAX.md §3.3）。

## 与编译器现状的差距（v1.21.1 实测）

47 个 `.vx` 文件中 **23 个可被当前编译器 `--check` 通过，24 个失败**。
`import` 已按「模块即头文件」装载声明（类/结构、枚举、函数签名），
此前的跨模块 `error` / `StringUtil` / `Result` 未定义（E0010）大部分已消除。
接收类实例的返回类型、`var` 形参/字段、`pass`、十六进制字面量、`_` 丢弃槽
与 `int(x)` / `float(x)` / `str(x)` 显式转换已支持。
剩余失败按根因分三类：

### 1. 解析缺口（语法尚不支持）

| 文件 | 依赖的不支持语法 |
|------|------------------|
| `bytes.vx` | 泛型参数 `Vec<int>` |
| `macros.vx` | `macro` 声明 |
| `tests/test_string_io.vx` | 字符串方法调用 `s.split(...)` |

### 2. 语义层严格类型检查（E0007，无隐式转换 / 集合运算缺失）

自 v1.16.5 起语义分析按类型完全匹配校验，std/ 设计稿中大量
`float`/`int` 混算、`string` + `int` 拼接、类实例按 `int` 返回等
写法不再通过：

| 文件 | 首个报错（示例） |
|------|------------------|
| `math/math.vx` | `-` 两侧 `float`/`int` 混用 |
| `math/random.vx` | 返回值期望 `float` 实际 `int` |
| `mem/arena.vx`、`io/file.vx`、`string/codec.vx` | `+` 不支持 `list` 与 `list`（无列表拼接） |
| `error.vx`、`mem/general.vx`、`string/core.vx`、`string/util.vx`、`sys/datetime.vx` | `string` 与 `int` 拼接 |
| `sys/duration.vx` | 赋值期望 `int` 实际 `float` |
| `tests/test_collections.vx` | `*` 不支持 `string` 与 `int` |
| `collections/hashmap.vx`、`collections/linked_list.vx`、`collections/set.vx` | 方法标注 `-> int` 却返回类实例（源码 workaround；现可改用 `-> 类名`） |

### 3. 源码自身的声明/语句缺口（E0010）

| 文件 | 原因 |
|------|------|
| `iter.vx`、`string/pattern.vx`、`tests/run_all.vx` | 引用 `Vec`，但 `collections/vec.vx` 未声明该类（设计稿未完成） |
| `sys/time.vx` | 使用 `Duration` 但未 `import std.sys.duration` |

> 可解析 ≠ 可执行：全局状态、字符串方法等运行时支撑同样未落地。
> `std/tests/` 从未接入 CTest，不代表有效回归覆盖。
> 唯一可编译执行的例外是 `sys/sync.vx`（v1.19.0 原生重写，见上文）。

## 编译器能力补齐后的演进路径

1. 解析缺口（enum / 泛型 / 宏 / 类型转换 / 十六进制字面量）逐项支持后，
   std/ 源码先消除第 1 类失败；
2. 语义层放宽或 std/ 源码适配「无隐式转换」规则（显式标注 /
   显式转换），消除第 2 类失败；
3. 模块系统（跨编译单元类型解析）落地后，第 3 类失败随之消除，
   std/ 以 `@imp` 或模块编译单元形式接入工具链；
4. VM 前提的功能（协程、环境变量、高精度时间等）按原生语义
   重新设计或裁剪。

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
