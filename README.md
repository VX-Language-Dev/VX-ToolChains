# VX-ToolChains

VX 编程语言的完整工具链——编译器、链接器、运行时虚拟机、包管理器、语言服务器、调试器。

## 项目简介

**VX-ToolChains** 是 VX 语言的 Rust 原生实现，提供从源码到可执行文件的完整编译流水线。采用模块化架构，支持多种运行模式：

- **解释模式**（默认）：编译为字节码，由内置 VM 解释执行
- **AOT 模式**（可选）：通过 Cranelift 将 TypeIR 编译为原生机器码（ELF/Mach-O/PE）

**核心特性**：

- **27 关键字精简语法**：仅保留 VM 底层硬编码 22 核心 + 5 原生类型，string/vector 入库，and/or/not→符号运算符，详细变更见 [CHANGELOG](CHANGELOG.md)
- **完整编译流水线**：`.vx` 源码 → Token 流 → AST → 所有权检查 → 类型化 IR → VXOBJ v3 字节码
- **项目构建系统**：`vxsetting.toml` 配置驱动，支持多文件项目（bin/vxlib/lib/module）和单文件项目的统一构建，增量缓存加速
- **内存安全模型**：类 Rust 的所有权系统，支持 `new`（构造）、`move`（所有权转移）、`&`（借用检查），编译期检查 + 运行时代数机制双重保障
- **VXOBJ v3 容器格式**：分段存储（TypeIR + Bytecode + DebugInfo + SourceMap + TypeMeta），支持 LZ4 压缩
- **AOT 原生编译**：可选 Cranelift 后端，支持 x86_64 / aarch64 交叉编译
- **LSP 语言服务器**：提供代码补全、悬停信息、符号导航、诊断、跳转定义等 IDE 功能
- **CLI 调试器**：支持断点设置、单步执行、栈查看的交互式调试
- **VPM 包管理器**：管理 .vack 格式（7z 压缩包）的第三方包，支持 Python/TS/JS/Java/Rust/Go/C/C++ 等多语言实现
- **自包含运行时**：VM 将字节码附加到运行时后可执行文件的末尾，生成独立可执行文件

## 环境要求

| 组件        | 最低版本                |
| ----------- | ----------------------- |
| Rust 工具链 | 1.70+（2021 Edition）   |
| Cargo       | 1.70+                   |
| 7z / 7zz    | 可选（VPM 需要）        |
| 操作系统    | Linux / Windows / macOS |

## 项目结构

```
VX-ToolChains/
├── Cargo.toml               # 项目配置（6 个二进制目标 + 1 个库）
├── Cargo.lock               # 依赖锁定
├── CHANGELOG.md             # 版本历史
├── LICENSE
├── README.md
├── docs/
│   └── vxlinker_rust.md     # 链接器调用 Rust 原生库说明
├── dist/                    # 预编译产物
│   ├── README.txt
│   ├── vx_runtime
│   ├── vxcompiler
│   ├── vxdbg
│   └── vxlinker
├── package/                 # VPM 包目录
│   └── browser_detect/
│       └── info.toml
├── tests/
│   ├── integration_test.rs  # VM 集成测试
│   └── fixtures/            # 测试用例
│       ├── basic_arithmetic.vx
│       ├── boolean_logic.vx
│       ├── control_flow.vx
│       ├── fibonacci.vx
│       └── string_ops.vx
├── qodana.yaml              # 静态分析配置
├── vxset.toml语法模板.toml   # 项目配置模板
└── src/
    ├── lib.rs               # vx_vm 库入口（模块声明 + API 重导出）
    ├── opcode.rs            # VM 指令集定义
    ├── value.rs             # 运行时值类型系统
    ├── instruction.rs       # 指令/函数/模块/调用帧数据结构
    ├── vm.rs                # VM 核心（创建、加载、断点、步进）
    ├── vm_dispatch.rs       # VM 按操作类别分组的指令分发器
    ├── vm_exec.rs           # VM 指令分发执行器入口
    ├── memory_safety.rs     # 堆内存安全运行时（代数机制）
    ├── bytecode.rs          # VXOBJ v2/v3 序列化/反序列化（LZ4 压缩）
    ├── type_ir.rs           # 类型化中间表示（供 AOT 编译器消费）
    ├── aot_backend.rs       # Cranelift AOT 后端（feature: aot）
    ├── token.rs             # 词法分析器（Lexer）
    ├── parser/
    │   ├── mod.rs           # 解析器入口
    │   ├── ast.rs           # AST 节点定义
    │   ├── expr.rs          # 表达式解析
    │   └── stmt.rs          # 语句解析
    ├── compiler_core.rs     # 编译器核心（源码 → 字节码/TypeIR）
    ├── compiler_bytecode.rs # 编译器字节码格式定义
    ├── compiler_ownership.rs# 编译期所有权检查器
    ├── vxsetting.rs         # vxsetting.toml 项目配置解析
    ├── builder.rs           # 项目构建器（多文件项目 + 单文件回退）
    ├── cache.rs             # 增量构建缓存（mtime/fingerprint/哈希）
    ├── ipt.rs               # 编译器 CLI 入口（vxcompiler）
    ├── vxlinker.rs          # 链接器 CLI 入口（链接 stub + 字节码）
    ├── main.rs              # 运行时 CLI 入口（自解压 VM）
    ├── pm.rs                # VPM 包管理器 CLI 入口
    ├── debugger/
    │   └── main.rs          # CLI 调试器入口（vxdbg）
    └── lsp/
        ├── main.rs          # LSP 服务器入口
        ├── backend.rs       # LSP 后端逻辑
        ├── state.rs         # 文档状态管理
        ├── completion.rs    # 代码补全
        ├── diagnostics.rs   # 诊断/错误提示
        ├── hover.rs         # 悬停信息
        ├── symbols.rs       # 符号导航
        └── goto.rs          # 跳转定义
```

