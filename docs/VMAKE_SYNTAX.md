# VMake 构建配置语法参考

本文档描述 `VMakeSet.vmake`（VMake 项目构建配置）**实际支持**的语法，
与 `tools/vmake/src/config.cpp` / `tools/vmake/src/build.cpp` / `tools/vmake/src/main.cpp`
保持一致。Tree-sitter 高亮定义位于 `tree-sitter-vmake/grammar.js`。

## 1. 文件与运行方式

- 配置文件名固定为 `VMakeSet.vmake`，必须位于**当前工作目录**（vmake 从 cwd 读取）。
- 每个项目一个清单；清单中声明若干 `exe(...)` / `dylib(...)` / `staticlib(...)` 目标。
- `sources` 支持 `.vx`（vxc 编译）与 `.c` / `.cpp` / `.cc` / `.cxx`（clang -c 编译）混合。
- 运行：

```bash
vmake build -B <构建目录> [目标名...] [选项]   # 编译并链接
vmake run   -B <构建目录> [目标名] [选项] [-- 参数...]  # 构建并运行 exe
vmake test  -B <构建目录> [目标名...] [选项] [-- 参数...] # 构建并运行 tests
vmake clean [-B <构建目录>]                    # 删除构建目录
```

- 构建目录 `-B` **必填**（不存在会自动创建；`clean` 可省略，默认 `build-out`）；
  VMake 自身不编译，只编排 `vxc`（每源文件）、`vlnk`（合并链接）与 `ar`（staticlib 归档）。
- 退出码：`0` 成功，`1` 编译/链接/测试失败，`2` 用法或配置错误。

## 2. 词法

### 2.1 注释

```vmake
# 整行注释
exe("app", sources=["main.vx"])  # 行尾注释
```

`#` 至行尾为注释，整行注释与空行不参与缩进比较（VMake 不缩进敏感）。

### 2.2 空白与换行

- 空格、Tab、`\r` 被忽略。
- 换行（`\n`）是语句分隔符，但在 `(` `)` `[` `]` 内的换行被忽略，
  因此目标声明可跨多行书写。
- 语句之间可有任意数量的空行。

### 2.3 字符串

两种引号：

```vmake
exe("app", sources=["main.vx", 'util.vx'])
```

- 双引号 `"..."` 支持转义：`\n` `\t` `\r` `\"` `\\` `\uXXXX`（4 位十六进制，
  编码为 UTF-8）；未知转义视为错误（字符串未闭合/非法）。
- 单引号 `'...'` **不处理转义**，反斜杠按字面保留。
- 字符串**不能跨行**（内部换行导致解析失败）。
- 字符串内容允许 UTF-8（如中文路径）。

### 2.4 标识符

```text
[A-Za-z_][A-Za-z0-9_-]*
```

用于目标类型关键字（`exe` / `dylib` / `staticlib`）与字段名（`name` / `sources` /
`output` / `deps` / `libs` / `lib_dirs` / `ccflags` / `cxxflags` / `incs` / `std` /
`cflags` / `defines` / `ldflags` / `tests`）。
VMake 的标识符允许连字符 `-`。

### 2.5 标点

```
(  )  [  ]  ,  =
```

## 3. 语句与声明

VMakeSet.vmake **只支持一种语句**：目标声明调用。文件不能为空（至少一个目标）。

### 3.1 目标类型

| 关键字 | 含义 | 默认产物 |
| ------ | ---- | -------- |
| `exe`  | 可执行文件 | `<name>` |
| `dylib` | 动态链接库 | 见 §6 |
| `staticlib` | 静态库（ar 归档，无链接阶段） | 见 §6 |

### 3.2 基本语法

```vmake
exe("hello", sources=["src/main.vx", "src/util.vx"])

dylib("mylib", sources=["src/lib.vx"], output="libmylib.dylib")

staticlib("util", sources=["src/util.vx"])
```

- 第一个参数可为**位置字符串**（目标名），也可省略并在关键字参数中用 `name=...` 给出。
- `sources` 为**必填**字符串数组，至少一个元素，每个元素必须是 `.vx` 文件。
- `output` 为**可选**字符串，相对 `-B` 构建目录；省略时使用默认名（§6）。
- 支持尾随逗号。

