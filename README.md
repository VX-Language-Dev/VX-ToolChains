# VX-ToolKit

![Language](https://img.shields.io/badge/language-C++-green)
![Version](https://img.shields.io/badge/version-v1.9.0-blue)
![License](https://img.shields.io/badge/license-AGPL--3.0-green)
![Platform](<https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey>)

**VX 编程语言的完整工具链** — 编译器、原生链接器、开发工具，以 C++23 实现。

> 语言版本: v1.7.0
> 协议: [AGPL-3.0](LICENSE)
> 当前实现: **C++ 工具链核心可用**（编译前端 vxc003、后端 vlnk、构建编排 vmake、
> vxls / vx-tidy；数组/字典、PE 等特性开发中，见 [CHANGELOG](CHANGELOG.md)）

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
- [API 参考](#api-参考)
- [高级用法](#高级用法)
- [贡献指南](#贡献指南)
- [变更日志](#变更日志)
- [许可](#许可)

---

## 项目介绍

VX-ToolKit 是 VX 语言的完整工具链实现，采用 **C++23** 实现（CMake + Ninja 构建），提供编译器、链接器与开发工具：

| 模块                  | 实现语言 | 职责                              | 状态      |
| --------------------- | -------- | --------------------------------- | --------- |
| 编译器 (vxc)          | C++      | 源码分析、TypeIR 生成、VXOBJ 输出 | ✅ 基础可用 |
| 原生链接器 (vlnk)     | C++      | LLVM IR 代码生成 + 可执行文件/动态库生成 | ✅ 基础可用 |
| VPM 包管理器 (vpm)    | C++      | 多语言包管理                      | 🚧 开发中 |
| 反编译器 (vxde)       | C++      | VXOBJ → VX 源码还原              | 🚧 开发中 |
| 格式化工具 (vfmt)     | C++      | VX 源码格式化                     | 🚧 开发中 |
| 项目构建器 (vmake)    | C++      | VMakeLists.toml 驱动，编排 vxc/vlnk | ✅ 基础可用 |
| LSP 语言服务器 (vxls)  | C++      | 代码补全、悬停、跳转定义等        | ✅ 基础可用 |
| 静态分析 (vx-tidy)     | C++      | AST 级规则检查（VXTIDYxxx）       | ✅ 基础可用 |
| Util 基础库           | C++      | 内存分配器、字符串、文件系统      | ✅ 稳定   |

### 编译流水线

```
.vx 源码
  ↓ Lexer → Token 流
  ↓ Parser → AST
  ↓ @imp / @cppimp 导入表（VX ABI 符号 + 链接依赖）
  ↓ Codegen → TypeIR（栈机指令序列）
  ↓ serialize → VXOBJ v4 跨平台文件
  ↓ vlnk 解析 VXOBJ → LLVM IR（.ll 文本）
  ↓ 宿主 clang 编译链接（--target 传 -target 交叉编译）
  ↓ 原生可执行文件（macOS Mach-O / Linux ELF）
```

> 规划中：OwnershipChecker 所有权/借用检查、直接机器码发射与 PE 格式支持。

### 核心特性

- **纯静态类型系统** — 所有变量/参数/字段必须显式声明类型，无隐式类型转换
- **内存安全模型** — 编译期所有权 + 借用检查 (aliasing XOR mutation)，类 Rust 语义（规划中，检查器尚未实现）
- **原生编译** — Codegen 产出 TypeIR，后端发射 LLVM IR 交宿主 clang 生成原生机器码，无 VM 解释执行层
- **跨平台中间格式** — VXOBJ v4 不包含平台特征，一次编译、多平台链接
- **多架构支持** — 目标由宿主 clang 决定（开发与测试以 x86_64 / aarch64 为主）
- **VPM 包管理器** — 多语言包管理 (Python/TypeScript/Java/Rust/Go/C/C++)
- **编译时宏系统** — 参数化宏，零运行时开销
- **反编译与反链接** — 支持从 VXOBJ 还原源码，从可执行文件提取中间表示

---

## 快速开始

### 环境要求

| 组件        | 最低版本                |
| ----------- | ----------------------- |
| CMake       | 3.23+                   |
| Ninja       | 1.10+                   |
| C++ 编译器  | 支持 C++23 (Clang 16+, GCC 13+, MSVC 19.35+) |
| clang       | vlnk 链接依赖（Xcode CLT / LLVM，任意近期版本） |
| 操作系统    | Linux / macOS / Windows |

### 获取源码

```bash
git clone https://gitee.com/vx-language-dev/vx-toolkit.git
cd vx-toolkit
```

### 构建工具链

```bash
# 获取源码
git clone https://gitee.com/vx-language-dev/vx-toolkit.git
cd vx-toolkit

# 使用 CMake + Ninja 构建（推荐）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build --config MinSizeRel
```

构建产物位于 `build/` 目录：

| 产物                          | 说明                                    |
| ----------------------------- | --------------------------------------- |
| `vxc002`                      | 编译器前端 CLI（`--version` 显示 vxc003：lex → parse → codegen → .vxobj） |
| `vlnk`                        | 原生链接器 CLI（.vxobj → LLVM IR → clang 链接） |
| `tools/vxls/vxls`             | LSP 语言服务器                          |
| `tools/vx-tidy/vx-tidy`       | 静态分析 CLI                            |
| `tools/vmake/vmake`           | 项目构建编排器（VMakeLists.toml）       |
| `libvx_util.a` / `libvx_compiler.a` / `libvx_backend.a` | 静态库 |
| `tools/vx-tidy/libvxtidy.dylib` | vx-tidy 检查引擎动态库                |

> 另有 15 组测试可执行文件（`vx_*_tests`），由 CTest 统一调度。

### 安装到系统

```bash
# 使用安装脚本安装到 ~/.local/bin
zsh install.zsh
```

> 注：CMake 暂未定义 `install()` 规则；`install.zsh` 会复制 `build/tools/*` 下的二进制
> 到 `~/.local/bin`，请确保该目录在 PATH 中。

### 运行测试

```bash
# 运行全部 C++ 单元测试
cmake --build build --target test
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
./build/vxc002 hello.vx

# 链接为原生可执行文件
./build/vlnk hello.vxobj -o hello

# 运行
./hello
```

---

## 使用指南

### 1. 编译 VX 源码

```bash
# 编译单文件（输出 hello.vxobj）
./build/vxc002 hello.vx

# 指定输出文件名
./build/vxc002 hello.vx -o output.vxobj

# 查看编译过程详细信息
./build/vxc002 hello.vx -v
```

编译器流水线：

1. **词法分析** (Lexer) → Token 流
2. **语法分析** (Parser) → AST
3. **库导入解析** (@imp / @cppimp) → 导入表（VX ABI 符号签名 + 链接依赖）
4. **Codegen** → TypeIR 栈机指令 + 序列化 → VXOBJ v4

> 所有权/借用检查（OwnershipChecker）为规划特性，尚未实现。

### 2. 链接为可执行文件

```bash
# 默认原生链接（宿主平台）
./build/vlnk hello.vxobj -o hello

# 查看 VXOBJ 文件信息（恢复的 ModuleIR 摘要）
./build/vlnk hello.vxobj --dump

# 仅发射 LLVM IR 到 stdout（不链接）
./build/vlnk hello.vxobj --emit-ir

# 显式指定目标三元组（交叉编译，透传 clang -target）
./build/vlnk hello.vxobj -o hello --target aarch64-unknown-linux-gnu

# 多文件目标：多个 .vxobj 合并为一个模块后再链接
./build/vlnk main.vxobj util.vxobj -o hello

# 输出动态库（clang -shared -fPIC，不生成 @main 入口包装）
./build/vlnk lib.vxobj --shared -o libmylib.dylib
```

链接器自动完成：

- 解析并合并多个 VXOBJ → 发射 LLVM IR（.ll 文本，含 vxrt 运行时常量）
- 调用宿主 clang 编译链接（`--target` 非宿主时自动交叉编译）
- @imp/@cppimp 的实现文件（.c/.a/.dylib/.cpp）与 C++ shim 一并参与链接

> PE（Windows）可执行文件生成尚未支持；`--embed-vxobj` 反链接嵌入待实现。

### 3. 库导入与 VX ABI (@imp / @cppimp)

通过 `@imp` 导入 VX 库，通过 `@cppimp` 引用 C++ 头文件（链接期自动生成 shim）。
导入符号以**库名前缀**限定访问。

```vx
@imp(math)                    # 根目录库：直接引用文件名
@imp(math) as m               # 别名导入：m.add(...) 等价 math.add(...)
@imp(/collections/vec)        # 子目录库：'/' + 子目录路径，库名取末段 → vec
@imp(./mylib)                 # 自定义库：相对源文件目录
@cppimp(cpp/greet.hpp)        # C++ 头文件 → 链接期生成 C++ ABI → VX ABI 的 shim

func main():
    out_int(math.add(2, 3))   # math 库 → 符号 _vx_math_add
    out_int(m.mul(6, 7))      # 别名 m → 同 _vx_math_mul
    out_int(vec.sum_to(10))   # vec 库  → 符号 _vx_vec_sum_to
    out(greet.greet("VX"))    # greet 库 → shim 包装 C++ 符号
```

**库查找规则**：库默认位于 vxc 可执行文件同级的 `headers/` 目录及其子目录下
（可用环境变量 `VX_HEADERS` 覆盖）。

**库文件形态**：VX 声明头 `.vxh`（VX 语法的 `extern` 签名声明），
同目录同名的 `.c` / `.a` / `.dylib` / `.cpp` 由 `vlnk` 在链接期并入。
`@cppimp` 的 VX 签名取自与 C++ 头文件**同目录、同主名**的 `.vxh` 伴生文件
（如 `greet.hpp` ↔ `greet.vxh`）。

**VX ABI**（`_vx_<lib>_<name>`）：

| VX 类型 | LLVM 类型 | C 类型（shim 用） |
| ------- | --------- | ----------------- |
| int     | i32       | int32_t           |
| float   | double    | double            |
| bool    | i32       | int32_t           |
| string  | ptr       | const char*       |
| void    | void      | void              |

**内置示例库**（`headers/`，覆盖全部 5 种 ABI 类型）：`math` / `fmath`（根库 int/float）、
`text`（string 入参、int/bool/void 返回）、`collections/vec`（子目录库）、`cpp/greet`（@cppimp）。

### 4. 项目构建（VMake，VMakeLists.toml）

vmake 位于 `tools/vmake/`。它读取项目根目录的 `VMakeLists.toml`，
**自身不编译任何代码**，而是按正确顺序在后台调用工具链：

```
每个源文件 → vxc <src> -o <obj>.vxobj
全部 .vxobj → vlnk <obj...> -o <产物> [--shared]
```

目前仅支持从源文件声明两类目标：可执行文件（`[[exe]]`）与动态链接库（`[[dylib]]`）。
动态库目标始终先于可执行文件构建；同一目标内的多个源文件由 `vlnk` 合并链接。

```toml
# VMakeLists.toml
[[exe]]
name = "hello"
sources = ["src/main.vx", "src/util.vx"]

[[dylib]]
name = "mylib"
sources = ["src/lib.vx"]
output = "libmylib.dylib"   # 可选，相对 -B 构建目录
```

构建目录必须显式用 `-B` 指定（不存在会自动创建），输出风格对齐 Cargo：

```bash
cmake -G Ninja -B build -S . && ninja -C build vmake

./build/tools/vmake/vmake build -B build-out
#    Compiling mylib (dylib)
#    Compiling hello (exe)
#     Finished build in 0.35s

./build/tools/vmake/vmake build -B build-out hello   # 只构建指定目标
./build/tools/vmake/vmake build -B build-out -v      # 打印实际 vxc/vlnk 命令
./build/tools/vmake/vmake build -B build-out --color=never
```

- **增量构建**：产物比配置与全部源文件新时跳过，仅打印 `Finished`（`-v` 显示 `Fresh`）。
- **工具链定位**：`VX_VXC` / `VX_VLNK` 可显式指定；否则在 vmake 可执行文件附近与 `PATH` 中查找。
- **导入库**：未设置 `VX_HEADERS` 时，自动尝试项目 `headers/` 与编译器旁的 `headers/`。
- 中间产物位于 `<构建目录>/obj/<目标>/`，最终产物默认：exe → `<name>`，
  dylib → `lib<name>.dylib`（macOS）/ `lib<name>.so`（Linux）。

### 5. VPM 包管理器 — 规划中

> vpm 尚未实现（模块状态见上表）。

```bash
# 安装包
vpm install <包名>

# 列出已安装包
vpm list
```

支持语言：Python、TypeScript、JavaScript、Java、Rust、Go、C、C++。

### 6. 反编译与反链接 — 规划中

> vxde 尚未实现（模块状态见上表）。

```bash
# 反编译 VXOBJ → VX 源码
vxde input.vxobj -o output.vx
```

### 7. LSP 语言服务器（vxls）

vxls 位于 `tools/vxls/`，C++ 实现的 LSP 服务器（stdio 传输），
复用 vx_compiler 前端做实时诊断与语义查询；**默认集成 vx-tidy**
（动态库 `libvxtidy.dylib`）静态检查，诊断合并发布：

| 功能     | 说明                                                       | 状态 |
| -------- | ---------------------------------------------------------- | ---- |
| 诊断     | 前端诊断 + vx-tidy 规则合并发布（`--no-tidy` 可关闭）      | ✅   |
| 悬停信息 | 签名+参数表+返回类型+#文档注释；类成员；局部变量/参数；内置运行时文档；导入库符号（VX ABI/来源 .vxh） | ✅   |
| 代码补全 | 关键字、文档符号、内置运行时                               | ✅   |
| 跳转定义 | 函数/extern/类/结构/局部变量/参数声明定位                   | ✅   |
| 符号导航 | textDocument/documentSymbol 文档符号                       | ✅   |
| 内嵌提示 | 类型推断、参数名提示                                       | 🚧 开发中 |

```bash
cmake -G Ninja -B build -S . && ninja -C build vxls

./build/tools/vxls/vxls                    # 启动 LSP（默认带 vx-tidy）
./build/tools/vxls/vxls --no-tidy          # 仅前端诊断，不跑 vx-tidy
./build/tools/vxls/vxls --stdio            # 显式声明 stdio（编辑器配置兼容）
./build/tools/vxls/vxls --only-tidy a.vx   # 仅运行 vx-tidy 后退出（不启 LSP）
./build/tools/vxls/vxls --only-tidy --format=json a.vx   # JSON 输出（CI 用）
./build/tools/vxls/vxls --help / --version # 帮助 / 版本
```

### 8. 静态分析（vx-tidy）

vx-tidy 位于 `tools/vx-tidy/`，引擎编译为**动态库 `libvxtidy.dylib`**
（CLI `vx-tidy` 与 vxls 共用），基于 AST 与作用域符号表的单文件检查，
规则带稳定编号（VXTIDY001~005 语义规则 + VXTIDY900 前端语法错误透传）：

```bash
cmake -G Ninja -B build -S . && ninja -C build vx-tidy

./build/tools/vx-tidy/vx-tidy --list-rules
./build/tools/vx-tidy/vx-tidy src.vx              # text 输出
./build/tools/vx-tidy/vx-tidy --format=json src.vx  # JSON 输出（CI 用）
# 退出码：0 = 无 error；1 = 有 error；2 = 用法错误
```

---

## 语言特性

> 以下为语言设计说明；**编译器当前实际支持的语法以 [docs/SYNTAX.md](docs/SYNTAX.md) 为准**。

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

**内建类型**: `int`（i32）、`float`（IEEE double）、`bool`、`string`、`void`、`var`（推断槽位）
**复合类型**: `struct`、`class`（`enum` / `union` 为软关键字，规划中）
**标准库类型**: `std::String`、`std::Vec<T>`（`std/` 源码中，随编译器进度接入）

### 内存安全模型（规划中）

> 所有权/借用检查器（OwnershipChecker）尚未实现；`mut` 当前与 `var` 等价，不做可变性检查。

#### 可变性

| 关键字   | 说明               | 示例                    |
| -------- | ------------------ | ----------------------- |
| `mut`  | 声明可变变量       | `mut x: int = 1`      |
| `&`    | 不可变引用（借用） | `r: pointer = &x`     |
| `&mut` | 可变引用（借用）   | `r: pointer = &mut x` |

#### 所有权规则

- 每个值有且仅有一个所有者
- 使用 `move` 显式转移所有权
- 标量类型 (`int` / `float` / `bool`) 默认 Copy 语义

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

规划中的编译期检测：use-after-move、double-free、对不可变变量赋值、活跃借用冲突、内存泄漏。

### 编译时宏系统（规划中）

参数化宏计划在编译期展开，零运行时开销（`macro` 目前为软关键字，宏尚未实现）。

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
  TypeIR         — 类型化中间表示 (代码、类型信息、链接表 linkage)
  Deps           — 导入库依赖 (@imp/@cppimp 的实现文件与 C++ 头文件，可选)
```

> 链接表条目记录每个跨模块符号：VX 侧名、参数/返回签名，以及 VX ABI 符号名
> (`_vx_<lib>_<name>`)、来源库名与 `from_cpp` 标志。

---

## 项目结构

```
.
├── src/                      # C++ 源码目录
│   ├── util/                 # Util 层：arena / str / fs / platform
│   ├── compiler/             # Compiler 层：diag / vxabi / lexer / parser / importer / codegen / vxobj
│   ├── backend/              # Backend 层：vxobj_reader / llvm_emitter / link_driver
│   └── driver/               # vxc002（前端 CLI）与 vlnk（后端 CLI）入口
├── include/                  # 公开头文件目录（vx/util、vx/compiler、vx/backend）
├── headers/                  # 导入库一套（@imp/@cppimp 测试覆盖全类型）
│   ├── math.* / text.*       # 根目录库：int / string+bool+void
│   ├── fmath.*               # 根目录库：float（double ABI）
│   ├── collections/vec.*     # 子目录库（@imp(/collections/vec)）
│   └── cpp/greet.{hpp,vxh,cpp} # @cppimp 样例（C++ 头文件 + 伴生声明 + 实现）
├── tests/                    # C++ 单元测试 + .vx 端到端样例
│   ├── *_test.cpp            # util/diag/lexer/parser/codegen/vxobj/e2e/backend/import 测试
│   └── test_*.vx             # e2e 样例（simple/void/return/math/prime）
├── tools/                    # 独立工具子项目（主构建自动检测 tools/*/CMakeLists.txt）
│   ├── vmake/                # 项目构建编排器（VMakeLists.toml → vxc/vlnk）
│   ├── vxls/                 # VX 语言服务器（LSP over stdio，集成 vx-tidy）
│   └── vx-tidy/              # 静态分析引擎（libvxtidy.dylib + vx-tidy CLI）
├── tree-sitter-vx/           # Tree-sitter 语法（编辑器高亮、语法导航）
├── cpp_lsp/                  # 早期独立 LSP 原型（已由 tools/vxls 取代，保留参考）
├── std/                      # VX 标准库源码（.vx 源文件）
├── compiler/                 # VX 编译器前端源码（.vx 源文件，自举参考）
├── docs/                     # 语言与格式文档（SYNTAX.md 等）
├── ci/                       # CI 配置
├── CMakeLists.txt            # CMake 构建配置（CMake + Ninja）
├── Makefile                  # 常用构建命令封装
├── CHANGELOG.md              # 变更日志
├── CONTRIBUTING.md           # 贡献指南
├── LICENSE                   # AGPL-3.0 协议
└── README.md                 # 项目说明（本文件）
```

---

## 依赖概览

### C++ 部分

**纯自研，零第三方库依赖。** 编译器/后端仅依赖 C++ 标准库；
`vlnk` 链接期调用**宿主 clang**（系统工具链，非项目依赖）。

目标架构支持由宿主 clang 决定（发射的是目标无关的 LLVM IR 文本）：

| 架构      | 状态                        |
| --------- | --------------------------- |
| x86_64    | ✅ 常用（`--target` 交叉编译） |
| aarch64   | ✅ 常用（macOS 开发主机）    |
| ARM32     | 🧪 取决于宿主 clang         |
| RISC-V 32 | 🧪 取决于宿主 clang         |
| RISC-V 64 | 🧪 取决于宿主 clang         |

### 架构说明

| 架构      | 三元组示例                        | 说明                  |
| --------- | --------------------------------- | --------------------- |
| x86_64    | `x86_64-unknown-linux-gnu`      | 64位 x86 处理器       |
| aarch64   | `aarch64-apple-macosx` / `aarch64-unknown-linux-gnu` | ARM 64位 (Apple M1+) |
| ARM32     | `armv7-unknown-linux-gnueabihf` | ARM 32位 (嵌入式/IoT) |
| RISC-V 32 | `riscv32-unknown-linux-gnu`     | RISC-V 32位           |
| RISC-V 64 | `riscv64-unknown-linux-gnu`     | RISC-V 64位           |

> `--target` 接受任意 clang 认识的三元组；`vxc002 -v` 会打印本次编译所用的 target。

---

## API 参考

### Util 层 API (C++)

```cpp
#include "vx/util/util.hpp"

int main() {
    // 线性分配器
    vx::util::Arena arena(64 * 1024);
    int* a = arena.create<int>(42);
    int* b = arena.create<int>(7);
    arena.reset(); // 批量回收

    // 字符串工具（split 按单字符分隔）
    std::string_view s = "hello,world";
    auto parts = vx::util::split(s, ','); // std::vector<std::string_view>

    // 文件系统工具（std::expected 统一错误）
    auto content = vx::util::read_file("input.vx"); // expected<std::string, std::string>
    auto ok = vx::util::write_file("output.txt", "result");

    return 0;
}
```

> 注：编译器与后端库（`libvx_compiler.a` / `libvx_backend.a`）目前面向内部工具
> （vxc002 / vlnk / vxls）使用，公开 API 将随版本演进稳定。

---

## 高级用法

> 本节为语言设计示例：编译时宏、所有权/借用均**规划中**，
> 当前编译器支持的语法见 [docs/SYNTAX.md](docs/SYNTAX.md)。

### 多文件项目

使用 VMake 的 `sources` 数组即可（见“使用指南”第 4 节）：
VMake 会为每个源文件调用 `vxc`，再由 `vlnk` 合并全部 `.vxobj` 后链接。
跨文件函数调用由合并后的模块统一解析。

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
    # 可变借用
    data.value = 42

func read(data: &Data) -> int:
    # 不可变借用
    return data.value

func main():
    mut data: Data = Data{ .value = 0 }

    # 可变借用
    process(&mut data)

    # 不可变借用
    val: int = read(&data)

    # 所有权转移
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
# 克隆后运行测试确保环境正常（15 组测试）
cmake --build build --target test

# 默认构建（MinSizeRel）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build --config MinSizeRel

# Debug 构建（含调试信息）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug

# 构建全部工具（含 vmake / vxls / vx-tidy，主构建自动纳入 tools/）
cmake -G Ninja -B build -S . && ninja -C build
# 产物：build/tools/vxls/vxls、build/tools/vx-tidy/vx-tidy、build/tools/vmake/vmake
```

### 运行特定测试

```bash
# 运行全部测试
cmake --build build --target test
```

---

## 变更日志

详细版本变更请查看 [CHANGELOG.md](CHANGELOG.md)。

---

## 许可

AGPL-3.0 — 详见 [LICENSE](LICENSE)

> 本项目为完全独立自研项目。编译器全部代码从零编写，没有复刻、修改、打包任何第三方编译器源码。
> 仅用于编译原理研究与开源协作，不会封装为线上 SaaS 服务对外运营。
