# License Statement
This project is fully independently developed. The entire compiler codebase is written from scratch without forking, modifying or repackaging any third-party compiler source code.
The project uses AGPL-3.0 license and is only used for compiler principle research and open source co-construction, and will not be deployed as online SaaS services.

本项目为完全独立自研项目，编译器全部代码从零编写，没有复刻、修改、打包任何第三方编译器源码。
项目采用 AGPL-3.0 开源协议，仅用于编译原理学术研究与开源协作，不会封装为线上SaaS服务对外运营。
# VX-ToolChains

VX 编程语言的完整工具链——编译器、链接器、包管理器、语言服务器。

## 项目简介

**VX-ToolChains** 是 VX 语言的 Rust 原生实现，提供从源码到原生可执行文件的完整编译流水线。采用模块化架构：

- **原生编译路径**（唯一模式）：源码 → VXOBJ v4 跨平台中间文件 → 链接器自动适配平台添加可执行文件特征 → 原生机器码执行
- **AOT 编译**（可选）：通过 Cranelift 将 TypeIR 编译为原生机器码（ELF/Mach-O/PE）

**核心特性**：

- **纯静态类型系统**：所有变量、参数、字段必须显式声明类型，移除 `var` 类型推断
- **精简语法**：保留核心关键字，string/vector 等移入标准库，and/or/not 与符号运算符并存
- **完整编译流水线**：`.vx` 源码 → Token 流 → AST → 所有权/借用检查 → 类型化 IR → VXOBJ v4 跨平台中间文件
- **原生编译**：链接器自动检测外部依赖切换静态/动态链接，生成独立可执行文件
- **VXOBJ v4 中间格式**：跨平台二进制文件，不包含平台特定可执行特征，链接器自动适配目标平台
- **Rust 内存安全模型**：所有权系统 + 完整借用检查（aliasing XOR mutation）+ 可变/不可变引用 + Copy 语义
- **项目构建系统**：`vxsetting.toml` 配置驱动，支持多文件项目和单文件项目，增量缓存加速
- **AOT 原生编译**：可选 Cranelift 后端，支持 x86_64 / aarch64 交叉编译
- **LSP 语言服务器**：代码补全、悬停信息、符号导航、诊断、跳转定义等 IDE 功能
- **VPM 包管理器**：管理第三方包，支持多语言实现
- **编译时宏系统**：参数化宏定义和展开，零运行时开销

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
├── Cargo.toml               # 项目配置（多个二进制目标 + 库）
├── Cargo.lock               # 依赖锁定
├── README.md
├── tests/
│   ├── integration_test.rs  # 集成测试（VXOBJ v4 + 内存模型）
│   └── fixtures/            # 测试用例
├── src/
│   ├── lib.rs               # vx_vm 库入口（模块声明 + API 重导出）
│   ├── opcode.rs            # 指令集定义
│   ├── bytecode.rs          # VXOBJ v4 序列化/反序列化
│   ├── type_ir.rs           # 类型化中间表示
│   ├── aot_backend.rs       # Cranelift AOT 后端（feature: aot）
│   ├── token.rs             # 词法分析器（Lexer）
│   ├── parser/
│   │   ├── mod.rs           # 解析器入口
│   │   ├── ast.rs           # AST 节点定义
│   │   ├── expr.rs          # 表达式解析
│   │   └── stmt.rs          # 语句解析
│   ├── compiler_core.rs     # 编译器核心
│   ├── compiler_bytecode.rs # 编译器字节码格式定义
│   ├── compiler_ownership.rs# 编译期所有权/借用检查器
│   ├── compiler_expr.rs     # 表达式编译
│   ├── compiler_stmt.rs     # 语句编译
│   ├── compiler_module.rs   # 模块编译
│   ├── compiler_typeir.rs   # TypeIR 编译
│   ├── vxsetting.rs         # 项目配置解析
│   ├── builder.rs           # 项目构建器
│   ├── cache.rs             # 增量构建缓存
│   ├── macros.rs            # 编译时宏系统
│   ├── ipt.rs               # 编译器 CLI 入口（vxcompiler）
│   ├── vxlinker.rs          # 链接器 CLI 入口
│   ├── pm.rs                # VPM 包管理器 CLI 入口
│   └── lsp/
│       ├── main.rs          # LSP 服务器入口
│       ├── backend.rs       # LSP 后端逻辑
│       ├── state.rs         # 文档状态管理
│       ├── completion.rs    # 代码补全
│       ├── diagnostics.rs   # 诊断/错误提示
│       ├── hover.rs         # 悬停信息
│       ├── symbols.rs       # 符号导航
│       └── goto.rs          # 跳转定义
```
（注：runtime VM、调试器、指令运行时、值系统等模块已在 v4 中移除，仅保留原生编译路径。）

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
#   target/debug/vpm          — 包管理器
#   target/debug/vx-lsp       — 语言服务器
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
cargo test                    # 所有测试
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

存在 `[bin]` / `[vxlib]` / `[lib]` / `[[module]]` 任一目标时，构建器自动启用多文件模式；否则回退为单文件编译。详见 [vxsetting.rs](src/vxsetting.rs)。

### 1. 编译 VX 源码 → VXOBJ v4

```bash
# 单文件编译
./target/debug/vxcompiler hello.vx