### 3.3 多行书写

```vmake
exe("app", sources=[
  "src/main.vx",   # 入口
  "src/util.vx",
])
```

括号内换行与空白被忽略，注释可在元素间出现。

### 3.4 字段规则

| 字段 | 类型 | 必填 | 说明 |
| ---- | ---- | ---- | ---- |
| `name` | 字符串 | 是（位置或关键字） | 目标名；合法字符 `A-Za-z0-9_-`，非空 |
| `sources` | 字符串数组 | 是 | 源文件列表，相对 `VMakeSet.vmake` 所在目录；`.vx` / `.c` / `.cpp` / `.cc` / `.cxx` 混合 |
| `output` | 字符串 | 否 | 产物路径，相对构建目录 |
| `deps` | 字符串数组 | 否 | 依赖的目标名；被依赖方必须是 dylib 或 staticlib 目标（§3.7） |
| `libs` | 字符串数组 | 否 | 外部库：裸名 → `-l<name>`；含路径/后缀 → `--lib <path>`（exe/dylib） |
| `lib_dirs` | 字符串数组 | 否 | 库搜索目录，`-L<dir>` 置于 `-l` 之前（exe/dylib） |
| `ccflags` | 字符串数组 | 否 | C 编译参数（`clang -c .c` 时生效，§3.10） |
| `cxxflags` | 字符串数组 | 否 | C++ 编译参数（`clang -c .cpp` 时生效，§3.10） |
| `incs` | 字符串数组 | 否 | clang 头文件搜索目录（`-I<dir>`，§3.10） |
| `std` | 字符串 | 否 | C/C++ 标准（如 `"c17"` / `"c++20"`），映射为 `-std=<v>`；仅传给匹配语言的编译 |
| `cflags` | 字符串数组 | 否 | 额外 clang 参数：编译期作用于 `.c`/`.cpp`，链接期透传 clang（exe/dylib，§3.8） |
| `defines` | 字符串数组 | 否 | 宏定义，每项转为 `-D<name>[=<value>]`（编译期与链接期均生效，§3.8） |
| `ldflags` | 字符串数组 | 否 | 链接器参数，置于链接期 clang 命令末尾（exe/dylib，§3.8） |
| `tests` | 字符串数组 | 否 | 测试源文件（`.vx`），构建为 `<name>-test` 由 `vmake test` 运行（§3.9） |

- 字段名只能是上表所列字段，其它字段报 `未知字段`。
- 同一字段重复出现报 `重复`。
- 位置 `name` 与 `name=` 同时出现报 `name 重复`。
- `sources` 为空数组报 `不能为空`；元素后缀非法报 `源文件必须是 .vx / .c / .cpp 文件`；空字符串报含空源文件路径。
- `output` 不能为空；不能是绝对路径；路径分量中不得包含 `..`（防逃逸）。
- `deps` 元素为空字符串报 `含空依赖名`；引用未声明目标报 `未声明的目标`；
  自引用报 `不能依赖自身`；被依赖方不是 dylib/staticlib 报 `必须是 dylib 或 staticlib 目标`（允许前向引用）。
- `libs` / `lib_dirs` / `ccflags` / `cxxflags` / `incs` / `cflags` / `defines` / `ldflags` 元素不能为空字符串。
- `tests` 为空数组报 `不能为空`；空路径报 `含空测试文件路径`；元素非 `.vx` 报 `测试文件必须是 .vx 文件`。
- `staticlib` 目标不允许 `deps`（无链接阶段）、不允许 `libs` / `lib_dirs` / `ldflags`（无链接阶段），
  但**允许** `cflags` / `defines` / `ccflags` / `cxxflags` / `incs` / `std`（供其 `.c`/`.cpp`
  源的 clang -c 编译）与 `tests`。
- 目标名在全部目标中必须唯一，重复报 `重复`。

### 3.5 非法示例（均应报错）

