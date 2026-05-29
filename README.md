# VX-ToolChains

VX 编程语言的完整工具链——编译器、链接器、运行时虚拟机。

## 项目简介

VX-ToolChains 是 VX 语言的 Rust 原生实现，提供从源码到可执行文件的完整编译流水线。项目采用模块化架构，将编译器前端（词法分析、语法解析、所有权检查）与后端（字节码生成、链接、VM 执行）分离为独立组件。

**核心特性**：
-   **完整编译流水线**：`.vx` 源码 → Token 流 → AST → 所有权检查 → VXOBJ 字节码
-   **内存安全模式**：类 Rust 的所有权系统，支持 `newz`（堆分配）、`free`（显式释放）、`move`（所有权转移）、`&`（借用检查）
-   **自包含运行时**：VM 将字节码附加到运行时后可执行文件的末尾，生成独立可执行文件
-   **零警告编译**：`cargo build` 完全干净，无任何 warning

## 环境要求

| 组件 | 最低版本 |
|------|----------|
| Rust 工具链 | 1.70+（2021 Edition） |
| Cargo | 1.70+ |
| 操作系统 | Linux / Windows / macOS |

## 项目结构

```
VX-ToolChains/
├── Cargo.toml              # 项目配置（3 个二进制目标 + 1 个动态库 + 2 个依赖）
├── Cargo.lock              # 依赖锁定文件
├── vxmodel                 # VPM 模块清单（key:value 格式）
├── src/
│   ├── lib.rs              # vx_vm 库入口（17 行，纯声明 + 重导出）
│   ├── opcode.rs           # VM 指令集定义（62 条指令）
│   ├── value.rs            # 运行时值类型系统
│   ├── instruction.rs      # 指令/函数/模块/调用帧数据结构
│   ├── vm.rs               # VM 核心（创建、加载模块、栈操作）
│   ├── vm_exec.rs          # VM 指令分发执行器（run 方法）
│   ├── memory_safety.rs    # 堆内存安全运行时（分配/验证/解引用/释放/析构）
│   ├── bytecode.rs         # VXOBJ 序列化/反序列化（v2 格式）
│   ├── ipt.rs              # 编译器入口（107 行）
│   ├── token.rs            # 词法分析器（Lexer）
│   ├── parser.rs           # AST 定义 + 递归下降解析器
│   ├── compiler_opcode.rs  # 编译器内部指令集
│   ├── compiler_bytecode.rs# 编译器字节码格式
│   ├── compiler_ownership.rs# 编译期所有权检查器
│   ├── compiler_core.rs    # 编译器核心（Compile/CompileStmt/CompileExpr）
│   ├── main.rs             # 运行时入口（自解压 VM）
│   └── vxlinker.rs         # 链接器（stub + bytecode → 可执行文件）
├── docs/                   # 文档
└── logs/                   # 日志
```

### Cargo.toml 配置说明

```toml
[package]
name = "vx_language_compiler"
version = "1.0.0"
edition = "2021"

# 三个二进制目标
[[bin]]
name = "vxcompiler"          # 编译器 → src/ipt.rs
[[bin]]
name = "vxlinker"            # 链接器 → src/vxlinker.rs
[[bin]]
name = "vx_runtime"          # 运行时 → src/main.rs

# 一个库目标（可作为外部 crate 引用）
[lib]
name = "vx_vm"
path = "src/lib.rs"
crate-type = ["lib", "cdylib"]  # Rust 库 + C 动态库

# 运行时依赖
[dependencies]
serde = { version = "1.0", features = ["derive"] }
yaml-rust2 = "0.9"

# 测试依赖
[dev-dependencies]
assert_cmd = "2.0"
predicates = "3.0"
tempfile = "3.0"
trycmd = "0.14"
```

