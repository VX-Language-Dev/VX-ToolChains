# VX 标准库 v1.0

VX 语言第一版标准库，提供基础数据结构、字符串处理、数学运算、文件 I/O、系统与并发、错误处理等核心功能。

## 目录结构

```
std/
├── mod.vx                 # std 总入口
├── prelude.vx             # 预导入模块
├── error.vx               # 错误处理（零依赖）
├── macros.vx              # 编译时宏工具（零依赖）
├── collections/           # 核心数据结构
│   ├── mod.vx
│   ├── vec.vx             # 动态数组 Vec
│   ├── hashmap.vx          # 哈希表 HashMap
│   ├── linked_list.vx      # 双向链表 LinkedList
│   └── set.vx              # 集合 Set
├── string/                # 字符串处理
│   ├── mod.vx
│   ├── util.vx             # 字符串工具方法
│   ├── pattern.vx          # 通配符模式匹配
│   └── codec.vx            # 编码转换（Hex/Base64）
├── math/                  # 基础数学
│   ├── mod.vx
│   ├── math.vx             # 三角函数/幂/对数/常量
│   └── random.vx           # 伪随机数生成器
├── io/                    # 输入输出与文件系统
│   ├── mod.vx
│   ├── file.vx             # 文件读写操作
│   ├── stream.vx           # stdin/stdout/stderr
│   ├── path.vx             # 路径处理工具
│   └── dir.vx              # 目录遍历操作
└── sys/                   # 系统与并发
    ├── mod.vx
    ├── time.vx             # 单调时钟 Instant
    ├── duration.vx         # 时间间隔 Duration
    ├── datetime.vx         # 日期时间 DateTime
    ├── coroutine.vx        # 协程支持
    ├── sync.vx             # 同步原语（Mutex/Semaphore/Channel）
    └── env.vx              # 环境变量与进程控制
```

## 宏工具

`std::macros` 提供基于 VX 编译时宏系统的常用代码模板，由 `prelude` 自动注入，零运行时开销。

```vx
func main():
    var x = 42
    #debug_print("x", x)             # DEBUG: x = 42

    #assert(x > 0, "x 必须为正数")   # 条件失败时输出错误信息

    #repeat(3, out("Hello"))        # 输出 3 次 Hello

    #when(x > 0, out("x 为正数"))   # 条件为真时执行

    #log_info("服务启动完成")       # 输出 [INFO] 服务启动完成
```

可用宏：

| 宏 | 参数 | 说明 |
|----|------|------|
| `#assert` | `(condition, message)` | 断言，失败时输出错误信息 |
| `#debug_print` | `(name, value)` | 调试输出变量名和值 |
| `#log_info` | `(message)` | 信息日志 |
| `#log_warn` | `(message)` | 警告日志 |
| `#log_error` | `(message)` | 错误日志 |
| `#log_debug` | `(message)` | 调试日志 |
| `#when` | `(condition, action)` | 条件为真时执行 |
| `#unless` | `(condition, action)` | 条件为假时执行 |
| `#repeat` / `#times` | `(n, body)` | 重复执行 n 次 |
| `#swap` | `(a, b)` | 交换两个变量值 |
| `#ensure_ok` | `(result, message)` | 确保 Result 成功 |
| `#try_return` | `(result)` | Result 失败时直接返回 |

## 快速开始

```vx
# 导入全部标准库
import std

# 或按需导入
import std.error
import std.collections.vec
import std.math

# 使用动态数组
func main():
    var v = new Vec()
    v.push(1)
    v.push(2)
    v.push(3)
    out("len=" + v.size())

    var r = v.get(0)
    if r.is_ok:
        out("first=" + r.value)

# 使用错误处理
func read_config(path: var) -> var:
    if file_exists(path) == false:
        return Result.err(error.io_error("config not found: " + path))
    var content = file_read(path)
    return Result.ok(content)
```

## 模块依赖关系

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

## 设计原则

- **轻量性**：最小内核约 600 行 VX 代码，按需导入实现树摇
- **可扩展性**：模块化设计，命名空间隔离，新模块可独立添加
- **跨平台**：Path 模块统一处理路径分隔符，平台相关代码集中隔离
- **安全性**：利用 VX 所有权系统，Mutex 等同步原语避免数据竞争

## 语法约束（当前 VX 编译器限制）

v1.0 标准库基于当前 VX 编译器能力编写：
- 使用 `class` 替代 `struct + impl`（VX 无 `impl` 块）
- 使用 `var` 类型替代泛型（VX 泛型尚未完整实现）
- 使用 `if/elif/else` 替代 `match` 表达式
- 使用函数返回值替代 `const` 常量
- 所有定义默认公开（VX 无 `pub` 关键字）

这些约束在编译器能力增强后可以逐步移除，标准库将随之升级。

## TODO：VM 层待支持的内建指令

以下标准库功能依赖 VM 层的新指令支持：

| 优先级 | 功能 | 所需指令 |
|--------|------|----------|
| P1 | 系统时钟 | `sys_time` / `monotonic_clock` |
| P1 | 目录操作 | `mkdir` / `read_dir` / `rm_dir` |
| P1 | 环境变量 | `get_env` / `set_env` |
| P2 | 协程支持 | `coro_spawn` / `coro_yield` / `coro_resume` |
| P2 | 进程控制 | `exit` / `exec` |
| P3 | 标准输入 | `stdin_read` |
| P3 | 文件权限 | `file_stat` / `chmod` |
| P3 | 网络 | `tcp_connect` / `tcp_listen` |