```vmake
foo()                                       # 未知目标类型
name = "x"                                  # 未知目标类型
exe(sources=["a.vx"])                       # 缺少 name
exe("a")                                    # 缺少 sources
exe("a", sources=[])                        # 不能为空
exe("a", sources=["a.txt"])                 # 必须是 .vx
exe("a", name="b", sources=["a.vx"])        # name 重复
exe("a", sources=["a.vx"], version=1)       # 未知字段
exe("bad name", sources=["a.vx"])            # 非法（含空格）
exe("a", sources=["a.vx"], output="/abs/out")   # 相对构建目录
exe("a", sources=["a.vx"], output="../out")     # 不得包含 ..
exe("a", sources=["a.vx"], deps=["ghost"])      # 未声明的目标
exe("a", sources=["a.vx"], deps=["a"])          # 不能依赖自身
exe("a", sources=["a.vx"], deps=["b"])          # 必须是 dylib 或 staticlib（b 是 exe）
exe("a", sources=["a.vx"], deps=["b"])
exe("b", sources=["b.vx"])                      # ↑ 被依赖方不是库目标
exe("a", sources=["a.vx"], cflags=[""])         # 含空 cflags 条目
exe("a", sources=["a.vx"], tests=["t.txt"])     # 测试文件必须是 .vx
exe("a", sources=["a.vx"], tests=[])            # tests 不能为空
staticlib("u", sources=["u.vx"], deps=["x"])    # staticlib 不允许 deps
staticlib("u", sources=["u.vx"], libs=["m"])    # staticlib 不允许 libs / lib_dirs
staticlib("u", sources=["u.vx"], ldflags=["-s"]) # staticlib 不允许 ldflags
```

空文件报 `未声明任何目标`。

### 3.6 staticlib 目标

```vmake
staticlib("util", sources=["src/util.vx"])
```

- 产物为 `ar` 归档（默认名见 §6），构建阶段只调用 `vxc`（`.vx`）/ `clang -c`
  （`.c`/`.cpp`）编译每个源文件再 `ar rcs` 归档（两种对象可混合归档），
  **没有链接阶段**。
- 因此不允许 `deps` / `libs` / `lib_dirs` / `ldflags`，但允许 `cflags` / `defines`
  / `ccflags` / `cxxflags` / `incs` / `std`（供其 `.c`/`.cpp` 源的编译期使用，
  见 §3.10）与 `tests`（测试二进制会并入目标自身源文件，见 §3.9）。
- `exe` / `dylib` 可通过 `deps` 依赖 staticlib（产物以 `vlnk --lib` 并入）。

### 3.7 目标依赖（deps）

```vmake
exe("app", sources=["src/main.vx"], deps=["calc"])
dylib("calc", sources=["src/calc.vx"])
```

- `app` 链接期把 `calc` 的产物（`libcalc.dylib` / `libcalc.so` / staticlib 归档）以
  `vlnk --lib` 并入；`app` 的 VX 代码用 `extern func ...` 声明库内的
  函数（符号 = 函数名）即可调用。
- 被依赖方必须是 dylib 或 staticlib 目标；允许前向引用（deps 引用后声明的目标）。
- 构建顺序保证依赖先于依赖方构建；命令行只选部分目标时，依赖经
  传递闭包自动纳入计划（如 `vmake build -B out app` 也会构建 `calc`）。
- 增量：依赖重新链接后内容变化 → 依赖方自动重链（依赖方源文件不变
  时只重链，不重编）。

### 3.8 外部库与链接期参数（libs / lib_dirs / cflags / defines / ldflags）

```vmake
exe("sys", sources=["src/sys.vx"],
    libs=["ssl", "/opt/lib/libfoo.a"],
    lib_dirs=["/usr/local/lib"],
    cflags=["-Wall", "-fno-exceptions"],
    defines=["NDEBUG", "FOO=1"],
    ldflags=["-Wl,-rpath,/opt/lib"])
```

- `libs`：裸库名（如 `"ssl"`）→ `-lssl`；含路径或后缀（如 `"/opt/lib/libfoo.a"`）
  → `vlnk --lib <path>`。`lib_dirs` → `-L<dir>`，置于 `-l` 之前。
  CLI `--lib-dir` 与目标 `lib_dirs` 合并（CLI 在前）。
