# VXC 编译器改进分析（重写前） (已归档：Go 工具链版本)

> **注意**：此文档记录 Go 版本工具链的分析。当前项目已迁移至 C++ 重写，Go 工具链已删除。此文档仅作历史参考。

> 目标背景：彻底重写 `compiler/vxc.vx`（自举 CLI 入口），**保持现有参数支持不变**，
> 输出改为 **Cargo 风格 + 彩色**，退出码正确。
> 本文档盘点现状、可改进点、可行性约束，并给出建议重写范围。

---

## 0. 现状概览

- 自举编译链路：`.vx` → Lexer → Parser/TypeIR → VXOBJ Writer → `.vxobj`，由 `compiler/` 下 5 个 VX 文件实现：
  - `vxc.vx`（主入口 / CLI / import 合并）
  - `lexer.vx`（词法，关键字表为模块级 `var`）
  - `parser.vx`（语法 + TypeIR 生成）
  - `codegen.vx`（TypeIR 模块生成 / VXOBJ 数据）
  - `vxobj.vx`（VXOBJ v4 序列化）
- `vxc.vx` 本身由 **Go vxc**（`go/internal/compiler`）编译，故其语法受 Go 编译器子集限制。
- 运行时已具备的平台能力（`go/internal/translator/platform.go`）：
  - `exit` / `halt`：`declare void @exit(i32) noreturn` 已生成（L36、L119、L147），**可直接 `exit(1)` 设置退出码**。
  - 颜色：无内置 ANSI 转义，VX 字符串转义仅支持 `\n \t \\ \" \'`（`\x1b` / `\e` 不支持），需 `chr(27)` 手工拼 ESC。

---

## 一、CLI 层（重写主目标，优先级 P0）

### 1. 退出码全部丢失（最严重）
所有错误路径仅 `out(...)` 后 `return`，编译失败退出码仍为 `0`：
- `compiler/vxc.vx#L130-132` 读源文件失败
- `compiler/vxc.vx#L138-140` Lexer 失败
- `compiler/vxc.vx#L146-148` Parser 失败
- `compiler/vxc.vx#L230-232` CodeGen 失败
- `compiler/vxc.vx#L237-240` VXOBJ 序列化失败

Makefile / CI / 脚本无法判断编译是否成功。
**改进**：失败路径统一 `exit(1)`；成功路径默认 `exit(0)`（可不写）。

### 2. 参数解析脆弱（P0）
- `-o` / `--target` 缺参数时静默吞掉，不报错（`compiler/vxc.vx#L100-107`：`if i+1 < arg_count` 不满足则直接跳过，OUTPUT/TARGET 不变）。
- 未知选项（如 `--targe`、`--version`）落入 `else` 分支被当成源文件名（`compiler/vxc.vx#L113-116`），最终误报 `cannot read source file`。
- 多个位置参数时只取第一个、其余静默丢弃（`source_file` 仅首次赋值）。
- 缺 `--version`。
- 帮助文本版本号 `v0.1`（`compiler/vxc.vx#L248`）与 README 不一致，需对齐。

### 3. 输出风格（本次改造核心，P0）
- 现状：错误是 `Error: ...` / `Lexer error: ...` 纯文本，全部走 `out()`（stdout）；阶段日志缩进两个空格无条件打印 `Written: ...`。
- Cargo 风格目标：
  - `error:` 红色、`warning:` 黄色、`Compiling` / `Finished` 绿色；阶段进度（Lexer/Parser/CodeGen）用 dim 灰，仅 `-v` 显示。
  - 成功：`Compiling <name> ...` → `Finished ...` + 产物路径（绿色）。
  - 失败：`error: <message>` + 定位（见第四节）。
- 颜色开关：自举运行时无 `isatty` / `env` 检测，提供 `--color=<auto|always|never>`（默认 `always`；`never` 或环境变量 `NO_COLOR` 关闭）。

---

## 二、诊断质量（P1，部分需 lexer/parser 配合）

### 4. 错误无定位细节（P1）
- parser 错误形如 `expected token 30 got 10 at line 3`（`compiler/parser.vx#L49`）：无**文件名**、无**列号**、无**源码行摘录**。
- Token 结构为 `[kind, value, line]`，**缺少列号**（`compiler/lexer.vx#L82-84`）：需 lexer 记录 `col`。
- Cargo 风格应提供：`--> compiler/foo.vx:3:5` + 源码行 + `^` 指示。

### 5. fail-fast 单错误（P2）
每个阶段遇错即停。Cargo/rustc 风格是**聚合多错误**一次性报告。
- 至少 import 循环内错误当前是逐个 `continue` 丢弃（`compiler/vxc.vx#L185-200`），可改为累积后一起报。

