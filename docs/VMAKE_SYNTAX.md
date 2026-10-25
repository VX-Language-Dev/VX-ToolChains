# VMake 构建配置语法参考

本文档描述 `VMakeSet.vmake`（VMake 项目构建配置）**实际支持**的语法，
与 `tools/vmake/src/config.cpp` / `tools/vmake/src/build.cpp` / `tools/vmake/src/main.cpp`
保持一致。Tree-sitter 高亮定义位于 `tree-sitter-vmake/grammar.js`。

## 1. 文件与运行方式

- 配置文件名固定为 `VMakeSet.vmake`，必须位于**当前工作目录**（vmake 从 cwd 读取）。
- 每个项目一个清单；清单中声明若干 `exe(...)` / `dylib(...)` 目标。
- 运行：

```bash
vmake build -B <构建目录> [目标名...] [选项]
```

- 构建目录 `-B` **必填**（不存在会自动创建）；VMake 自身不编译，只编排
  `vxc`（每源文件）与 `vlnk`（合并链接）。
- 退出码：`0` 成功，`1` 编译/链接/环境错误，`2` 用法或配置错误。

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

用于目标类型关键字（`exe` / `dylib`）与字段名（`name` / `sources` / `output`）。
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
| `exe`  | 可执行文件 | `<name>`（Windows 追加 `.exe`） |
| `dylib` | 动态链接库 | 见 §6 |

### 3.2 基本语法

```vmake
exe("hello", sources=["src/main.vx", "src/util.vx"])

dylib("mylib", sources=["src/lib.vx"], output="libmylib.dylib")
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
| `sources` | 字符串数组 | 是 | 源文件列表，相对 `VMakeSet.vmake` 所在目录 |
| `output` | 字符串 | 否 | 产物路径，相对构建目录 |

- 字段名只能是 `name` / `sources` / `output`，其它字段报 `未知字段`。
- 同一字段重复出现报 `重复`。
- 位置 `name` 与 `name=` 同时出现报 `name 重复`。
- `sources` 为空数组报 `不能为空`；元素非 `.vx` 报 `必须是 .vx`；空字符串报含空源文件路径。
- `output` 不能为空；不能是绝对路径；路径分量中不得包含 `..`（防逃逸）。
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
```

空文件报 `未声明任何目标`。

## 4. 构建顺序

- **动态库（dylib）始终先于可执行文件（exe）构建**，组内保持声明顺序。
- 例如声明顺序 `exe app` → `dylib calc` → `exe tool` → `dylib extra`，
  实际顺序为 `calc`、`extra`、`app`、`tool`。
- 命令行可追加目标名只构建子集（如 `vmake build -B out app`）；
  未列出的目标跳过，顺序规则不变。

## 5. 增量构建（Fresh）

- 若产物文件存在，且其修改时间**不早于** `VMakeSet.vmake` 与所有源文件，
  则该目标视为 `Fresh`，跳过编译与链接。
- `-v/--verbose` 时打印 `Fresh <name> (<kind>)`；默认静默，仅打印 `Finished`。
- 任一源文件或配置被修改即触发重编；产物缺失也触发。

## 6. 产物命名与路径

### 6.1 默认产物名

`host_os` 由运行平台决定：

| 目标 | macOS | Linux | Windows |
| ---- | ----- | ----- | ------- |
| `exe` | `<name>` | `<name>` | `<name>.exe` |
| `dylib` | `lib<name>.dylib` | `lib<name>.so` | `<name>.dll` |

`output` 显式指定时覆盖默认名，可含子目录（如 `output="bin/custom"`），
最终路径为 `<构建目录>/<output>`。

### 6.2 中间产物

```
<构建目录>/obj/<目标名>/<index>_<stem>.vxobj
```

例如源 `src/main.vx` 为第 0 个源文件时：`obj/app/0_main.vxobj`。

### 6.3 头文件 / 导入库

- 未设置 `VX_HEADERS` 时，vmake 自动尝试 `<项目根>/headers/`，其次编译器旁的
  `headers/`（最多向上 4 级）。