- `cflags` / `defines` / `ldflags` 在**链接期**生效：vxc 前端只接受固定参数，
  所有源（`.ll` / `.c` / `.cpp` / vxrt）在 `vlnk` → clang 的单次调用中一并编译
  链接，因此三者统一透传给该次 clang 调用（`vlnk --cflag <arg>` / `--ldflag <arg>`）：
  - `cflags` 紧随输入文件（影响 `.ll`/`.c`/`.cpp` 的编译与链接）；
  - `defines` 每项转为 `-D<name>[=<value>]`（已带 `-D` 前缀则原样），归入 cflags 列；
  - `ldflags` 置于 clang 命令末尾（如 `-Wl,-rpath,...` / `-static`）。
- 目标含 `.c`/`.cpp` 源时，`cflags` / `defines` 同时在**编译期**生效于
  clang -c（见 §3.10）；含 C++ 源时 vmake 自动补 C++ 标准库
  （macOS `-lc++` / Linux `-lstdc++`）。
- 上述参数均纳入**链接签名**：修改 flags 或切换 profile 只触发重链，
  `.c`/`.cpp` 源的编译签名变化则触发重编。
- `staticlib` 无链接阶段，禁止 `ldflags`（`cflags`/`defines` 仍允许，供编译期）。

### 3.9 测试目标（tests）

```vmake
exe("app", sources=["src/app.vx"], deps=["calc"],
    tests=["tests/app_test.vx", "tests/util_test.vx"])
staticlib("calc", sources=["src/calc.vx"], tests=["tests/calc_test.vx"])
```

- `tests` 声明测试源文件（`.vx`）；`vmake test` 为每个声明了 `tests` 的目标
  **派生**一个测试可执行目标 `<name>-test`（kind=exe）并构建、运行。
- 派生规则（vlnk 合并期同模块函数名不得重复，且每程序仅一个入口）：
  - `staticlib` / `dylib`：目标本身无入口 → 测试二进制源 = **目标源 + 测试源**，
    测试文件提供 `main`；
  - `exe`：目标源含 `main`，与测试入口冲突 → 测试二进制源 = **仅测试源**，
    经 `deps` / `libs` 引用被测代码。
- 派生目标继承 `deps` / `libs` / `lib_dirs` / `cflags` / `defines` / `ldflags`
  / `ccflags` / `cxxflags` / `incs` / `std`。
- 运行规则：测试进程退出码 `0` 记 `ok`，非零记 `FAILED` 并打印其输出；
  汇总 `vmake-test: N passed, M failed`，任一失败则整体退出码 `1`。
- 未声明任何 `tests` → 打印提示并返回 `0`；显式点名无 `tests` 的目标 → 退出码 `2`。
- 派生目标名 `<name>-test` 若与清单中已有目标冲突 → 报错。
- `--` 之后的参数转发给**每个**测试程序。

### 3.10 C/C++ 源编译期参数（ccflags / cxxflags / incs / std）

```vmake
exe("app", sources=["src/main.vx", "src/platform.c", "src/extra.cpp"],
    ccflags=["-Wall"],               # 仅 .c 编译
    cxxflags=["-Wall", "-Wextra"],   # 仅 .cpp/.cc/.cxx 编译
    incs=["include", "third_party/include"],
    std="c++20")                     # 映射为 -std=c++20
```

- `sources` 中的 `.c` / `.cpp` / `.cc` / `.cxx` 由 vmake 直接调用 clang
  `-c -x c` / `-x c++` 编译为 `.o`（语言由 vmake 显式指定，不依赖驱动猜测），
  再经 `vlnk --lib` 并入链接（或进 staticlib 的 `ar` 归档）。
- 编译同时以 `-MMD -MF` 写出 `<对象>.o.d` 头依赖文件：签名纳入 `.d` 所列头文件
  的内容哈希，`#include` 的头文件修改即触发本单元重编（§5）。
- `ccflags` / `cxxflags` / `incs` / `std` / `defines` / `cflags` / profile
  （debug `-g` / release `-O2`）全部纳入**对象编译签名**：任一变化 → 对应
  `.c`/`.cpp` 源重编（`.vx` 对象不受影响）。
