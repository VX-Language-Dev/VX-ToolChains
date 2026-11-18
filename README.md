# VX-ToolKit

> **从零自研的编程语言工具链** — C++23 实现，一条纯自研的编译链路把 `.vx` 源码变成原生机器码
>
> ![Language](https://img.shields.io/badge/language-C%2B%2B23-blue)
> ![Version](https://img.shields.io/badge/version-v1.21.1-blue)
> ![Tests](https://img.shields.io/badge/tests-18%20suites%20100%25%20passing-brightgreen)
> ![Deps](https://img.shields.io/badge/third--party%20deps-zero-orange)
> ![License](https://img.shields.io/badge/license-AGPL--3.0-green)

---

### 一秒上手

```vx
# hello.vx
func main():
    out("Hello, VX!")
```

```bash
cmake -S . -B build -G Ninja && cmake --build build --config MinSizeRel
./build/vxc hello.vx && ./build/vlnk hello.vxobj -o hello && ./hello
# → Hello, VX!
```

---

### 为什么选 VX-ToolKit

| 特性 | 说明 |
|------|------|
| **原生编译 · 无 VM** | `.vx` → TypeIR → VXOBJ v4 → LLVM IR → clang → Mach-O / ELF 机器码。没有解释层、没有字节码，直接交给宿主编译器生成原生产物 |
| **零第三方依赖** | 编译器与后端仅依赖 C++23 标准库。链接期才调用宿主 clang（系统工具链，不计入项目依赖） |
| **编译链路全自研** | 从 Lexer / Parser / Semantics / Codegen / VXOBJ 格式 / LLVM Emitter / Link Driver 到 LSP 服务器与构建系统，7 个子项目全栈自写 |
| **静态类型 + var 推断** | 显式标注或 `var` 推断；无隐式类型转换；方法支持按参数类型重载解析 |
| **跨平台中间格式** | VXOBJ v4 不携带平台特征，同一份 `.vxobj` 可在任意 clang 宿主上链接出对应平台的可执行文件 |
| **混合语言互操作** | `@imp(lib)` 导入 C 库，`@cppimp(header.hpp)` 一行接入 C++ 头文件——链接期自动生成 ABI shim，不需手写桥接代码 |
| **开发工具全家桶** | `vxc` 编译器 · `vlnk` 链接器 · `vmake` 增量构建（内容哈希签名 + 并行编译）· `vfmt` 格式化工具（保留注释 + AST 结构自检）（LSP/静态分析见独立仓库 `dry` / `drut`） |
| **增量构建** | `vmake` 基于内容哈希签名（非 mtime），未变化的目标跳过；修改编译参数仅重链不重编 |

---

### 加入我们

VX-ToolKit 是一个完全自研的开源编译器工具链，我们正在寻找对编译原理、
语言设计与工具链工程感兴趣的贡献者：

- 🧩 **语言前端**：Lexer / Parser / Semantics / 类型系统
- ⚙️ **后端与链接**：LLVM IR 发射、链接驱动、调试信息
- 🛠️ **工具链**：vmake 构建编排、vfmt 格式化、LSP/静态分析
- 📚 **标准库**：`std/` 与 `headers/` 的原生化与扩展

无论你是想学习编译器实现，还是希望把 VX 推向更完整的工程化工具链，
都欢迎通过 [Issues](https://gitee.com/vx-language-dev/vx-toolkit/issues) 与 Pull Request 参与。
详见下方 [贡献指南](#贡献指南)。

---

### 编译流水线

```
.vx 源码
  ↓ Lexer → Token 流
  ↓ Parser → AST
  ↓ @imp / @cppimp 导入表（VX ABI 符号 + 链接依赖）
  ↓ Semantics → 符号表 + 类型检查 + 重载解析（多错误聚合）
  ↓ Codegen → TypeIR（类型化栈机指令）
  ↓ VXOBJ v4 跨平台文件
  ↓ vlnk → LLVM IR → clang 链接
  ↓ Mach-O / ELF 原生可执行文件
```

> 规划中：所有权 / 借用检查、直接机器码发射、宏系统、泛型。`enum` 已支持（整数枚举）。见 [CHANGELOG](CHANGELOG.md)。

---

### 各模块状态

| 模块 | 职责 | 状态 | 版本 |
|------|------|------|------|
| `vxc` | 编译器前端：lex → parse → semantics → codegen → .vxobj | ✅ 可用 | 0.7.1 |
| `vlnk` | 原生链接器：.vxobj → llc 生成 .o → lld 链接（自动探测系统 C/C++ 编译器，无 llc/lld 时回退 clang 驱动） | ✅ 可用 | 0.2.3 |
| `vmake` | 项目构建编排：VMakeSet.vmake 驱动 vxc/vlnk/ar | ✅ 可用 | 0.4.0 |
| `vmake-lsp` | VMakeSet.vmake 语言服务器 | ✅ 可用 | 0.3.0 |
| `vfmt` | 源码格式化（保留注释 + AST 结构自检） | ✅ 可用 | 0.1.0 |
| `vx` | 元命令：`vx compile` / `vx run` / `vx fmt` / `vx write-settings` | ✅ 可用 | 1.21.1 |
| VPM 包管理器 | 多语言包管理 | 🚧 规划 | — |
| 反编译器 vxde | VXOBJ → VX 源码还原 | 🚧 规划 | — |

> `vxls`（LSP 服务器）与 `vx-tidy`（静态分析引擎）已拆分为独立仓库
> `dry` 与 `drut`，不再随本仓库构建；安装与使用见各自仓库 README。

---

## 目录

- [快速开始](#快速开始)
- [示例程序](#示例程序)
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

## 路线图（规划中 · 欢迎参与）

以下能力尚未实现，详见 [CHANGELOG](CHANGELOG.md)。我们欢迎你通过 Issue / PR 参与其中，共同把 VX-ToolKit 打造成更完整的工具链：

- **所有权 / 借用检查**：Rust 风格 `mut` / `&` / `&mut`，编译期 aliasing XOR mutation 检测
- **编译时宏系统**：参数化宏，零运行时开销
- **union / 泛型**：联合体与类型参数化扩展（`enum` 已支持整数枚举）
- **std 标准库接入**：`std::String`、`std::Vec<T>` 等下沉到编译器构建
- **直接机器码发射**：跳过 clang 的原生代码生成
- **VPM 包管理器**、**反编译器 vxde**

> Windows 原生（PE/MSVC）输出已移除；Windows 用户请使用 MSYS2 / Git Bash / Cygwin / WSL / WSL2。

---

## 快速开始

### 环境要求

| 组件        | 最低版本                |
| ----------- | ----------------------- |
| CMake       | 3.23+                   |
| Ninja       | 1.10+                   |
| C++ 编译器  | 支持 C++23 (Clang 16+ / GCC 13+) |
| clang       | vlnk 链接依赖（Xcode CLT / LLVM，任意近期版本） |
| 操作系统    | Linux / macOS（Windows 请使用 MSYS2 / Git Bash / Cygwin / WSL / WSL2） |

### Windows 平台开发说明

VX-ToolKit **不再提供 Windows 原生（PE/MSVC）支持**。Windows 用户请在以下 POSIX 环境中开发与构建：

- **MSYS2**（推荐，提供完整的类 Unix 工具链与包管理）
- **Git Bash**
- **Cygwin**
- **WSL / WSL2**（推荐，完整 Linux 环境）

在这些环境中，工具链按 Linux/Unix 路径构建与运行，产物为 POSIX 可执行文件。

### 构建工具选择建议

- **大多数情况下优先使用 VMake**（`vmake build -B <dir>`）：它会按正确顺序自动调用 `vxc` / `vlnk`，并处理依赖、增量构建与头文件搜索目录（`-I`）。
- **仅在需要手动干预底层编译**（如单独调试 `vxc`/`vlnk`、交叉编译 `--target`、链接原生库 `--lib`）时，才直接使用 `vxc` / `vlnk` CLI。

### 获取源码

```bash
git clone https://gitee.com/vx-language-dev/vx-toolkit.git
cd vx-toolkit
```

### 构建工具链

```bash
# 使用 CMake + Ninja 构建（推荐）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build --config MinSizeRel
```

构建产物位于 `build/` 目录：

| 产物                          | 说明                                    |
| ----------------------------- | --------------------------------------- |
| `vxc`                         | 编译器前端 CLI（CMake 目标 `vxc002`；`--version` 显示 0.7.1：lex → parse → imports → semantics → codegen → .vxobj，支持 `--check` / `--dump` / `-I` / `--target` / `-O<n>` / `-g`） |
| `vlnk`                        | 原生链接器 CLI（.vxobj → LLVM IR → clang 链接） |
| `tools/vmake/vmake`           | 项目构建编排器（VMakeSet.vmake）        |
| `tools/vmake-lsp/vmake-lsp`   | VMakeSet.vmake 语言服务器（LSP over stdio） |
| `tools/vfmt/vfmt`             | VX 源码格式化工具（`-w` 就地改写 / `--check` CI 检查） |
| `libvx_util.a` / `libvx_compiler.a` / `libvx_backend.a` | 静态库 |
| `libvxrt.a`                   | 预编译 VX 运行时（vlnk 链接期直接并入） |

> 另有 18 组测试可执行文件（`vx_*_tests`、`vmake*`、`vmake-lsp` 与 `vfmt` 相关），
> 由 CTest 统一调度。`vxls` / `vx-tidy` 拆分为独立仓库后，其测试不再计入本仓库。

### 安装到系统

**方式一：源码安装**（构建并安装到 `~/.local`）

```bash
zsh install.zsh
```

`install.zsh` 构建工具链后安装到 `~/.local/bin`：`vxc`、`vlnk`、
`vmake`、`vmake-lsp`、`vfmt`、`vx` 元命令；导入库 `headers/` 与二进制**同目录**（vxc
的默认查找根，可用 `-I` 追加搜索目录或 `VX_HEADERS_DIR`（优先）/ `VX_HEADERS` 覆盖库根）；预编译运行时
`libvxrt.a` 安装到 `~/.local/lib`（vlnk 优先直接链接
libvxrt.a）；shell 补全安装到
`~/.local/share` / `~/.zsh/completions`。请确保 `~/.local/bin` 在 PATH 中。

**方式二：发布包安装**（多平台 Zip，免构建）

```bash
./scripts/build-release.sh                 # 产出 dist/vx-toolkit-<平台>-<配置>.zip
# 解压后：
chmod +x install.sh && ./install.sh        # 默认安装到 ~/.local；PREFIX=/opt ./install.sh 可定制
```

发布包内含全部可执行文件、静态库、`headers/`、`std/`、`completions/` 与
`install.sh`；`install.sh` 将 `headers/` 装到 `$PREFIX/bin/headers`（与二进制同目录，
满足默认查找规则），`libvxrt.a` 装到 `$PREFIX/lib`（vlnk 经 `bin/../lib` 自动定位）。

> 注：以上两种脚本为日常安装的推荐方式；根 `CMakeLists.txt` 亦提供标准
> `install()` 规则（`cmake --install build --prefix <dir>`，安装 vxc / vlnk、
> 静态库、`headers/`、`std/` 与补全文件）。

### 运行测试

```bash
# 运行全部 C++ 单元/端到端测试（18 组，CTest 统一调度）
cmake --build build --target test
# 或等价：ctest --test-dir build --output-on-failure
```

### 示例程序

[`examples/`](examples/) 提供 7 个可直接编译运行的示例，覆盖内置运行时、类与构造器、
`@imp` 导入、`@cppimp` 接入 C++ 头文件以及线程。在仓库根目录执行：

```bash
bash examples/run.sh    # 编译、链接并运行全部示例
```

每个示例的说明与预期输出见 [examples/README.md](examples/README.md)。

### Hello World

创建 `hello.vx`：

```vx
func main():
    out("Hello, VX!")
```

编译并运行：

```bash
# 编译为 VXOBJ（默认输出 hello.vxobj）
./build/vxc hello.vx

# 链接为原生可执行文件
./build/vlnk hello.vxobj -o hello

# 运行
./hello
```

实际输出：

```text
OK hello.vx (1 funcs, 3 insts, 0 externs) → hello.vxobj
wrote hello
Hello, VX!
```

---

## 使用指南

### 1. 编译 VX 源码

```bash
# 编译单文件（默认输出 <源文件主名>.vxobj，即 hello.vx → hello.vxobj）
./build/vxc hello.vx

# 指定输出文件名
./build/vxc hello.vx -o output.vxobj

# 仅做检查，不写输出文件
./build/vxc hello.vx --check

# 指定 @imp/@cppimp 头文件搜索目录（可多次；优先于默认 headers/，亦支持 -I<dir>）
./build/vxc hello.vx -I ./mylibs -I /opt/vx/include

# 交叉编译目标三元组（默认自动检测 host triple）
./build/vxc hello.vx --target aarch64-unknown-linux-gnu

# 优化级别与调试信息（写入 VXOBJ，vlnk 链接时生效）
./build/vxc hello.vx -O2 -g

# 打印模块摘要（funcs/insts/externs/globals）
./build/vxc hello.vx --dump

# 查看编译过程详细信息
./build/vxc hello.vx -v
```

编译器流水线：

1. **词法分析** (Lexer) → Token 流
2. **语法分析** (Parser) → AST
3. **库导入解析** (@imp / @cppimp) → 导入表（VX ABI 符号签名 + 链接依赖）
4. **Semantics** → 符号表（Scope 栈 / 函数表 / 类布局表）+ 类型环境检查：无隐式转换、`var` 推断、方法重载解析，多错误聚合输出
5. **Codegen** → TypeIR 栈机指令 + 序列化 → VXOBJ v4

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

# 只生成原生目标文件（供外部构建系统 / ar 归档）
./build/vlnk hello.vxobj -c -o hello.o

# 优化 / 调试（-g 在 macOS 上同时生成 .dSYM）
./build/vlnk hello.vxobj -O2 -g -o hello

# 链接选项：剥离符号 / rpath / 自定义 sysroot
./build/vlnk hello.vxobj -o hello --strip --rpath /opt/lib --sysroot /path/to/sdk

# 链接原生库（.a / .dylib / .so，可多次）
./build/vlnk hello.vxobj -o hello --lib /opt/lib/libssl.a

# 额外 clang 参数：编译期 --cflag（作用于 .ll/.c/.cpp）、链接期 --ldflag
./build/vlnk hello.vxobj -o hello --cflag -Wall --ldflag -Wl,-rpath,/opt/lib
```

链接器自动完成：

- 解析并合并多个 VXOBJ → 发射 LLVM IR（.ll 文本）
- 并入预编译 VX 运行时 `libvxrt.a`（找不到时回退为临时 `vxrt.c` 源现编）
- 调用宿主 clang 编译链接（`--target` 非宿主时自动交叉编译）
- @imp/@cppimp 的实现文件（.c/.a/.dylib/.cpp）与 C++ shim 一并参与链接

> `--embed-vxobj` 反链接嵌入待实现。Windows 原生 PE 输出不再支持；Windows 用户请通过
> MSYS2 / Git Bash / Cygwin / WSL / WSL2 等 POSIX 环境构建（产物为 ELF/Mach-O 或 POSIX 可执行文件）。

### 2.1 系统能力（命令行参数 / 文件 I/O）

内置运行时提供一组**无需 `extern` 声明**的系统函数：

```vx
func main() -> int:
    # 命令行参数
    out_int(sys_argv_count())     # 含可执行文件名
    out(sys_argv_get(1))          # 第 1 个参数（越界返回空串）
    # 文件 I/O
    file_write("out.txt", "hello")     # 覆写写入，返回字节数
    var content = file_read("out.txt") # 读取全文（失败返回空串）
    if file_exists("out.txt"):
        out(content)
    return 0
```

| 符号 | 说明 |
| --- | --- |
| `sys_argv_count() -> int` | 命令行参数个数 |
| `sys_argv_get(i: int) -> string` | 取第 i 个参数 |
| `file_read(path: string) -> string` | 读取整个文件 |
| `file_write(path: string, content: string) -> int` | 覆写写入，返回字节数（失败 `-1`） |
| `file_exists(path: string) -> bool` | 文件是否存在 |

实现位于内置运行时 `vxrt`（链接期并入 `libvxrt.a`），完整列表见
[docs/SYNTAX.md](docs/SYNTAX.md)。执行 shell 命令的 `system` 由 `sys`
导入库提供（见 §3）：

```vx
@imp(sys)
func main() -> int:
    sys.system("echo hello")          # sys 库 → 符号 _vx_sys_system
    return 0
```

### 3. 库导入与 VX ABI (@imp / @cppimp)

通过 `@imp` 导入 VX 库，通过 `@cppimp` 引用 C++ 头文件（链接期自动生成 shim）。
导入符号以**库名前缀**限定访问。

```vx
@imp(math)                    # 根目录库：直接引用文件名
@imp(math) as m               # 别名导入：m.add(...) 等价 math.add(...)
@imp(/collections/vec)        # 子目录库：'/' + 子目录路径，库名取末段 → vec
@imp(./mylib)                 # 自定义库：相对源文件目录
@imp(proc)                    # 进程原语库：仅 .vxh 声明，链接期直连系统 libc
@cppimp(cpp/greet.hpp)        # C++ 头文件 → 链接期生成 C++ ABI → VX ABI 的 shim

func main():
    out_int(math.add(2, 3))   # math 库 → 符号 _vx_math_add
    out_int(m.mul(6, 7))      # 别名 m → 同 _vx_math_mul
    out_int(vec.sum_to(10))   # vec 库  → 符号 _vx_vec_sum_to
    out(greet.greet("VX"))    # greet 库 → shim 包装 C++ 符号
```

**模块导入（`import`，跨编译单元声明解析）**：`import a.b.c` 把 `a/b/c.vx` 的
**声明**（类/结构、枚举、函数签名）并入当前编译单元，函数体与实例布局代码由该模块
自己的 `.vxobj` 提供，链接期由 `vlnk` 合并——等价「模块即头文件」，保持既有
单文件编译 + 多输入链接的架构：

```vx
# pkg/mathx.vx
enum Level:
    Low
    High = 5
class Counter:
    n: int
    func new(start: int):
        self.n = start
    func bump() -> int:
        return self.n
func add(a: int, b: int) -> int:
    return a + b

# main.vx
import pkg.mathx

func main():
    out_int(mathx.add(1, 2))          # 模块函数 → 符号 add
    var c: Counter = mathx.Counter(10) # 构造：NEWZ + 调用 Counter.new
    out_int(c.n)                       # 导入类的字段布局已知
    out_int(Level.High)                # 枚举成员编译期内联
```

```bash
vxc main.vx -o main.vxobj && vxc pkg/mathx.vx -o mathx.vxobj
vlnk main.vxobj mathx.vxobj -o app
```

> 模块搜索根：`-I` 目录 → 源文件目录 → 其上级（最多 4 级）；`import a.b.c` 也接受
> `a/b/c/mod.vx`。未找到的模块静默跳过，解析失败记警告并跳过其声明。
> 尚不支持模块级常量、传递导入与可见性控制。

**进程原语库 `proc`（多进程并发）**：`headers/proc.vxh` 只有声明、**无 `.c` 实现文件**——
链接期 emitter 识别 `_vx_proc_*` 后直接发射对系统 libc 的调用，零 C/C++ 源码：

```vx
@imp(proc)
func main() -> int:
    var pid = proc.fork()          # 父返回子 PID，子返回 0
    if pid == 0:
        out("child\n")
        exit(0)
    proc.waitpid(pid, nil, 0)      # 阻塞回收；status 传 nil/0 = 不采集退出状态
    out_bool(proc.waitpid(proc.getpid(), 0, 1) == -1)  # options=1（WNOHANG）非阻塞轮询
    out("\n")
    return 0
```

**线程与任务池（`proc` 库同一套零 C/C++ 实现）**：任务以**函数值 `&fn`** 传入，
后端为每个被取址函数生成通用适配器（`i64(i64,i64,i64,i64)`），符号在链接期解析，
因此任务函数可跨文件、可存入变量。任务可带 0~4 个实参，返回值类型不限
（由 `join_int` / `join_float` / `join_str` / `join_ptr` 按类型取回）。
实参按 **64 位透传**，`int` 走 `sext`、`ptr` / `string` 走 `ptrtoint`，
因此指针可安全跨线程传递（不再被截断）：

```vx
@imp(proc)
func worker(x: int, y: int) -> int:
    proc.sleep(1)
    return x * y

func main() -> int:
    var h = proc.spawn2(&worker, 6, 7)
    out_int(proc.join_int(h))    # 42（阻塞回收并按 int 取回）
    out("\n")

    proc.pool_start(4)               # 4 个工作线程（夹到 1..64）
    proc.pool_submit2(&worker, 1, 2)
    proc.pool_submit2(&worker, 3, 4)
    proc.pool_wait()                 # 阻塞到全部任务完成
    proc.pool_stop()                 # 关闭并回收工作线程
    return 0
```

> 任务池为固定工作线程 + 1024 槽有界队列；`pool_submitN` 在队列满或未启动时返回 0。
> 线程句柄上限 1024，每个句柄只可 join 一次。

**同步原语（`proc` 库，直连 pthread）**：互斥锁与条件变量以**裸内存句柄**暴露，
内存由调用方提供（`malloc(64)` 足以跨平台容纳 macOS 64 字节 / Linux 40 字节的
`pthread_mutex_t`，`malloc` 返回值满足对齐要求），`attr` 传 `nil` 即默认属性：

```vx
@imp(proc)
func main() -> int:
    var mtx = malloc(64)
    var cv = malloc(64)
    proc.mutex_init(mtx, nil)          # 0 = 成功
    proc.cond_init(cv, nil)

    proc.mutex_lock(mtx)               # 阻塞加锁
    out_bool(proc.mutex_trylock(mtx) != 0)   # → true：已被本线程持有（EBUSY）
    proc.cond_wait(cv, mtx)            # 原子解锁并阻塞；须由其它线程 cond_signal 唤醒
    proc.mutex_unlock(mtx)

    proc.cond_destroy(cv)
    proc.mutex_destroy(mtx)
    free(cv)
    free(mtx)

    proc.usleep(1000)                  # 微秒级睡眠（> 1 秒请用 proc.sleep 秒级）
    return 0
```

| 函数 | 说明 |
| --- | --- |
| `mutex_init(mtx, attr)` | 初始化（`0` 成功） |
| `mutex_lock(mtx)` | 阻塞加锁（`0` 成功） |
| `mutex_trylock(mtx)` | 非阻塞尝试：`0` 拿到锁，非 `0` 已被占用（EBUSY，平台相关值） |
| `mutex_unlock(mtx)` / `mutex_destroy(mtx)` | 解锁 / 销毁 |
| `cond_init(cv, attr)` | 初始化条件变量 |
| `cond_wait(cv, mtx)` | 原子解锁 `mtx` 并阻塞；被唤醒后重新加锁 |
| `cond_signal(cv)` / `cond_broadcast(cv)` | 唤醒一个 / 全部等待者（无等待者返回 `0`） |
| `cond_destroy(cv)` | 销毁（须无等待者） |
| `usleep(us)` | 微秒级睡眠（`0` 睡满 / `-1` 失败） |

> POSIX 信号量与屏障不提供：macOS 的无名 `sem_init` 不可用、且无 `pthread_barrier_*`。
> VX 语言层的 **Semaphore / Barrier / Channel**（mutex + cond 组合实现）已落地在
> [`std/sys/sync.vx`](std/sys/sync.vx)，连同 `Mutex` / `Condvar` 一起可被当前工具链
> 编译执行（回归覆盖：`tests/import_e2e_test.cpp` 的 `synclib` 用例）：
>
> ```vx
> # 构造两步：new X() 只分配字段内存，init(...) 完成初始化
> var m = new Mutex()
> m.init()
> m.lock()
> # ... 临界区 ...
> m.unlock()
>
> var ch = new Channel()
> ch.init(8)          # 容量 8 的有界通道（int 值）
> ch.send(42)
> out_int(ch.recv())  # → 42
> ch.close()
>
> m.dispose()
> ch.dispose()
> ```
>
> 当前以同编译单元方式使用（拼接/复制源码）；跨编译单元的类引用待模块系统落地。

**多线程共享状态（指针实参 + 原子操作）**：任务实参按 64 位透传，因此可直接把
共享缓冲区指针传给工作线程；配合 `mutex_*` 或原子操作即可安全并发修改同一内存：

```vx
@imp(proc)
@imp(ptr)
func bump(shared: ptr) -> int:
    var i = 0
    var c = ptr.add(shared, 64)     # 前 64 字节为 mutex，偏移 64 为计数器
    while i < 500:
        proc.mutex_lock(shared)
        *c = *c + 1
        proc.mutex_unlock(shared)
        i = i + 1
    return 0

func main() -> int:
    var shared = malloc(72)
    proc.mutex_init(shared, nil)
    var c = ptr.add(shared, 64)
    *c = 0
    var h1 = proc.spawn1(&bump, shared)   # 指针实参不会被截断
    var h2 = proc.spawn1(&bump, shared)
    proc.join_int(h1)
    proc.join_int(h2)
    out_int(*c)                           # → 1000（无丢失更新）
    out("\n")
    proc.mutex_destroy(shared)
    free(shared)

    # 无锁版本：原子计数器（32 位字、4 字节对齐、seq_cst）
    var counter = malloc(4)
    proc.atomic_store(counter, 0)
    proc.atomic_fetch_add(counter, 1)     # 返回旧值
    out_int(proc.atomic_load(counter))    # → 1
    out("\n")
    free(counter)
    return 0
```

| 原子操作 | 说明 |
| --- | --- |
| `atomic_load(addr)` | 原子读，返回当前值 |
| `atomic_store(addr, val)` | 原子写（无返回值） |
| `atomic_fetch_add(addr, delta)` | 原子加，返回加之前的旧值 |
| `atomic_fetch_sub(addr, delta)` | 原子减，返回减之前的旧值 |
| `atomic_cas(addr, expected, desired)` | 比较交换，返回旧值（`== expected` 表示成功） |
| `atomic_fence()` | 全局内存栅栏（无返回值） |

> 原子操作由后端直接发射 LLVM `load atomic` / `atomicrmw` / `cmpxchg` / `fence`
> 指令，零外部符号；`addr` 需为 `malloc` 返回值或经 `ptr.add` 得到的 4 字节对齐地址。

**指针、管道与子进程输出捕获**：语言层新增 `*p` 读写（32 位字）与 `&fn` 取函数值；
`ptr` 库提供字节偏移与整数互转；`proc` 提供 `pipe` / `read_str` / `write_str` / `capture`：

```vx
@imp(proc)
@imp(ptr)
func main() -> int:
    var fds = proc.pipe()                 # 返回 ptr：两个 int 的 fd 对
    var rfd = *fds                        # 读端（*p 解引用）
    var wfd = *(ptr.add(fds, 4))          # 写端（字节偏移 + 解引用）
    proc.write_str(wfd, "hello")
    proc.close(wfd)
    out(proc.read_str(rfd, 64))           # → hello
    proc.close(rfd)

    out(proc.capture("echo hi"))          # 捕获 shell 命令 stdout → hi
    return 0
```

**库查找规则**：`-I <dir>` 指定的搜索目录（可多次，按序）优先；随后是库根，
按以下顺序解析、先命中先用（与 `vmake` 的库根优先级一致）：

1. 环境变量 `VX_HEADERS_DIR`（优先）/ `VX_HEADERS`——显式覆盖；
2. 当前工作目录（项目根）的 `headers/`——`vxc` 单独调用与 `vmake`
   构建因此解析到**同一个**库根；
3. vxc 可执行文件同级的 `headers/`，未命中则向上逐级查找（最多 4 级，
   覆盖 `build/vxc` → `build` → 仓库根的构建树布局）。

`@cppimp` 的 C/C++ 头文件在未命中上述目录时，会继续
探测系统 C/C++ 编译器（`c++`/`clang++`/`g++` 等）的头文件搜索目录；此时
VX 签名的 `.vxh` 伴生文件可在 `-I` / 库根中按同主名提供。

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
`text`（字符串查找/截取/trim/数值转换/行切分）、`collections/vec`（子目录库）、
`cpp/greet`（@cppimp）、`sys`（执行 shell 命令）、`fs`（目录创建/列举、文件信息、路径运算）。

**字符串与文件系统能力**（`text` / `fs`，均为 `.vxh` + `.c` 实现）：

```vx
@imp(text)
@imp(fs)

func main():
    out_int(text.find("hello world", "world"))   # 6（未找到 -1）
    out(text.substr("abcdef", 2, 3))             # cde
    out(text.trim("  hi  "))                     # hi
    out_int(text.parse_int("-42"))               # -42
    out(text.format_int(123))                    # 123
    var p: string = fs.path_join("/tmp", "demo")
    fs.mkdir_p(p)                                # 递归创建目录，成功返回 0
    file_write(fs.path_join(p, "a.txt"), "abc")
    out(fs.list_dir(p))                          # a.txt（每行一个条目）
```

> `text` / `fs` 中返回 `string` 的函数均返回 **malloc 新串**，调用方用 `free` 释放；
> 入参为 `nil` 时按空串处理。


### 4. 项目构建（VMake，VMakeSet.vmake）

vmake 位于 `tools/vmake/`。它读取项目根目录的 `VMakeSet.vmake`
（Python 风格、函数调用式配置），**自身不编译任何代码**，
而是按正确顺序在后台调用工具链：

```
每个源文件 → vxc <src> -o <obj>.vxobj
全部 .vxobj → vlnk <obj...> -o <产物> [--shared]
staticlib → ar rcs <产物> <obj...>
```

支持三类目标：可执行文件（`exe(...)`）、动态链接库（`dylib(...)`）与
静态库（`staticlib(...)`）。staticlib 先于 dylib，dylib 先于可执行文件构建；
同一目标内的多个源文件由 `vlnk` 合并链接。

```python
# VMakeSet.vmake
exe("hello", sources=["src/main.vx", "src/util.vx"])

dylib("mylib", sources=["src/lib.vx"], output="libmylib.dylib")  # output 可选，相对 -B 构建目录

staticlib("util", sources=["src/util.vx"])  # ar 归档

exe("app", sources=["src/app.vx"], deps=["mylib", "util"])  # 链接期并入库产物

exe("sys", sources=["src/sys.vx"],          # 外部库与链接期 clang 参数
    libs=["ssl", "/opt/lib/libfoo.a"], lib_dirs=["/usr/local/lib"],
    cflags=["-Wall"], defines=["NDEBUG"], ldflags=["-Wl,-rpath,/opt/lib"],
    tests=["tests/app_test.vx"])            # → 构建 app-test，由 vmake test 运行
```

构建目录必须显式用 `-B` 指定（不存在会自动创建）：

```bash
cmake -G Ninja -B build -S . && ninja -C build vmake

./build/tools/vmake/vmake build -B build-out
#    [ 1/3] vxc src/main.vx
#    [ 2/3] vxc src/util.vx
#    [ 3/3] vlnk hello
#    vmake-builder: build

./build/tools/vmake/vmake build -B build-out hello   # 只构建指定目标
./build/tools/vmake/vmake build -B build-out -I libs # 头文件搜索目录透传给每个 vxc 调用
./build/tools/vmake/vmake build -B build-out --release  # release profile（链接期 -O2）
./build/tools/vmake/vmake build -B build-out -v      # 打印实际 vxc/vlnk 命令
./build/tools/vmake/vmake build -B build-out --color=never

./build/tools/vmake/vmake run -B build-out           # 构建并运行唯一 exe（-- 转发参数）
./build/tools/vmake/vmake test -B build-out          # 构建并运行 tests=[...] 派生的测试
./build/tools/vmake/vmake clean -B build-out         # 删除构建目录
```

- **增量构建**：基于内容哈希签名（非 mtime），未变化的目标跳过；
  修改 `cflags`/`defines`/`ldflags` 或切换 `--release`/`--debug` 只重链不重编。
- **tools 子命令**：`vx run` / `vx test` 转发到 `vmake run` / `vmake test`。
- **工具链定位**：`VX_VXC` / `VX_VLNK` / `VX_AR` 可显式指定；否则在 vmake 可执行文件附近与 `PATH` 中查找。
- **导入库**：未设置 `VX_HEADERS_DIR`（优先）/ `VX_HEADERS` 时，自动尝试项目 `headers/` 与编译器旁的 `headers/`；
  `-I <dir>`（可多次）把头文件搜索目录透传给每个 `vxc` 调用并参与增量签名。
- 中间产物位于 `<构建目录>/obj/<目标>/`，最终产物默认：exe → `<name>`，
  dylib → `lib<name>.dylib`（macOS）/ `lib<name>.so`（Linux），
  staticlib → `lib<name>.a`，测试 → `<name>-test`。
- 完整语法参考见 `docs/VMAKE_SYNTAX.md`。

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

### 7. LSP 与静态分析（已拆分为独立仓库）

`vxls`（LSP 服务器）与 `vx-tidy`（静态分析引擎）已拆分为独立仓库
`dry` 与 `drut`，不再随本仓库构建：

- `dry`：VX 语言服务器（原 `vxls`），LSP over stdio，功能不变；
  命令行形态的静态检查由 `drut` 提供。
- `drut`：静态分析引擎（原 `vx-tidy`），双模式——`libdrut` 动态库 + `drut` 可执行文件。

安装与使用见各自仓库 README。本仓库仅保留编译器（`vxc`）、链接器（`vlnk`）、
构建器（`vmake`）与格式化器（`vfmt`）。

### 9. VMake 配置语言服务器（vmake-lsp）

vmake-lsp 位于 `tools/vmake-lsp/`，针对 `VMakeSet.vmake` 的 LSP 服务器（stdio 传输，
纯标准 C++23、零外部依赖），诊断规则与 `tools/vmake/src/config.cpp` 逐条对齐：

| 功能       | 说明                                                   | 状态 |
| ---------- | ------------------------------------------------------ | ---- |
| 诊断       | 未知/缺失/重复字段、目标名、源文件后缀、output 路径约束；推送 + 拉取（`textDocument/diagnostic`） | ✅   |
| 悬停信息   | 目标类型、目标名、`sources` / `output` 字段与源文件      | ✅   |
| 跳转定义   | 目标名/字段定位；源文件跳到清单目录下对应的 `.vx`        | ✅   |
| 代码补全   | 语句位（`exe` / `dylib`）、字段名、字段值片段            | ✅   |
| 格式化     | 目标声明规范化为 `exe("name", sources=[...])` 单行      | ✅   |
| 大纲 / 折叠 | documentSymbol 大纲、foldingRange 折叠                 | ✅   |
| 跨文件语义 | 引用查找、重命名、工作区符号                            | 🚧 开发中 |

```bash
cmake -G Ninja -B build -S . && ninja -C build vmake-lsp

./build/tools/vmake-lsp/vmake-lsp --stdio     # 启动 LSP（编辑器配置用）
./build/tools/vmake-lsp/vmake-lsp --help      # 帮助 / --version 版本
```

编辑器接入方式与 `tree-sitter-vmake` 高亮见 [docs/VMAKE_SYNTAX.md](docs/VMAKE_SYNTAX.md) 第 12 节。

### 10. 编辑器语言注册（`vx write-settings`）

`vx write-settings <editor>` 把 VX 语言（`.vx`）注册进编辑器的设置文件（文件类型 + 注释符 `#`）：

| 编辑器 | 命令参数 | 写入的设置文件 |
| ------ | -------- | -------------- |
| Neovim | `nvim` / `neovim` | `~/.config/nvim/init.lua`（`vim.filetype.add` + `commentstring`） |
| Vim    | `vim` | `~/.vimrc`（`BufRead,BufNewFile *.vx` → `setfiletype vx`） |
| Helix  | `helix` / `hx` | `~/.config/helix/languages.toml`（`[[language]]`：`file-types` / `comment-token`） |
| Emacs  | `emacs` | `~/.emacs.d/init.el`（`define-derived-mode vx-mode` + `auto-mode-alist`） |

```bash
./build/tools/vx/vx write-settings nvim          # 写入 Neovim 设置
./build/tools/vx/vx write-settings helix --dry-run  # 只打印将写入的内容，不改文件
```

- **幂等**：配置块以 `>>> vx write-settings >>>` / `<<< vx write-settings <<<` 标记，
  重复执行就地更新该块，不会重复追加；既有配置内容保持不变，缺失目录自动创建。
- **退出码**：0 = 成功；1 = 写入失败；2 = 用法错误（未知编辑器 / 多余参数 / 缺少 `HOME`）。
- 语法高亮需另装 tree-sitter-vx 语法（独立仓库，Neovim 经 nvim-treesitter、
  Helix 经 `grammars` 段配置）；语义能力（补全/悬停/跳转）由 `dry --stdio` 提供（独立仓库）。

### 11. 源码格式化（vfmt）

vfmt 位于 `tools/vfmt/`，把 VX 源码规范化为统一风格：缩进、空格、标点排布与
空行密度，同时**保留全部注释**。

```bash
cmake -G Ninja -B build -S . && ninja -C build vfmt

./build/tools/vfmt/vfmt src.vx                  # 输出到标准输出
./build/tools/vfmt/vfmt src.vx -w               # 就地改写（内容有变化才写盘）
./build/tools/vfmt/vfmt src.vx --check          # 只检查（CI）：需格式化则退出码 1
./build/tools/vfmt/vfmt src.vx --indent 2       # 缩进宽度改为 2 空格
cat src.vx | ./build/tools/vfmt/vfmt -          # 从标准输入读取
./build/tools/vx/vx fmt src.vx -w               # 经元命令调用
```

| 选项 | 说明 |
| ---- | ---- |
| `-w`, `--write` | 就地改写文件（仅在内容确有变化时写盘） |
| `-c`, `--check` | 只检查是否为规范格式；任一文件需格式化则退出码 1（CI 用） |
| `--indent <n>` | 缩进宽度（空格数），默认 4 |
| `--line-length <n>` | 行宽上限，默认 88；超出的行**仅报告不折行**（在 stderr 提示） |
| `--no-verify` | 跳过格式化前后的 AST 结构等价性自检 |

**规范化规则**：

- 缩进统一为空格（制表符按 4 列折算后转空格），层级由块结构决定
- 二元运算符两侧一个空格；一元运算符与操作数紧贴（`not a` / `-a` / `!a`）
- 逗号后一个空格、逗号前无空格；类型注解冒号前无空格、后一个空格
- 函数调用与下标紧贴（`f(a)` / `a[0]`）；控制流关键字后保留空格（`if (x):`）
- 同一物理行的多条语句（`;` 分隔）拆分为独立行
- 空行：块内最多 1 个，顶层最多 2 个；文件首尾空行裁剪
- 多行列表 / 字典字面量**保留原有换行结构**（与 gofmt 一致），只规范化每行缩进
- 独立注释行与行尾注释均原样保留

**安全保证**：

- 格式化基于词法 token 流而非 AST 重建，因此 `var` / `mut` / `const` 关键字、
  `import ... as` 别名、字符串转义形式等信息不会被改写
- 格式化结果会重新词法/语法解析，并与原源码比对 **AST 结构指纹**；
  结构不一致即判定失败、不产出结果（可用 `--no-verify` 关闭）
- 源码存在词法 / 语法错误时拒绝格式化（诊断信息与 `vxc` 一致），退出码 2

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

**内建类型**: `int`（i32）、`float`（IEEE double）、`bool`、`string`、`void`、`var`（64 位推断槽位，可承载 int/float/string/对象指针）
**复合类型**: `struct`、`class`、`enum`（成员底层为 `int`；`union` 规划中）
**标准库类型**: `std::String`、`std::Vec<T>`（`std/` 源码中，随编译器进度接入）

### 对象与构造器

- 类/结构在运行时以实例指针传递；类型标注写类名即可（`-> Box`、`p: Box`），IR 中规范化为 `ptr`。
- `new C(args)` = 分配实例 + 调用 `C.new(self, args)`：
  - 构造器返回非 void 时，以返回值作为表达式结果（工厂模式，可返回新实例）；
  - 构造器返回 void 时，就地初始化并保留 `NEWZ` 分配的实例。
- 无实参的 `new C()` 始终是裸分配（构造器内部用它分配实例，避免自递归）。

### 显式类型转换

VX 不做隐式数值转换；需要跨类型时使用内建转换函数：

```vx
int(3.9)       # → 3（浮点截断）
float(2)       # → 2.0
bool(0)        # → false
str(42)        # → "42"
str(true)      # → "true"
```

- `int` / `float` / `bool` 支持数值与 `ptr` 之间的按位转换。
- `str` 对整数/浮点/布尔做格式化，字符串原样返回；返回的新串由运行时分配。
- `str(var)` 目前按 32 位整数格式化（`var` 无运行时类型标签）。

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
  Lines          — 源文件名 + 每个函数的指令行号表（可选；供 DWARF / 诊断）
```

> 链接表条目记录每个跨模块符号：VX 侧名、参数/返回签名，以及 VX ABI 符号名
> (`_vx_<lib>_<name>`)、来源库名与 `from_cpp` 标志。

---

## 项目结构

```
.
├── src/                      # C++ 源码目录
│   ├── util/                 # Util 层：arena / str / fs / platform
│   ├── compiler/             # Compiler 层：diag / vxabi / lexer / parser / importer / semantics / codegen / vxobj
│   │                         #   parser_*.hpp / semantics_*.hpp 为内部实现头（仅供同名入口 .cpp 包含）
│   ├── backend/              # Backend 层：vxobj_reader / llvm_emitter / link_driver
│   │                         #   emitter_*.hpp 为内部实现头（仅供 llvm_emitter.cpp 包含）
│   ├── runtime/              # vxrt.c：预编译为 libvxrt.a（vlnk 链接期并入）
│   └── driver/               # vxc（前端 CLI）与 vlnk（后端 CLI）入口
├── include/                  # 公开头文件目录（vx/util、vx/compiler、vx/backend）
├── headers/                  # 导入库一套（@imp/@cppimp 测试覆盖全类型）
│   ├── math.* / text.*       # 根目录库：int / 字符串处理（查找/截取/转换）
│   ├── fmath.*               # 根目录库：float（double ABI）
│   ├── fs.*                  # 目录/文件与路径运算库
│   ├── collections/vec.*     # 子目录库（@imp(/collections/vec)）
│   ├── proc.vxh              # 进程/线程/任务池/管道库（仅 .vxh，直连 libc + 后端 IR，无 .c）
│   ├── ptr.vxh               # 指针运算库（add / to_int / from_int，仅 .vxh）
│   └── cpp/greet.{hpp,vxh,cpp} # @cppimp 样例（C++ 头文件 + 伴生声明 + 实现）
├── tests/                    # C++ 单元测试 + .vx 端到端样例
│   ├── *_test.cpp            # util/diag/lexer/parser/codegen/semantics/vxobj/e2e/backend/import 测试
│   └── test_*.vx             # e2e 样例（simple/void/return/math/prime）
├── examples/                 # 可直接编译运行的示例（bash examples/run.sh）
├── tools/                    # 独立工具子项目（主构建自动检测 tools/*/CMakeLists.txt）
│   ├── json/                 # 极简 JSON 编解码静态库（vmake-lsp 共享，vx::json）
│   ├── lsp/                  # LSP 传输层共享静态库（Content-Length 帧协议，vmake-lsp 共用）
│   ├── vmake-shared/         # .vmake 共享词法 + 校验规则（vmake / vmake-lsp 共用同一套底层）
│   ├── vmake/                # 项目构建编排器（VMakeSet.vmake → vxc/vlnk）
│   ├── vmake-lsp/            # VMakeSet.vmake 语言服务器（诊断/补全/悬停/跳转/格式化）
│   ├── vfmt/                 # VX 源码格式化工具（保留注释，AST 结构自检）
├── scripts/toolchains/       # CMake 交叉编译工具链预设（toolchain-*.cmake）
├── std/                      # VX 标准库（设计参考/前瞻规范，尚未接入编译器构建；可用能力见 headers/）
├── docs/                     # 语言与格式文档（SYNTAX.md / VMAKE_SYNTAX.md / CODE_LAYOUT.md）
├── ci/                       # CI 配置
├── CMakeLists.txt            # CMake 构建配置（CMake + Ninja）
├── install.zsh               # 安装脚本（vxc/vlnk/vx/vmake/vmake-lsp/vfmt + libvxrt.a + headers → ~/.local）
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

> `--target` 接受任意 clang 认识的三元组；`vxc -v` 会打印本次编译所用的 target。

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
> （vxc / vlnk）使用，公开 API 将随版本演进稳定。

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

欢迎参与 VX-ToolKit 的开发！我们正在积极招募贡献者，无论经验水平如何，
都可以从 [CONTRIBUTING.md](CONTRIBUTING.md) 了解贡献流程、代码规范和开发约定。

**新手友好入口**：
- 查看 `good first issue` 标签的任务
- `std/` 标准库的原生化改造（见 `std/README.md`）
- 文档、测试用例与示例程序的补充

我们重视每一个 PR，维护者会在 48 小时内响应。

参与前请阅读 [行为准则](CODE_OF_CONDUCT.md)；安全相关问题请按 [安全策略](SECURITY.md) 私下报告，
不要提交公开 Issue。

### 快速参与

1. Fork 本仓库
2. 创建特性分支 (`git checkout -b feature/awesome-feature`)
3. 提交更改 (`git commit -m 'Add some awesome feature'`)
4. 推送到分支 (`git push origin feature/awesome-feature`)
5. 提交 Pull Request

### 开发环境

```bash
# 克隆后运行测试确保环境正常（18 组测试）
cmake --build build --target test

# 默认构建（MinSizeRel）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build --config MinSizeRel

# Debug 构建（含调试信息）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug

# 构建全部工具（含 vmake / vmake-lsp / vfmt，主构建自动纳入 tools/）
cmake -G Ninja -B build -S . && ninja -C build
# 产物：build/tools/vmake/vmake、build/tools/vmake-lsp/vmake-lsp、build/tools/vfmt/vfmt
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