---

## 三、正确性与健壮性疑点（重写时应修复或明确验证，P1/P2）

### 6. import 模块的 `top_vars` 未合并（需验证）
- 主模块 `module.top_vars` 被收集（`compiler/vxc.vx#L156`），但 import 循环只合并 `funcs/classes/structs`（`compiler/vxc.vx#L206-221`）。
- `compiler/lexer.vx#L47-102` 恰好大量使用模块级 `var`（`KW_*`、`kw_names`、`kw_values`）。
- **分析**：当前 bootstrap 能工作，是因为 `vxc.vx` 由 Go vxc 编译时，Go 层的 import 处理（正常模块链接，含 top_vars）与自举 `vxc.vx` 的"手工合并"是两条路径。自举路径运行时若把 import 的函数内联进主模块却不带其 top_vars，存在符号缺失风险。**结论：需在自举 vxc 实际跑通前验证**；若确为问题，import 循环应一并合并 `imp_module.top_vars`。

### 7. import 无去重 / 无循环检测（P2）
- 同一模块被 import 两次会合并两份函数；循环 import 会无限递归（`compiler/lexer.vx` 的 `import std.error` + 其他链路上游）。
- **改进**：维护 `visited` 集合（按 `resolved` 路径去重），并加递归深度上限。

### 8. `is_file_readable` 读全文件只为判断存在性（P2）
- `compiler/vxc.vx#L29-33` 每次路径探测都 `file_read` 整个文件内容后丢弃；`resolve_import_path` 最多探测 4 条路径（`L48-65`），import 多文件时浪费为 O(n·size)。
- 运行时已有 `file.exists` 平台能力（`platform.go#L34,118,291`），应改用 `file_exists(path)` 探测、仅最后命中时 `file_read`。

### 9. import 路径靠 CWD 相对猜测（P2）
- `resolve_import_path` 依赖 `std/`、`../`、`source_dir/` 等硬编码猜测（`compiler/vxc.vx#L47-63`），无"包根"概念，跨目录构建易失败。

---

## 四、重写约束与可行方案（自举限制）

1. **语法受限**：`vxc.vx` 须经 Go vxc 编译。注释已提示"Go vxc 不支持多行 dict 字面量"（`compiler/vxc.vx#L225`），单行字典字面量结构需保留。
2. **颜色实现**：`"\\x1b"` 不行；用 `chr(27) + "[31m"` 等封装。建议三个 helper：
   - `color(c:str, s:str) -> str`：按开关决定加不加 ESC。
   - `err(msg:str)` / `warn(msg:str)` / `info(msg:str)`：统一前缀 + 颜色 + 换行。
3. **退出码**：`exit(1)` 可行（platform.go 已支持），失败路径替换 `return`。
4. **保持参数面**：`-o <file>`、`--target <triple>`、`-v/--verbose`、`-h/--help`、位置源文件参数，外加新增 `--color=`、`--version`。
5. **诊断增强依赖 lexer**：若要做 Cargo 风格定位，需先在 `lexer.vx` 的 `make_tok` 增加 `col` 字段（`compiler/lexer.vx#L83-84`），parser/error 透传。

---

## 五、建议重写范围（待确认后动手）

| 项 | 文件 | 改动 |
|----|------|------|
| 结构化参数解析 | `vxc.vx` | 抽 `parse_args()`，缺参/未知项报错并 `exit(2)`；多源文件报错 |
| 彩色输出层 | `vxc.vx` | `color/err/warn/info` + `--color` 开关 + `NO_COLOR` |
| 正确退出码 | `vxc.vx` | 所有失败路径 `exit(1)`，成功 `exit(0)` |
| Cargo 风格日志 | `vxc.vx` | `Compiling`/`Finished` 绿，verbose 阶段灰，错误红黄 |
| `--version` | `vxc.vx` | 新增，版本号对齐 README |
| import 去重/循环 | `vxc.vx` | `visited` 集合 + 深度上限 |
| file_exists 探测 | `vxc.vx` | 替换 `is_file_readable` 全量读取 |
| top_vars 合并 | `vxc.vx` | 验证后决定是否在 import 循环合并 `top_vars` |
| lexer 列号 | `lexer.vx` | `make_tok` 增加 `col`，error 透传（用于定位） |

**不改动**（保持兼容）：`-o`/`--target`/`--verbose`/`--help`/位置参数的既有语义；VXOBJ 输出格式；与 `vlnk` 的对接契约。