- `std` 仅传给匹配语言的编译（`c++*`/`gnu++*` → 仅 C++；其余 → 仅 C），
  混合源目标可共用一个 `std` 字段。
- `incs` → `-I<dir>`；CLI `-I` 目录也会传给 clang（置于 `incs` 之后）。
- clang 定位：环境变量 `VX_CC` > PATH（`clang`/`cc`/`gcc`/`clang++`/`c++`/`g++`）；
  含 `.c`/`.cpp` 源但找不到编译器 → 预检报错（退出码 `2`）。

## 4. 构建顺序

- **staticlib 先于 dylib，dylib 先于 exe 构建**，组内保持声明顺序。
- 例如声明顺序 `exe app` → `dylib calc` → `staticlib util` → `exe tool`，
  实际顺序为 `util`、`calc`、`app`、`tool`。
- 命令行可追加目标名只构建子集（如 `vmake build -B out app`）；
  被选目标的 `deps` 经传递闭包自动纳入计划，其余未列出的目标跳过，
  顺序规则不变。

## 5. 增量构建（Fresh）

- 增量判定基于**内容哈希签名**（FNV-1a），写在产物旁的 `<产物>.sig` 文件中，
  而非比较文件修改时间。
- 对象签名 = 编译器工具身份（vxc 或 clang）+ 依赖签名 + 编译参数摘要 + 源文件内容哈希 + 源路径。
- 链接签名（`vmake-link-v2`）= vlnk 工具身份 + 依赖签名 + 目标描述 +
  全部对象内容哈希 + **profile + cflags/defines/ldflags**；
  staticlib 用归档签名（vxc/clang 身份 + 编译参数摘要 + 对象内容哈希）。
- 依赖签名覆盖 `headers/` 根整树与项目内 `*.vxh`，故 `@imp`/`@cppimp`
  引用的头或实现变更同样触发重建。
- 纯 `.vx` 目标：修改 `cflags` / `defines` / `ldflags`、切换 `--release` /
  `--debug` 只触发**重链**，不重编。
- 含 `.c`/`.cpp` 源的目标：`ccflags` / `cxxflags` / `incs` / `std` / `cflags` /
  `defines` / profile 变化触发对应源**重编**（见 §3.10）；`ldflags` 变化仅重链。
- `.c`/`.cpp` 头依赖追踪：编译时 clang `-MMD -MF` 在对象旁写出 `<对象>.o.d`，
  记录源实际 `#include` 的头文件；签名计算时读 `.d` 并哈希其中每个文件的内容，
  头文件内容修改 → 对象签名失配 → 重编（`.vx` 对象不受影响）；`.d` 被删除则
  签名失配自愈重建。签名在**编译成功后**以新 `.d` 重算存储，从零构建或修改
  `#include` 列表不会产生多余重编。`-MMD` 不含系统头（系统工具链头变更不触发
  重编，与 make 一致）。
- 签名一致则该目标视为 `Fresh`，跳过编译与链接；内容未变的 `touch` 不会重编，
  vxc/vlnk 自身重建后签名自动失效。
- 产物或签名缺失也触发重建。

### 5.1 系统头变更不触发重编（-MMD）

`.c`/`.cpp` 的 `.d` 依赖文件由 clang **`-MMD`** 选项生成，语义是"只列用户头，
排除系统头"。因此**系统头（`<stdint.h>`、`<stdio.h>`、SDK 头等）的内容变更
不会进入对象签名，不会触发 `.c`/`.cpp` 重编**。这是与 `make -MMD` 一致的
既定行为，也是有意的权衡：

- **性能**：`-MD`（含系统头）会把数百个系统头列入 `.d`——典型 C++ 单元引用
  数百个头，每次构建都要全量哈希这些文件（都在系统盘），签名计算开销显著
  上升；而项目内 `.d` 通常只有几个到几十个用户头。
