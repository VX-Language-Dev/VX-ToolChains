# VX-ToolKit

**VX 编程语言的完整工具链** — 编译器、原生链接器、包管理器。

> 语言版本: v1.6.0
> 协议: [AGPL-3.0](LICENSE)

---

## 项目介绍

VX-ToolKit 是 VX 语言的完整工具链实现，核心模块已从 Rust 迁移到 **Zig**：

| 模块                     | 实现语言 | 职责                       |
| ------------------------ | -------- | -------------------------- |
| 编译器 (vxc)             | Zig      | 源码分析、TypeIR 生成      |
| 原生链接器 (vlnk)        | Zig      | TypeIR 代码生成 + 可执行文件生成 |
| VPM 包管理器 (vpm)       | Zig      | 多语言包管理               |
| 项目构建器 / 增量缓存    | Rust     | vxsetting.toml 驱动的构建系统 |
| 链接器后端 (lld_linker)  | Rust     | LLD 原生链接               |

> **注意**: LSP 语言服务器、反编译器、反链接器的 Rust 实现已被移除，
> 等待后续 Zig 实现。

### 编译流水线

```
.vx 源码
  ↓ Lexer → Token 流
  ↓ Parser → AST
  ↓ OwnershipChecker → 内存安全验证
  ↓ TypeIR 生成器 → 类型化中间表示
  ↓ serialize → VXOBJ v4 跨平台文件
  ↓ Zig vlnk 解析 TypeIR
  ↓ 原生代码生成 (x86_64 / aarch64 / ARM32 / RISC-V)
  ↓ ELF / Mach-O / PE 可执行文件
```

### 核心特性

- **纯静态类型系统** — 所有变量/参数/字段必须显式声明类型
- **内存安全模型** — 编译期所有权 + 借用检查 (aliasing XOR mutation)，类 Rust 语义
- **原生编译** — TypeIR 直接编译为原生机器码，无 VM 解释执行层
- **跨平台中间格式** — VXOBJ v4 不包含平台特征，链接器自动适配目标平台
- **多架构支持** — x86_64、aarch64、ARM32、RISC-V (rv32/rv64)
- **VPM 包管理器** — 多语言包管理 (Python/TypeScript/Java/Rust/Go/C/C++)
- **编译时宏系统** — 参数化宏，零运行时开销

---

## 快速开始

### 环境要求

| 组件        | 最低版本      |
| ----------- | ------------- |
| Zig 工具链  | 0.13+         |
| 操作系统    | Linux / macOS / Windows |

### 构建

```bash
# 1. 克隆仓库
git clone https://gitee.com/vx-language-dev/vx-toolkit.git
cd vx-toolkit

# 2. 构建工具链 (Debug)
zig build

# 构建工具链 (Release)
zig build -Doptimize=ReleaseSafe

# 构建产物:
#   zig-out/bin/vxc    — VX 编译器
#   zig-out/bin/vlnk   — 原生链接器
#   zig-out/bin/vpm    — 包管理器
```

### 运行测试

```bash
# Zig 测试
zig build test

# Rust 库测试 (builder/cache/vxsetting)
cargo test

---

## 使用指南

### 1. 编译 VX 源码

```bash
# 编译单文件
vxc hello.vx

# 指定输出
vxc hello.vx -o hello.vxobj

# 查看编译过程信息
vxc hello.vx -v
```

编译器流水线:
1. **词法分析** (Lexer) → Token 流
2. **语法分析** (Parser) → AST
3. **所有权/借用检查** (OwnershipChecker) → 内存安全性验证
4. **TypeIR 生成** + 序列化 → VXOBJ v4

### 2. 链接为可执行文件

```bash
# 默认原生链接
vlnk hello.vxobj -o hello

# 查看 VXOBJ 信息
vlnk hello.vxobj --dump

# 嵌入 VXOBJ 数据（支持反链接）
vlnk hello.vxobj -o hello --embed-vxobj
```

链接器自动:
- 解析 TypeIR → 生成目标架构机器码
- 写入 ELF/Mach-O/PE 可执行文件头
- 处理入口符号、段布局
- (可选) 追加 VXOBJ 数据供反链接

### 3. 项目构建 (vxsetting.toml)

多文件项目在根目录创建 `vxsetting.toml`:

```toml
[bin]
source = ["main.vx", "util.vx"]
version = "1.0.0"
output = "dist/myapp"

[[module]]
info = "pkg/mymod/info.toml"
name = "mymod"
source = ["mymod/lib.vx"]

[vxset]
optimization = 20
cache = true
```

使用构建器统一编译:
```bash
vxc
```

### 4. LSP 语言服务器

```bash
vx-lsp
```

支持的功能:

| 功能     | 说明                                           |
| -------- | ---------------------------------------------- |
| 代码补全 | 关键字、函数名、变量名、成员访问、import 路径  |
| 悬停信息 | 类型签名、字段/方法文档、内置函数说明          |
| 符号导航 | 工作区符号、文档符号                           |
| 诊断     | 编译错误实时提示                               |
| 跳转定义 | 变量/函数/参数/导入别名定义跳转                |

### 5. VPM 包管理器

```bash
vpm install <包名>
vpm list
```

支持语言: Python、TypeScript、JavaScript、Java、Rust、Go、C、C++。

### 6. 反编译与反链接

```bash
# 反编译 VXOBJ → VX 源码
vxde input.vxobj -o output.vx

