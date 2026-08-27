# VX-ToolKit

![Language](https://img.shields.io/badge/language-Go-blue)
![Version](https://img.shields.io/badge/version-v1.6.0-blue)
![License](https://img.shields.io/badge/license-AGPL--3.0-green)
![Platform](<https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey>)

**VX 编程语言的完整工具链** — 编译器、原生链接器、包管理器，全栈 Go 实现。

> 语言版本: v1.6.0
> 协议: [AGPL-3.0](LICENSE)

---

## 目录

- [项目介绍](#项目介绍)
- [核心特性](#核心特性)
- [编译流水线](#编译流水线)
- [快速开始](#快速开始)
- [使用指南](#使用指南)
- [语言特性](#语言特性)
- [VXOBJ v4 格式](#vxobj-v4-格式)
- [项目结构](#项目结构)
- [依赖概览](#依赖概览)
- [贡献指南](#贡献指南)
- [变更日志](#变更日志)
- [许可](#许可)

---

## 项目介绍

VX-ToolKit 是 VX 语言的完整工具链实现，采用 **Go** 语言实现，提供完整的编译器、链接器、包管理器和开发工具：

| 模块                  | 实现语言 | 职责                              | 状态      |
| --------------------- | -------- | --------------------------------- | --------- |
| 编译器 (vxc)          | Go       | 源码分析、TypeIR 生成、VXOBJ 输出 | ✅ 稳定   |
| 原生链接器 (vlnk)     | Go       | TypeIR 代码生成 + 可执行文件生成  | ✅ 稳定   |
| VPM 包管理器 (vpm)    | Go       | 多语言包管理                      | ✅ 可用   |
| 反编译器 (vxde)       | Go       | VXOBJ → VX 源码还原              | ✅ 可用   |
| 格式化工具 (vfmt)     | Go       | VX 源码格式化                     | ✅ 可用   |
| 项目构建器 / 增量缓存 | Go       | vxsetting.toml 驱动的构建系统     | 🚧 开发中 |
| LSP 语言服务器        | Go       | 代码补全、悬停、跳转定义等        | 🚧 开发中 |

### 编译流水线

```
.vx 源码
  ↓ Lexer → Token 流
  ↓ Parser → AST
  ↓ OwnershipChecker → 内存安全验证
  ↓ TypeIR 生成器 → 类型化中间表示
  ↓ serialize → VXOBJ v4 跨平台文件
  ↓ vlnk 解析 TypeIR
  ↓ 原生代码生成 (x86_64 / aarch64 / ARM32 / RISC-V)
  ↓ ELF / Mach-O / PE 可执行文件
```

### 核心特性

- **纯静态类型系统** — 所有变量/参数/字段必须显式声明类型，无隐式类型转换
- **内存安全模型** — 编译期所有权 + 借用检查 (aliasing XOR mutation)，类 Rust 语义
- **原生编译** — TypeIR 直接编译为原生机器码，无 VM 解释执行层，零运行时开销
- **跨平台中间格式** — VXOBJ v4 不包含平台特征，一次编译、多平台链接
- **多架构支持** — x86_64（稳定）、aarch64 / ARM32 / RISC-V（实验性）
- **VPM 包管理器** — 多语言包管理 (Python/TypeScript/Java/Rust/Go/C/C++)
- **编译时宏系统** — 参数化宏，零运行时开销
- **反编译与反链接** — 支持从 VXOBJ 还原源码，从可执行文件提取中间表示

---

## 快速开始

### 环境要求

| 组件      | 最低版本                |
| --------- | ----------------------- |
| Go 工具链 | 1.21+                   |
| 操作系统  | Linux / macOS / Windows |

### 获取源码

```bash
git clone https://gitee.com/vx-language-dev/vx-toolkit.git
cd vx-toolkit
```

### 构建工具链

```bash
# 进入 Go 目录
cd go

# Debug 构建（默认）
go build -o vxc ./cmd/vxc
go build -o vlnk ./cmd/vlnk
go build -o vpm ./cmd/vpm
go build -o vxde ./cmd/vxde
go build -o vfmt ./cmd/vfmt

# 或使用 Makefile（推荐）
make build
```

构建产物位于 `go/` 目录：

| 二进制文件      | 说明             |
| --------------- | ---------------- |
| `vxc`         | VX 编译器        |
| `vlnk`        | 原生链接器       |
| `vpm`         | 包管理器         |
| `vxde`        | 反编译器         |
| `vfmt`        | 格式化工具       |
| `vxlsp`       | LSP 语言服务器   |
| `vxbootstrap` | VXOBJ 引导生成器 |

### 安装到系统

```bash
# 使用 Makefile 安装到 ~/.local/bin
cd go
make install

# 确保 ~/.local/bin 在 PATH 中
export PATH="$HOME/.local/bin:$PATH"
```

### 运行测试

```bash
# 运行全部 Go 单元测试
cd go
make test

# 或直接使用 go test
go test ./...
```

### Hello World

创建 `hello.vx`：

```vx
func main():
    out("Hello, VX!")
```

编译并运行：

```bash
# 编译为 VXOBJ
vxc hello.vx

# 链接为原生可执行文件
vlnk hello.vxobj -o hello

# 运行
./hello
```

---

## 使用指南

### 1. 编译 VX 源码

```bash
# 编译单文件（输出 hello.vxobj）
vxc hello.vx

# 指定输出文件名
vxc hello.vx -o output.vxobj

# 查看编译过程详细信息
vxc hello.vx -v
```

编译器流水线：

1. **词法分析** (Lexer) → Token 流
2. **语法分析** (Parser) → AST
3. **所有权/借用检查** (OwnershipChecker) → 内存安全性验证
4. **TypeIR 生成** + 序列化 → VXOBJ v4

### 2. 链接为可执行文件

```bash
# 默认原生链接
vlnk hello.vxobj -o hello

# 查看 VXOBJ 文件信息
vlnk hello.vxobj --dump

# 嵌入 VXOBJ 数据（支持后续反链接）
vlnk hello.vxobj -o hello --embed-vxobj
```

链接器自动完成：

- 解析 TypeIR → 生成目标架构机器码
- 写入 ELF / Mach-O / PE 可执行文件头
- 处理入口符号、段布局与重定位
- （可选）追加 VXOBJ 数据供反链接使用

### 3. 项目构建 (vxsetting.toml)

多文件项目在根目录创建 `vxsetting.toml`：

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

使用构建器统一编译：

```bash
vxc
```

### 4. VPM 包管理器

```bash
# 安装包
vpm install <包名>

# 列出已安装包
vpm list
```

支持语言：Python、TypeScript、JavaScript、Java、Rust、Go、C、C++。

### 5. 反编译与反链接

```bash
# 反编译 VXOBJ → VX 源码
vxde input.vxobj -o output.vx
```

### 6. LSP 语言服务器

LSP 已构建为独立可执行文件 `vxlsp`，位于 `src/lsp/`：

| 功能     | 说明                                     | 状态    |
| -------- | ---------------------------------------- | ------- |
| 代码补全 | 关键字、函数名、变量名、成员访问、import | ✅ 可用 |
| 悬停信息 | 类型签名、字段/方法文档、内置函数说明    | ✅ 可用 |
| 符号导航 | 工作区符号、文档符号                     | ✅ 可用 |
| 诊断     | 编译错误实时提示，自动发布到编辑器       | ✅ 可用 |
| 跳转定义 | 变量/函数/参数/导入别名定义跳转          | ✅ 可用 |
| 内嵌提示 | 类型推断、参数名提示                     | ✅ 可用 |

> **使用方式**: 编辑器配置 LSP 服务器路径为 `vxlsp`，支持标准 Language Server Protocol。

---

## 语言特性

### 类型系统

VX 是**纯静态类型**语言，所有变量、参数、函数返回类型必须显式声明：

```vx
func add(a: int, b: int) -> int:
    return a + b

func main():
    x: int = 10
    y: int = add(x, 5)
    out(y)
```

**原生标量类型**: `int`、`float`、`double`、`bool`、`void`
**复合类型**: `struct`、`class`、`enum`、`union`
**标准库类型**: `std::String`、`std::Vec<T>`

### 内存安全模型

#### 可变性

| 关键字   | 说明               | 示例                    |
| -------- | ------------------ | ----------------------- |
| `mut`  | 声明可变变量       | `mut x: int = 1`      |
| `&`    | 不可变引用（借用） | `r: pointer = &x`     |
| `&mut` | 可变引用（借用）   | `r: pointer = &mut x` |

#### 所有权规则

- 每个值有且仅有一个所有者
- 使用 `move` 显式转移所有权
- 标量类型 (`int` / `float` / `double` / `bool`) 默认 Copy 语义

```vx
func main():
    a: int = 1
    b: int = a    # Copy，a 仍然可用
    c: int = a    # 合法
```

#### 借用规则 (Aliasing XOR Mutation)

- 同一时刻：**一个可变借用 `&mut`** 或 **多个不可变借用 `&`**，二者不可并存
- 可变借用期间，原变量被**冻结**，不可直接访问
- 不可变变量无法创建可变借用

编译期自动检测：use-after-move、double-free、对不可变变量赋值、活跃借用冲突、内存泄漏。

### 编译时宏系统

参数化宏在编译期展开，零运行时开销，支持条件编译、代码生成等高级用法。

---

## VXOBJ v4 格式

VXOBJ 是 VX 语言的跨平台中间容器格式，不包含任何平台特定的可执行特征，由链接器在目标平台上生成对应格式的可执行文件。

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
├── go/                       # Go 源码目录
│   ├── cmd/                  # 命令行工具入口
│   │   ├── vxc/              # VX 编译器
│   │   ├── vlnk/             # 原生链接器
│   │   ├── vpm/              # 包管理器
│   │   ├── vxde/             # 反编译器
│   │   ├── vfmt/             # 格式化工具
│   │   └── vxlsp/            # LSP 语言服务器
│   │
│   ├── internal/             # 内部包
│   │   ├── lexer/            # 词法分析器
│   │   ├── parser/           # 语法分析器
│   │   ├── semantic/         # 语义分析器
│   │   ├── compiler/         # 编译器核心
│   │   ├── compiler_bytecode/# 编译器字节码
│   │   ├── typeirgen/        # TypeIR 生成器
│   │   ├── typeir/           # TypeIR 定义
│   │   ├── type_ir/          # TypeIR 序列化
│   │   ├── bytecode/         # VXOBJ v4 格式
│   │   ├── linker/           # 原生链接器
│   │   ├── llvmir/           # LLVM IR 生成
│   │   ├── translator/       # 代码翻译
│   │   ├── builder/          # 项目构建器
│   │   ├── cache/            # 增量编译缓存
│   │   ├── vxsetting/        # vxsetting.toml 配置解析
│   │   ├── ownership/        # 所有权检查
│   │   ├── optimize/         # 优化器
│   │   ├── macros/           # 宏系统
│   │   ├── opcode/           # 操作码定义
│   │   ├── token/            # Token 定义
│   │   ├── ast/              # AST 定义
│   │   ├── logger/           # 日志系统
│   │   ├── parallel_build/   # 并行构建
│   │   ├── stubs/            # 存根定义
│   │   ├── lsp/              # LSP 语言服务器
│   │   └── vfmt/             # 格式化工具
│   │
│   ├── build.sh              # 构建脚本
│   ├── build_all.sh          # 全量构建脚本
│   ├── go.mod                # Go 模块定义
│   └── go.sum                # Go 依赖锁定
│
├── std/                      # VX 标准库源码
├── ci/                       # CI 配置
├── .gitee/                   # Gitee 配置
├── .github/                  # GitHub 配置
├── Makefile                  # Make 构建脚本
├── CHANGELOG.md              # 变更日志
├── CONTRIBUTING.md           # 贡献指南
├── INSTALL_SCRIPTS.md        # 安装脚本说明
├── LICENSE                   # AGPL-3.0 协议
└── README.md                 # 项目说明（本文件）
```

---

## 依赖概览

### Go 部分

**纯自研，零外部依赖。** 工具链完全由 Go 实现，无第三方依赖。

| 架构      | 状态      |
| --------- | --------- |
| x86_64    | ✅ 稳定   |
| aarch64   | 🧪 实验性 |
| ARM32     | 🧪 实验性 |
| RISC-V 32 | 🧪 实验性 |
| RISC-V 64 | 🧪 实验性 |

### 架构说明

| 架构      | 三元组示例              | 说明                  |
| --------- | ----------------------- | --------------------- |
| x86_64    | `x86_64-linux-gnu`    | 64位 x86 处理器       |
| aarch64   | `aarch64-linux-gnu`   | ARM 64位 (Apple M1+)  |
| ARM32     | `arm-linux-gnueabihf` | ARM 32位 (嵌入式/IoT) |
| RISC-V 32 | `riscv32-linux-gnu`   | RISC-V 32位           |
| RISC-V 64 | `riscv64-linux-gnu`   | RISC-V 64位           |

---

## API 参考

### 编译器 API

```go
import "vx-toolkit/go/internal/compiler"

// 创建编译器
compiler := compiler.NewCompiler(allocator)

// 编译源文件
result, err := compiler.CompileFile("main.vx")
if err != nil {
    log.Fatal(err)
}

// 获取 TypeIR
typeir := result.TypeIR
```

### 链接器 API

```go
import "vx-toolkit/go/internal/linker"

// 创建链接器
linker := linker.NewLinker("x86_64-linux-gnu")

// 链接 VXOBJ 文件
err := linker.Link([]string{"main.vxobj"}, "output")
if err != nil {
    log.Fatal(err)
}
```

### 缓存 API

```go
import "vx-toolkit/go/internal/cache"

// 创建缓存
cache := cache.New(cacheDir)

// 检查缓存有效性
if cache.IsTargetFresh(key, sources, optLevel) {
    // 跳过编译
}

// 更新缓存
err := cache.UpdateEntry(key, sources, objPath, output, optLevel)
if err != nil {
    log.Fatal(err)
}

// 持久化
err = cache.SaveToDisk()
```

### 构建器 API

```go
import "vx-toolkit/go/internal/builder"

// 从配置文件创建构建器
settings, err := builder.LoadSettings("vxsetting.toml")
if err != nil {
    log.Fatal(err)
}

b := builder.NewBuilder(settings)

// 配置选项
b.WithForceRebuild(true)
b.WithNoCache(false)

// 执行构建
err = b.Build()
if err != nil {
    log.Fatal(err)
}
```

### 日志 API

```go
import "vx-toolkit/go/internal/logger"

// 创建日志器
logger := logger.New(writer, logger.InfoLevel)

// 基础日志
logger.info("编译开始: {s}", .{filename});
logger.warn("未使用的变量: {s}", .{var_name});
logger.err("编译失败: {s}", .{error_msg});

// 带上下文的错误日志
logger.logError("类型不匹配", .{}, .{
    .file = "main.vx",
    .line = 42,
    .column = 10,
    .hint = "尝试使用 `as` 进行类型转换",
});
```

---

## 高级用法

### 多文件项目

创建 `vxsetting.toml`:

```toml
[bin]
source = ["main.vx", "utils.vx", "network.vx"]
version = "2.0.0"
output = "dist/myapp"
o = 7

[vxset]
deadcode = false
cache = true

[libraries]
mylib = "packages/mylib"
```

### 编译时宏

```vx
macro assert(cond: bool):
    if not cond:
        panic("assertion failed")

macro max(a: int, b: int) -> int:
    if a > b:
        return a
    else:
        return b
```

### 所有权与借用

```vx
func process(data: &mut Data):
    // 可变借用
    data.value = 42

func read(data: &Data) -> int:
    // 不可变借用
    return data.value

func main():
    mut data: Data = Data{ .value = 0 }
  
    // 可变借用
    process(&mut data)
  
    // 不可变借用
    val: int = read(&data)
  
    // 所有权转移
    moved: Data = move data
```

---

## 贡献指南

欢迎参与 VX-ToolKit 的开发！请先阅读 [CONTRIBUTING.md](CONTRIBUTING.md) 了解贡献流程、代码规范和开发约定。

### 快速参与

1. Fork 本仓库
2. 创建特性分支 (`git checkout -b feature/awesome-feature`)
3. 提交更改 (`git commit -m 'Add some awesome feature'`)
4. 推送到分支 (`git push origin feature/awesome-feature`)
5. 提交 Pull Request

### 开发环境

```bash
# 克隆后运行测试确保环境正常
make test

# Debug 模式构建（编译快，包含调试信息）
make build

# 构建 LSP 语言服务器
make build
# 产物位于 bin/vxlsp
```

### 运行特定测试

```bash
# 运行全部测试
make test
```

---

## 变更日志

详细版本变更请查看 [CHANGELOG.md](CHANGELOG.md)。

---

## 许可

AGPL-3.0 — 详见 [LICENSE](LICENSE)

> 本项目为完全独立自研项目。编译器全部代码从零编写，没有复刻、修改、打包任何第三方编译器源码。
> 仅用于编译原理研究与开源协作，不会封装为线上 SaaS 服务对外运营。