## 安装与构建

### 克隆项目

```bash
git clone https://gitee.com/vx-language-dev/vx-tool-chains.git
cd VX-ToolChains
```

### 开发构建

```bash
# Debug 构建（默认不含 AOT 后端）
cargo build

# 包含 AOT 后端（启用 Cranelift）
cargo build --features aot

# 构建产物：
#   target/debug/vxcompiler   — 编译器
#   target/debug/vxlinker     — 链接器
#   target/debug/vx_runtime   — 运行时
#   target/debug/vpm          — 包管理器
#   target/debug/vx-lsp       — 语言服务器
#   target/debug/vxdbg        — 调试器
```

### 发布构建

```bash
# Release 构建（LTO + O3 + 剥离符号）
cargo build --release

# 带 AOT 的 Release
cargo build --release --features aot
```

### 运行测试

```bash
cargo test                    # 所有测试（库 + 集成）
cargo test --lib              # 仅库测试
cargo test --features aot     # 包含 AOT 测试
```

## 使用指南

### 0. 项目配置（vxsetting.toml）

多文件项目在根目录创建 `vxsetting.toml` 声明构建目标：

```toml
# [bin]  可执行文件
[bin]
source = ["main.vx", "util.vx"]
version = "1.0.0"
output = "dist/myapp"

# [[module]]  可复用模块（支持多个）
[[module]]
info = "pkg/mymod/info.toml"
name = "mymod"
source = ["mymod/lib.vx"]

# [vxset]  全局构建配置
[vxset]
optimization = 20    # 优化等级 0-20
cache = true          # 启用增量缓存
deadcode = true       # 允许死代码

# [libraries]  外部库路径映射
[libraries]
stdlib = "/usr/local/lib/vx/stdlib"
```

存在 `[bin]` / `[vxlib]` / `[lib]` / `[[module]]` 任一目标时,构建器自动启用多文件模式；否则回退为单文件编译。详见 [vxsetting.rs](src/vxsetting.rs)。

### 1. 编译 VX 源码 → 字节码

```bash
# 单文件编译
./target/debug/vxcompiler hello.vx

# 指定输出路径
./target/debug/vxcompiler hello.vx -o hello.vxobj
```

编译器流水线：

1. **词法分析**（Lexer）→ Token 流
2. **语法分析**（Parser）→ AST
3. **所有权检查**（OwnershipChecker）→ 验证内存安全性
4. **字节码生成 / TypeIR 生成** → `.vxobj` 文件（VXOBJ v3 格式）

### 2. 链接为可执行文件

```bash
# 解释模式（默认）：字节码附加到运行时 stub
./target/debug/vxlinker hello.vxobj -o hello

# AOT 模式：编译为原生机器码
./target/debug/vxlinker hello.vxobj -o hello --mode aot
```

链接器支持三种模式：

| 模式          | 说明                      | 命令                                    |
| ------------- | ------------------------- | --------------------------------------- |
| `interpret` | VM 解释执行字节码（默认） | `--mode interpret`                    |
| `jit`       | JIT 编译（保留，待实现）  | `--mode jit`                          |
| `aot`       | Cranelift 原生编译        | `--mode aot`（需 `--features aot`） |

### 3. 直接运行

```bash
# 运行链接后的可执行文件
./hello

# 或直接用 VM 运行字节码
./target/debug/vx_runtime --load hello.vxobj
```

### 4. LSP 语言服务器

```bash
# 启动 LSP 服务器（供 VS Code / Neovim / Emacs 等集成）
./target/debug/vx-lsp
```

支持的功能：