- **系统头由工具链管理，变更场景已被工具身份覆盖**：系统头随 Xcode /
  CommandLineTools / Linux 发行版的编译器包一同更新。对象签名含**编译器工具
  身份**（clang 路径 + 文件大小 + mtime），SDK/编译器升级几乎总伴随 clang
  二进制更新 → 工具身份失配 → 全量 `.c`/`.cpp` 重编，间接覆盖了系统头变更。
  这与 `.vx` 对象依赖 vxc 身份是同一机制。
- **安全影响**：系统头与编译器、libc、SDK 属同一整体；同一套系统上"头变了
  但编译器没变"通常只有系统 SDK 头文件被单独编辑等罕见场景（正常包管理
  更新不会发生）。

覆盖不到的边缘场景：仅替换 sysroot / SDK 而 clang 可执行文件本身未变（例如
手动指定 `--sysroot` 或 `VX_CC` 指向的编译器 mtime 未变）。若需要强制重建：

```bash
vmake clean -B <构建目录>            # 彻底重建
# 修改源文件内容（内容哈希，仅 touch 不会触发）
# 或修改任一编译期参数（ccflags/cxxflags/incs/std/cflags/defines）触发全量重编
```

> 说明：`.d` 中仍可能列出少量非头的 SDK 附属文件（如 clang 把
> `SDKSettings.json` 记为依赖）——它们会被哈希跟踪，但这类文件同样只随
> SDK/工具链升级而变，且通常与 clang 二进制同步更新。

## 6. 产物命名与路径

### 6.1 默认产物名

`host_os` 由运行平台决定（Windows 原生 PE 不再支持；Windows 用户请在 MSYS2/Git Bash/Cygwin/WSL/WSL2 等 POSIX 环境下构建，此时 `host_os` 为 `linux`/`unix` 等）：

| 目标 | macOS | Linux/Unix |
| ---- | ----- | ---------- |
| `exe` | `<name>` | `<name>` |
| `dylib` | `lib<name>.dylib` | `lib<name>.so` |
| `staticlib` | `lib<name>.a` | `lib<name>.a` |
| 测试（`<name>-test`） | `<name>-test` | `<name>-test` |

`output` 显式指定时覆盖默认名，可含子目录（如 `output="bin/custom"`），
最终路径为 `<构建目录>/<output>`。

### 6.2 中间产物

```
<构建目录>/obj/<目标名>/<index>_<stem>.vxobj   # .vx 源
<构建目录>/obj/<目标名>/<index>_<stem>.o       # .c/.cpp/.cc/.cxx 源
<构建目录>/obj/<目标名>/<index>_<stem>.o.d     # clang -MMD 头依赖文件（增量用）
```

例如源 `src/main.vx` 为第 0 个源文件时：`obj/app/0_main.vxobj`；
`src/platform.c` 为第 1 个源文件时：`obj/app/1_platform.o`。

### 6.3 头文件 / 导入库

- `-I <dir>`（可多次）指定头文件搜索目录，原样透传给每次 `vxc` 调用，
  并纳入增量依赖签名（目录内容变化会触发重编）。
- 未设置 `VX_HEADERS_DIR` / `VX_HEADERS` 时，vmake 自动尝试 `<项目根>/headers/`，其次编译器旁的
  `headers/`（解析符号链接到真实二进制位置后，最多向上 4 级）。
- 可用环境变量 `VX_HEADERS_DIR` 覆盖导入库根目录（`VX_HEADERS` 为兼容别名）。
- 查找顺序与 vxc 一致：`-I` 目录（按序）→ headers 根（环境变量 → 项目
  `headers/` → 编译器旁 `headers/` 向上查找）；`@cppimp` 的 C/C++
  头文件再追加探测系统 C/C++ 编译器的头目录；`./` 相对引用只查源文件目录。

## 7. CLI 选项

```text
Usage: vmake [build|run|test] -B <dir> [options] [target...]
       vmake clean [-B <dir>]
```