# 指定输出路径
./target/debug/vxcompiler hello.vx -o hello.vxobj
```

编译器流水线：

1. **词法分析**（Lexer）→ Token 流
2. **语法分析**（Parser）→ AST
3. **所有权/借用检查**（OwnershipChecker）→ 验证内存安全性
4. **TypeIR 生成** → `.vxobj` 文件（VXOBJ v4 格式）

### 2. 链接为可执行文件

```bash
# 静态链接（默认）
./target/debug/vxlinker hello.vxobj -o hello

# AOT 原生编译
./target/debug/vxlinker hello.vxobj -o hello --mode aot

# 检测并链接外部依赖（自动选择静态/动态链接）
./target/debug/vxlinker hello.vxobj -o hello --mode native
```

链接器仅支持 **native** 模式，将 VXOBJ v4 中的 TypeIR 编译为原生机器码，自动适配目标平台并添加可执行文件特征。

### 3. 直接运行

```bash
# 运行链接后的可执行文件
./hello
```

### 4. LSP 语言服务器

```bash
./target/debug/vx-lsp
```

支持的功能：

| 功能     | 说明                                              |
| -------- | ------------------------------------------------- |
| 代码补全 | 关键字、函数名、变量名；函数签名 snippet；成员访问 `.` / `->`；`import` 路径 |
| 悬停信息 | 类型签名、字段/方法文档、内置函数说明、精确 token 高亮 |
| 符号导航 | 工作区符号、文档符号                              |
| 诊断     | 编译错误实时提示                                  |
| 跳转定义 | 变量/函数/参数/导入别名定义跳转，支持作用域内最近匹配 |

### 5. VPM 包管理器

```bash
./target/debug/vpm install <包名>
./target/debug/vpm list
```

VPM 支持的语言：Python、TypeScript、JavaScript、Java、Rust、Go、C、C++。

## VXOBJ v4 格式

VXOBJ v4 是基于 Section 的跨平台容器格式，不包含平台特定可执行特征，链接器负责自动适配目标平台：

```
[Header]
  5 bytes magic: "VXOBJ"
  4 bytes version (u32 BE): 4
  N bytes TargetTriple
  1 byte external_deps_flag
  4 bytes section_count (u32 BE)

[Section Index Table]
  For each section: name, offset, size
  Sections: TypeIR | ExternalDeps
```

- 向后兼容 v3 格式
- 仅存储 TypeIR 和外部依赖信息，无字节码/调试段
- 目标三元组记录（支持交叉编译）
- 无 LZ4 压缩（后续版本按需引入）

## 静态类型系统

VX 是**纯静态类型**语言，编译期必须显式声明所有变量、函数参数、结构体/类字段的类型，不再支持 `var` 类型推断。

```vx
# 正确：显式类型声明
func add(a: int, b: int) -> int:
    return a + b

func main():
    x: int = 10
    y: int = add(x, 5)
    out(y)
```

```vx
# 错误：不再支持 var 推断
var x = 10        # 解析错误
func f(a): ...    # 参数缺少类型
```

原生标量类型：`int`、`float`、`double`、`bool`、`void`。`string` 和 `vector` 已降级为标准库类型 `std::String` / `std::Vec<T>`。自定义类型（`struct`、`class`、`enum`、`union`）通过标识符引用。

## 内存安全模型

VX 编译器内置编译期所有权/借用检查和 Rust 完整内存模型：

### 可变性

| 关键字   | 说明             | 示例                      |
| -------- | ---------------- | ------------------------- |
| `mut`  | 声明可变变量     | `mut x: int = 1`          |
| `&`    | 不可变引用（借用）| `r: pointer = &x`         |
| `&mut` | 可变引用（借用）  | `r: pointer = &mut x`     |

### 所有权转移

| 关键字   | 说明             | 示例                      |
| -------- | ---------------- | ------------------------- |
| `new`  | 构造实例         | `p: pointer = new Point(10, 20)` |
| `move` | 转移所有权       | `q: pointer = move p`            |

### Copy 语义

标量类型（`int`、`float`、`double`、`bool`）默认实现 `Copy`，赋值时复制而非转移所有权：
```vx
func main():
    a: int = 1
    b: int = a    # 复制，不是 move
    c: int = a    # a 仍然可用
```

### 借用规则（aliasing XOR mutation）

编译器 `OwnershipChecker` 严格实施 Rust 借用规则：

- **一个可变借用（`&mut`）** 或 **多个不可变借用（`&`）**，不可并存
- 可变借用期间，原变量被**冻结**（不可读写）
- 不可变变量不能创建可变借用

### 编译期检测

- use-after-move / use-after-free
- double-free
- 对不可变变量赋值
- 从不可变变量创建可变借用
- 活跃借用冲突
- 内存泄漏（作用域结束时未释放的堆变量）

## AOT 编译（Cranelift 后端）

启用 `--features aot` 后，通过 Cranelift 将 TypeIR 编译为原生机器码：

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

```
// src/lib.rs — 添加一行声明
mod compiler_new_module;

// 然后在 src/compiler_new_module.rs 中编写代码
```

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
| `target-lexicon`      | 目标平台三元组解析      |
| `serde`               | 缓存文件序列化          |
| `toml`                | vxsetting.toml 配置解析 |
| `cranelift-*`（可选） | AOT 原生代码生成        |

## 许可证

AGPL-3.0 — 详见 [LICENSE](LICENSE)