| 功能     | 说明                   |
| -------- | ---------------------- |
| 代码补全 | 关键字、函数名、变量名 |
| 悬停信息 | 类型签名、文档注释     |
| 符号导航 | 工作区符号、文档符号   |
| 诊断     | 编译错误实时提示       |
| 跳转定义 | 变量/函数定义跳转      |

### 5. 调试器

```bash
# 启动调试器加载字节码
./target/debug/vxdbg hello.vxobj
```

交互式命令：

| 命令                   | 简写          | 说明     |
| ---------------------- | ------------- | -------- |
| `break <pc>`         | `b`         | 设置断点 |
| `clear <pc>`         |               | 清除断点 |
| `list`               | `l`         | 列出函数 |
| `run` / `continue` | `r` / `c` | 开始执行 |
| `help`               | `h`         | 显示帮助 |
| `quit`               | `q`         | 退出     |

### 6. VPM 包管理器

```bash
# 安装包
./target/debug/vpm install <包名>

# 列出已安装包
./target/debug/vpm list
```

VPM 支持的语言：Python、TypeScript、JavaScript、Java、Rust、Go、C、C++。

## VXOBJ v3 格式

VXOBJ v3 是基于 Section 的容器格式，支持元数据分段存储和按需解压：

```
[Header]
  5 bytes magic: "VXOBJ"
  4 bytes version (u32 BE): 3
  N bytes TargetTriple

[Section Index Table]
  4 bytes section count
  For each section: name, offset, compressed_size, raw_size, flags

[Sections] (可选，按需压缩)
  TypeIR      — 类型化中间表示
  Bytecode    — VM 字节码
  Debug       — 调试信息
  SourceMap   — 源码映射
  TypeMeta    — 元类型信息
```

- 向后兼容 v2 格式
- 支持 LZ4 压缩（>64 bytes 自动启用）
- 目标三元组记录（支持交叉编译）

## 内存安全模型

VX 编译器内置编译期所有权检查和运行时内存安全保护：

| 关键字   | 说明                 | 示例                      |
| -------- | -------------------- | ------------------------- |
| `new`  | 构造实例（栈分配）   | `p = new Point(10, 20)` |
| `move` | 转移所有权           | `q = move p`            |
| `&`    | 获取引用（借用检查） | `r = &p.x`              |

> 从 v1.1.1 起，`newz`（堆分配）和 `free`（显式释放）已迁移至标准库 `mem::zeroed<T>()` / `mem::free(ptr)`。详见 [CHANGELOG](CHANGELOG.md)。

**编译期** `OwnershipChecker` 检测：

- use-after-move / use-after-free
- double-free
- 内存泄漏（作用域结束时未释放的堆变量）
- 活跃借用冲突

**运行时** VM 通过代数（generation）机制检测悬垂指针/野指针。

## AOT 编译（Cranelift 后端）

启用 `--features aot` 后，可通过 Cranelift 将 TypeIR 编译为原生机器码：

- **宿主原生**：自动检测 CPU 特性（x86_64 / aarch64）
- **交叉编译**：指定 target triple（如 `aarch64-unknown-linux-gnu`）
- **输出格式**：ELF / Mach-O / PE 对象文件
- **链接**：产物可与 C/Rust 目标文件链接为最终可执行文件

```bash
# 交叉编译到 ARM64
cargo build --release --features aot
./target/release/vxlinker hello.vxobj -o hello_arm64 --mode aot --target aarch64-unknown-linux-gnu
```

## 开发说明

### 添加新的编译器模块

```rust
// src/ipt.rs — 添加一行声明
mod compiler_new_module;

// 然后在 src/compiler_new_module.rs 中编写代码
```

### 添加新的 VM 功能

```rust
// src/lib.rs
mod vm_new_feature;

// Re-export 公开 API
pub use vm_new_feature::NewType;
```

之后其他 Rust 项目可通过 `vx_vm = { path = "..." }` 引用 VM 库。

### 代码风格

- 零警告：`cargo build` 应在任何分支上零警告通过
- 模块边界清晰：每个文件关注单一职责
- 公开 API 集中在 `lib.rs` 重导出

## 依赖概览

| 依赖                    | 用途                    |
| ----------------------- | ----------------------- |
| `tower-lsp`           | LSP 协议实现            |
| `lsp-types`           | LSP 类型定义            |
| `tokio`               | LSP 异步运行时          |
| `dashmap`             | 并发文档状态管理        |
| `lz4_flex`            | VXOBJ section 压缩      |
| `target-lexicon`      | 目标平台三元组解析      |
| `serde`               | 缓存文件序列化          |
| `toml`                | vxsetting.toml 配置解析 |
| `cranelift-*`（可选） | AOT 原生代码生成        |

## 许可证

AGPL-3.0 — 详见 [LICENSE](LICENSE)