| 选项 | 说明 |
| ---- | ---- |
| `-B <dir>` / `-B<dir>` | 构建目录（必填；`clean` 默认 `build-out`） |
| `-I <dir>` / `-I<dir>` | 头文件搜索目录（可多次；透传给每个 `vxc` 调用，参与增量签名） |
| `--lib-dir <dir>` | 库搜索目录（可多次；透传 vlnk `-L<dir>`，与目标 `lib_dirs` 合并，CLI 在前） |
| `--target <triple>` | 交叉编译目标三元组（透传每次 `vxc` / `vlnk`，参与签名） |
| `--release` | release profile：链接期 clang 追加 `-O2` |
| `--debug` | debug profile（默认）：链接期 clang 追加 `-g` |
| `--color auto\|always\|never` | 诊断颜色（默认 auto；`NO_COLOR` / `TERM=dumb` / 非 tty 时关闭） |
| `-j <n>` | 并行编译任务数（默认 CPU 核数；`-j0` = 自动） |
| `-v`, `--verbose` | 打印实际执行的 `vxc`/`vlnk`/`ar` 命令 |
| `--` | 其余参数原样转发给 `run` / `test` 的目标程序 |
| `-V`, `--version` | 打印版本 |
| `-h`, `--help` | 帮助 |

子命令：

| 命令 | 说明 |
| ---- | ---- |
| `build`（默认） | 编译并链接所选目标 |
| `run` | 构建 exe 目标后直接执行（继承 stdio，保持交互性）；最多一个目标；未指定时用清单中唯一的 exe（0 个或多个 → 退出码 2） |
| `test` | 构建并运行 `tests=[...]` 派生的 `<name>-test` 目标（§3.9）；`--` 参数转发给每个测试 |
| `clean` | 删除构建目录 |

环境变量：

| 变量 | 说明 |
| ---- | ---- |
| `VX_VXC` | vxc 编译器路径（显式指定，不可执行时报错而非回退） |
| `VX_VLNK` | vlnk 链接器路径 |
| `VX_CC` | C/C++ 编译器路径（供 `.c`/`.cpp` 源；未设时按 PATH 查找 clang/cc/gcc 等） |
| `VX_AR` | ar 归档器路径 |
| `VX_HEADERS_DIR` | 导入库根目录（优先） |
| `VX_HEADERS` | 导入库根目录（兼容别名） |
| `NO_COLOR` | 禁用颜色 |

工具链查找顺序：环境变量 → vmake 可执行文件附近（含 `build/tools/vmake` 布局，
向上 3 级）→ `PATH`。

## 8. 输出风格

```text
[ 1/4] vxc src/main.vx
[ 2/4] vxc src/util.vx
[ 3/4] vlnk app
[ 4/4] vlnk libcalc.dylib
vmake-builder: build
```

- 每步打印 `[ k/N] <工具> <对象>`；`-v` 时另打印实际命令行。
- 编译失败时打印子进程合并输出，并以 `error: could not compile \`name\` (kind)` 结尾。
- 链接失败时以 `error: could not link \`name\` (kind)` 结尾。
- `vmake test` 每个测试打印 `test <name> ... ok` / `... FAILED (exit N)`，
  汇总 `vmake-test: N passed, M failed`（失败时打印测试捕获的输出）。

## 9. 语法（EBNF）

```ebnf
file          := { newline } { statement { newline } }
statement     := target_call
target_call   := kind "(" [ string "," ] [ keyword_args ] ")"
kind          := "exe" | "dylib" | "staticlib"
keyword_args  := keyword_arg ( "," keyword_arg )* [ "," ]
keyword_arg   := identifier "=" ( string | string_array )
string_array  := "[" [ string ( "," string )* [ "," ] ] "]"
string        := dq_string | sq_string
dq_string     := '"' { char | escape } '"'
sq_string     := "'" { char } "'"
identifier    := [A-Za-z_] [A-Za-z0-9_-]*
newline       := "\n"
comment       := "#" { any except "\n" }
```

## 10. 已知限制

- `.c`/`.cpp` 源的编译期参数独立于链接期参数（§3.10）；vxc 前端目前不接受
  任意编译标志，故 `.vx` 源无独立的按文件编译选项。
- `.c`/`.cpp` 源的头依赖经 `-MMD` `.d` 文件追踪（§5），但**不含系统头**：
  系统工具链头（如 `<stdint.h>`）变更不触发重编（行为说明、设计权衡与
  强制重建方法见 §5.1）。
