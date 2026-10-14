# VX-ToolKit

![Language](https://img.shields.io/badge/language-C++-green)
![Version](https://img.shields.io/badge/version-v1.6.0-blue)
![License](https://img.shields.io/badge/license-AGPL--3.0-green)
![Platform](<https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey>)

**VX 编程语言的完整工具链** — 编译器、原生链接器、包管理器，正在迁移到 C++ 实现。

> 语言版本: v1.7.0
> 协议: [AGPL-3.0](LICENSE)
> 当前实现: **C++ 重写中**（Util 层已完成，编译器/链接器等模块后续补齐）

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
| 编译器 (vxc)          | C++      | 源码分析、TypeIR 生成、VXOBJ 输出 | 🚧 开发中 |
| 原生链接器 (vlnk)     | C++      | TypeIR 代码生成 + 可执行文件生成  | 🚧 开发中 |
| VPM 包管理器 (vpm)    | C++      | 多语言包管理                      | 🚧 开发中 |
| 反编译器 (vxde)       | C++      | VXOBJ → VX 源码还原              | 🚧 开发中 |
| 格式化工具 (vfmt)     | C++      | VX 源码格式化                     | 🚧 开发中 |
| 项目构建器 / 增量缓存 | C++      | vxsetting.toml 驱动的构建系统     | 🚧 开发中 |
| LSP 语言服务器        | C++      | 代码补全、悬停、跳转定义等        | 🚧 开发中 |
| Util 基础库           | C++      | 内存分配器、字符串、文件系统      | ✅ 稳定   |

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

| 组件        | 最低版本                |
| ----------- | ----------------------- |
| CMake       | 3.23+                   |
| Ninja       | 1.10+                   |
| C++ 编译器  | 支持 C++23 (Clang 16+, GCC 13+, MSVC 19.35+)
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

# 使用 Makefile 构建（推荐）
make build

# 或直接使用 CMake
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build --config MinSizeRel
```

构建产物位于 `build/` 目录：

| 二进制文件         | 说明            |
| ------------------ | --------------- |
| `vx_util_tests`  | Util 层单元测试 |

> 注：C++ 版本目前仅实现 Util 层（内存分配器、字符串、文件系统），编译器、链接器、包管理器等工具正在开发中。

### 安装到系统

```bash
# 使用 Makefile 安装到 ~/.local/bin
make install

# 确保 ~/.local/bin 在 PATH 中
export PATH="$HOME/.local/bin:$PATH"
```

> 注：当前仅安装 `vx_util_tests` 测试可执行文件。完整工具链安装待 C++ 实现完成后提供。

### 运行测试

```bash
# 运行全部 C++ 单元测试
make test

# 或直接使用 ctest
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

LSP 正在 C++ 中开发，位于 `cpp_lsp/` 目录：

| 功能     | 说明                                     | 状态    |
| -------- | ---------------------------------------- | ------- |
| 代码补全 | 关键字、函数名、变量名、成员访问、import | 🚧 开发中 |
| 悬停信息 | 类型签名、字段/方法文档、内置函数说明    | 🚧 开发中 |
| 符号导航 | 工作区符号、文档符号                     | 🚧 开发中 |
| 诊断     | 编译错误实时提示，自动发布到编辑器       | 🚧 开发中 |
| 跳转定义 | 变量/函数/参数/导入别名定义跳转          | 🚧 开发中 |
| 内嵌提示 | 类型推断、参数名提示                     | 🚧 开发中 |

> **使用方式**: 编译器前端完成后将提供 `vxlsp` 可执行文件，支持标准 Language Server Protocol。

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
├── src/                      # C++ 源码目录
│   └── util/                 # Util 层实现
│       ├── arena.cpp         # 线性分配器 (bump allocator)
│       ├── fs.cpp            # 文件系统工具
│       └── str.cpp           # 字符串工具
├── include/                  # 公开头文件目录
│   └── vx/util/              # Util 层公开 API
│       ├── arena.hpp
│       ├── fs.hpp
│       ├── str.hpp
│       └── util.hpp
├── tests/                    # 测试与示例
│   ├── util_test.cpp         # Util 层单元测试
│   ├── run_tests.sh          # VX 测试运行脚本
│   ├── batch/                # 批量测试用例
│   ├── std_lib_test.vx       # 标准库测试
│   ├── test_math.vx          # 数学测试
│   ├── test_prime.vx         # 素数测试
│   ├── test_simple.vx        # 简单测试
│   ├── test_void.vx          # void 返回测试
│   ├── test_return.vx        # 返回值测试
│   ├── test_import.vx        # 导入测试
│   └── runtime.c             # 运行时支持
├── std/                      # VX 标准库源码（.vx 源文件）
├── compiler/                 # VX 编译器前端源码（.vx 源文件，自举目标）
├── cpp_lsp/                  # C++ LSP 语言服务器实现
├── ci/                       # CI 配置
├── CMakeLists.txt            # CMake 构建配置
├── Makefile                  # Make 构建脚本
├── CHANGELOG.md              # 变更日志
├── CONTRIBUTING.md           # 贡献指南
├── INSTALL_SCRIPTS.md        # 安装脚本说明
├── LICENSE                   # AGPL-3.0 协议
└── README.md                 # 项目说明（本文件）
```

---

## 依赖概览

### C++ 部分

**纯自研，零外部运行时依赖。** Util 层仅依赖 C++ 标准库。

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

### Util 层 API (C++)

```cpp
#include "vx/util/util.hpp"

int main() {
    // 线性分配器
    vx::util::Arena arena(64 * 1024);
    int* a = arena.create<int>(42);
    int* b = arena.create<int>(7);
    arena.reset(); // 批量回收

    // 字符串工具
    vx::util::String s = "hello";
    auto parts = vx::util::split(s, ",");

    // 文件系统工具
    auto content = vx::util::read_file("input.vx");
    vx::util::write_file("output.txt", "result");

    return 0;
}
```

> 注：C++ 版本目前仅实现 Util 层。编译器、链接器、包管理器等高层 API 正在开发中，将在后续版本中提供。

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