> **模块发现机制**：Rust 通过源码中的 `mod` 声明自动发现子模块文件，**无需**在 Cargo.toml 中逐文件注册。例如 `lib.rs` 中的 `mod opcode;` 会让 Cargo 自动编译 `src/opcode.rs`。新增的 12 个子模块文件（`opcode.rs`、`value.rs`、`instruction.rs`、`vm.rs`、`vm_exec.rs`、`compiler_opcode.rs`、`compiler_bytecode.rs`、`compiler_ownership.rs`、`compiler_core.rs` 等）均通过此机制纳入编译。

## 安装与构建

### 克隆项目

```bash
git clone https://github.com/yourusername/vx-language-compiler.git
cd VX-ToolChains
```

### 开发构建

```bash
# Debug 构建（含调试信息，未优化）
cargo build

# 构建产物位置：
#   target/debug/vxcompiler    — 编译器
#   target/debug/vxlinker      — 链接器
#   target/debug/vx_runtime    — 运行时
#   target/debug/libvx_vm.so   — VM 动态库
```

### 发布构建

```bash
# Release 构建（LTO + 最高优化 + 剥离符号）
cargo build --release

# 构建产物位于 target/release/
```

### 运行测试

```bash
cargo test          # 所有测试
cargo test --lib    # 仅库测试（bytecode 往返测试）
```

## 使用指南

### 1. 编译 VX 源码 → 字节码

```bash
# 基本用法
./target/debug/vxcompiler hello.vx

# 指定输出路径
./target/debug/vxcompiler hello.vx -o hello.vxobj
```

编译器流水线：
1.  读取 `.vx` 源码
2.  词法分析（Lexer）→ Token 流
3.  语法分析（Parser）→ AST
4.  所有权检查（OwnershipChecker）→ 验证内存安全性
5.  字节码生成（Compiler）→ `.vxobj` 文件（VXOBJ v2 格式）

编译器内置 VPM 系统接口：`sys_argv` / `os_system` / `file_read` / `file_write` / `file_exists`

### 2. 链接为可执行文件

```bash
# 将字节码与运行时链接
./target/debug/vxlinker hello.vxobj -o hello

# Windows 上自动生成 .exe 后缀
./target/debug/vxlinker hello.vxobj -o hello.exe
```

链接器将 VXOBJ 字节码附加到运行时 stub 末尾（后跟 8 字节 payload 大小），生成自包含的可执行文件。

### 3. 直接运行

```bash
# 运行链接后的可执行文件
./hello
```

运行时启动流程：
1.  读取自身 EXE 文件
2.  从末尾提取字节码 payload
3.  初始化 VM 并加载模块
4.  执行 `__main__` 函数

## 内存安全模型

VX 编译器内置编译期所有权检查和运行时内存安全保护：

| 关键字 | 说明 | 示例 |
|--------|------|------|
| `newz` | 堆分配，返回指针 | `p = newz Point(10, 20)` |
| `free` | 显式释放堆内存 | `free(p)` |
| `move` | 转移所有权 | `q = move p` |
| `&` | 获取引用（借用检查） | `r = &p.x` |

编译期 `OwnershipChecker` 检测：
-   use-after-move / use-after-free
-   double-free
-   内存泄漏（作用域结束时未释放的堆变量）
-   活跃借用冲突

运行时 VM 通过代数（generation）机制检测悬垂/野指针。

## 开发说明

### 添加新的编译器模块

编译器模块由 `src/ipt.rs` 中的 `mod` 声明管理，只需：

```rust
// src/ipt.rs — 添加一行声明
mod compiler_new_module;

// 然后在 src/compiler_new_module.rs 中编写代码
```

### 添加新的 VM 功能

VM 模块由 `src/lib.rs` 中的 `mod` 声明管理：

```rust
// src/lib.rs
mod vm_new_feature;

// Re-export 公开 API
pub use vm_new_feature::NewType;
```

之后其他 Rust 项目可通过 `vx_vm = { path = "..." }` 引用 VM 库。

### 代码风格

-   零警告：`cargo build` 应在任何分支上零警告通过
-   模块边界清晰：每个文件关注单一职责
-   公开 API 集中在 `lib.rs` 重导出

## 许可证

AGPL-3.0 — 详见 [LICENSE](LICENSE)