- staticlib 无链接阶段，不支持 `deps` / `libs` / `lib_dirs` 及 `ldflags`。
- 测试目标为派生的独立可执行文件（`<name>-test`），不支持测试过滤/并行运行/
  测试框架；`tests` 中的文件需自带 `main`（staticlib/dylib）或经 deps 引用被测代码（exe）。
- `vmake-lsp` 覆盖单文件诊断与常用编辑功能；跨文件语义（引用查找、重命名、工作区符号）尚未支持。
- 增量判断基于内容哈希签名（产物旁 `<产物>.sig`），并纳入导入库依赖；
  不支持按文件修改时间的旧式判定。
- 不支持条件/变量/函数等配置逻辑；VMakeSet.vmake 是纯声明式。

## 11. 示例

最小项目：

```vmake
# VMakeSet.vmake
exe("hello", sources=["src/main.vx", "src/util.vx"])

dylib("calc", sources=["src/calc.vx"], output="libcalc.dylib")

staticlib("util", sources=["src/util.vx"])
```

```bash
vmake build -B build-out
./build-out/hello
```

带依赖、外部库、链接期参数与测试：

```vmake
exe("app", sources=["src/app.vx"], deps=["calc", "util"],
    libs=["ssl"], lib_dirs=["/usr/local/lib"],
    cflags=["-Wall"], defines=["NDEBUG"], ldflags=["-Wl,-rpath,/opt/lib"],
    tests=["tests/app_test.vx"])
dylib("calc", sources=["src/calc.vx"])
staticlib("util", sources=["src/util.vx"], tests=["tests/util_test.vx"])
```

```bash
vmake build -B build-out                # 构建全部
vmake build -B build-out app --release  # release profile（仅重链不重编）
vmake run   -B build-out                # 构建并运行唯一 exe
vmake run   -B build-out app -- --port 8080  # 参数转发给 app
vmake test  -B build-out                # 构建并运行 app-test / util-test
vmake test  -B build-out util           # 只跑 util 的测试
vmake clean -B build-out
```

## 12. 编辑器支持

### 12.1 高亮（tree-sitter）

`tree-sitter-vmake/grammar.js` 定义了 VMakeSet.vmake 的语法（目标类型关键字
`exe` / `dylib` / `staticlib`、字段名、字符串与注释），供编辑器高亮与语法导航使用。

### 12.2 语言服务器（vmake-lsp）

`vmake-lsp` 是 VMakeSet.vmake 的 LSP 服务器（`vmake-lsp --stdio`），纯标准 C++23 实现。
诊断规则与 `tools/vmake/src/config.cpp` 逐条对齐，编辑时即可看到 `vmake build` 会报的同类错误：

| 能力 | 方法 |
| ---- | ---- |
| 诊断（推送 + 拉取） | `textDocument/publishDiagnostics`、`textDocument/diagnostic` |
| 大纲 | `textDocument/documentSymbol` |
| 悬停 | `textDocument/hover`（含 `cflags` / `defines` / `ldflags` / `tests` 字段说明） |
| 跳转定义 | `textDocument/definition`（源文件/测试文件跳到清单目录下对应的 `.vx`；deps 跳到被依赖目标声明） |
| 补全 | `textDocument/completion`（`exe` / `dylib` / `staticlib`、全部字段名、字段值片段） |
| 格式化 | `textDocument/formatting`（规范化为单行，保留附加字段） |
| 折叠 | `textDocument/foldingRange`（多行目标声明与字符串数组） |
| 保存后重查 | `textDocument/didSave` |

Neovim（0.11+）接入示例：

```lua
vim.filetype.add({ extension = { vmake = 'vmake' } })
vim.lsp.config['vmake-lsp'] = {
  cmd = { 'vmake-lsp', '--stdio' },
  filetypes = { 'vmake' },
  root_markers = { 'VMakeSet.vmake' },
}
vim.lsp.enable('vmake-lsp')
```

其他编辑器同理：以 `vmake-lsp --stdio` 为命令、`VMakeSet.vmake` 为语言标识注册即可。