- 可用环境变量 `VX_HEADERS` 覆盖导入库根目录。

## 7. CLI 选项

```text
Usage: vmake [build] -B <dir> [options] [target...]
```

| 选项 | 说明 |
| ---- | ---- |
| `-B <dir>` / `-B<dir>` | 构建目录（必填） |
| `--color auto\|always\|never` | 诊断颜色（默认 auto；`NO_COLOR` / `TERM=dumb` / 非 tty 时关闭） |
| `-v`, `--verbose` | 打印实际执行的 `vxc`/`vlnk` 命令与 Fresh 行 |
| `-V`, `--version` | 打印版本 |
| `-h`, `--help` | 帮助 |

环境变量：

| 变量 | 说明 |
| ---- | ---- |
| `VX_VXC` | vxc 编译器路径（显式指定，不可执行时报错而非回退） |
| `VX_VLNK` | vlnk 链接器路径 |
| `VX_HEADERS` | 导入库根目录 |
| `NO_COLOR` | 禁用颜色 |

工具链查找顺序：环境变量 → vmake 可执行文件附近（含 `build/tools/vmake` 布局，
向上 3 级）→ `PATH`。

## 8. 输出风格（Cargo 风格）

```text
    Compiling mylib (dylib)
    Compiling app (exe)
     Finished build in 0.35s
```

- 状态行右对齐 12 列标签（`Compiling` / `Fresh` / `Finished` / `Running`）。
- 编译失败时打印子进程合并输出，并以 `error: could not compile \`name\` (kind)` 结尾。
- 链接失败时以 `error: could not link \`name\` (kind)` 结尾。

## 9. 语法（EBNF）

```ebnf
file          := { newline } { statement { newline } }
statement     := target_call
target_call   := kind "(" [ string "," ] [ keyword_args ] ")"
kind          := "exe" | "dylib"
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

- 仅支持 `exe` 与 `dylib` 两类目标；无测试目标、无自定义编译标志。
- 目标间没有显式依赖声明，仅按「dylib 先于 exe」的固定顺序构建。
- `vmake-lsp` 覆盖单文件诊断与常用编辑功能；跨文件语义（引用查找、重命名、工作区符号）尚未支持。
- 增量判断仅比较文件修改时间，不校验内容哈希。
- 不支持条件/变量/函数等配置逻辑；VMakeSet.vmake 是纯声明式。

## 11. 示例

最小项目：

```vmake
# VMakeSet.vmake
exe("hello", sources=["src/main.vx", "src/util.vx"])

dylib("calc", sources=["src/calc.vx"], output="libcalc.dylib")
```

```bash
vmake build -B build-out
./build-out/hello
```

只构建指定目标并打印命令：

```bash
vmake build -B build-out hello -v
```

## 12. 编辑器支持

### 12.1 高亮（tree-sitter）

`tree-sitter-vmake/grammar.js` 定义了 VMakeSet.vmake 的语法（关键字 `exe` / `dylib`、
字段名、字符串与注释），供编辑器高亮与语法导航使用。

### 12.2 语言服务器（vmake-lsp）

`vmake-lsp` 是 VMakeSet.vmake 的 LSP 服务器（`vmake-lsp --stdio`），纯标准 C++23 实现。
诊断规则与 `tools/vmake/src/config.cpp` 逐条对齐，编辑时即可看到 `vmake build` 会报的同类错误：

| 能力 | 方法 |
| ---- | ---- |
| 诊断（推送 + 拉取） | `textDocument/publishDiagnostics`、`textDocument/diagnostic` |
| 大纲 | `textDocument/documentSymbol` |
| 悬停 | `textDocument/hover` |
| 跳转定义 | `textDocument/definition`（源文件跳到清单目录下对应的 `.vx`） |
| 补全 | `textDocument/completion`（`exe` / `dylib`、字段名、字段值片段） |
| 格式化 | `textDocument/formatting`（规范化为 `exe("name", sources=[...])` 单行） |
| 折叠 | `textDocument/foldingRange`（多行目标声明与 `sources` 数组） |
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