# 从可执行文件提取嵌入的 VXOBJ
vdlnk input.bin -o extracted.vxobj
```

---

## 语言特性

### 类型系统

VX 是**纯静态类型**语言，所有变量、参数、函数返回类型必须显式声明:

```vx
func add(a: int, b: int) -> int:
    return a + b

func main():
    x: int = 10
    y: int = add(x, 5)
    out(y)
```

原生标量类型: `int`、`float`、`double`、`bool`、`void`  
复合类型: `struct`、`class`、`enum`、`union`  
标准库类型: `std::String`、`std::Vec<T>`

### 内存安全模型

#### 可变性

| 关键字   | 说明               | 示例                    |
| -------- | ------------------ | ----------------------- |
| `mut`    | 声明可变变量       | `mut x: int = 1`        |
| `&`      | 不可变引用（借用） | `r: pointer = &x`       |
| `&mut`   | 可变引用（借用）   | `r: pointer = &mut x`   |

#### 所有权规则

- 每个值有唯一所有者
- `move` 转移所有权
- 标量类型 (`int`/`float`/`double`/`bool`) 默认 Copy

```vx
func main():
    a: int = 1
    b: int = a    # Copy
    c: int = a    # a 仍然可用
```

#### 借用规则 (aliasing XOR mutation)

- 一个可变借用 **`&mut`** 或 多个不可变借用 **`&`**，不可并存
- 可变借用期间原变量被**冻结**
- 不可变变量不能创建可变借用

编译期检测: use-after-move、double-free、对不可变变量赋值、活跃借用冲突、内存泄漏。

### VXOBJ v4 格式

跨平台容器格式，不包含平台特定可执行特征:

```
[Header]
  Magic:    "VXOBJ" (5 bytes)
  Version:  4 (u32 BE)
  Flags:    u32 BE
  Target:   Triple string (e.g. "x86_64-unknown-linux-gnu")

[Section Index]
  count: u32 BE
  For each section:
    name, offset, size

[Sections]
  TypeIR         — 类型化中间表示 (代码与类型信息)
  ExternalDeps   — 外部依赖声明 (可选)
```

---

## 项目结构

```
.
├── src/                  # Rust 源码
│   ├── ipt.rs            编译器 CLI 入口
│   ├── vxlinker.rs       链接器 CLI 入口 (Zig vxlinker 包装)
│   ├── lib.rs            库入口 (编译器核心、LSP、工具)
│   ├── compiler_core.rs  编译器主流程
│   ├── compiler_expr.rs  表达式编译
│   ├── compiler_stmt.rs  语句编译
│   ├── compiler_typeir.rs TypeIR 生成
│   ├── compiler_ownership.rs 所有权/借用检查
│   ├── compiler_module.rs 模块系统
│   ├── compiler_bytecode.rs 字节码定义
│   ├── parser/           语法分析器
│   ├── token.rs          词法分析
│   ├── type_ir.rs        TypeIR 定义与序列化
│   ├── bytecode.rs       VXOBJ v4 格式
│   ├── builder.rs        项目构建器
│   ├── pm.rs             包管理器
│   ├── lsp/              LSP 语言服务器
│   ├── macros.rs         编译时宏系统
│   ├── decompiler.rs     反编译器
│   ├── delinker.rs       反链接器
│   ├── lld_linker.rs     LLD 链接器 (备用)
│   ├── target_profile.rs 目标平台配置
│   └── aot_backend.rs    Cranelift AOT (实验性)
├── editors/vscode/       # VS Code 扩展
├── dist/                 # 运行时库分发
├── Cargo.toml
└── README.md
```

---

## 依赖概览

### Rust 依赖

| 依赖              | 用途                      |
| ----------------- | ------------------------- |
| `tower-lsp`       | LSP 协议实现              |
| `lsp-types`       | LSP 类型定义              |
| `tokio`           | LSP 异步运行时            |
| `dashmap`         | 并发文档状态管理          |
| `target-lexicon`  | 目标平台三元组解析        |
| `serde`           | 缓存文件序列化            |
| `toml`            | vxsetting.toml 配置解析   |

### Zig 依赖

纯自研，无外部依赖。代码生成器覆盖:

- x86_64 (稳定)
- aarch64 (实验性)
- ARM32 (实验性)
- RISC-V 32/64 (实验性)

---

## 许可

AGPL-3.0 — 详见 [LICENSE](LICENSE)

> 本项目为完全独立自研项目。编译器全部代码从零编写，没有复刻、修改、打包任何第三方编译器源码。
> 仅用于编译原理研究与开源协作，不会封装为线上 SaaS 服务对外运营。