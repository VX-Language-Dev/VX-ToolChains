# CHANGELOG

## v1.6.0 — 原生编译路径唯一化 + LSP 增强 (2026-06-29)

### 原生编译路径

- **移除字节码-VM 解释执行路径**：彻底删除 `src/vm.rs`、`src/vm_dispatch.rs`、`src/vm_exec.rs`、`src/value.rs`、`src/instruction.rs`、`src/debugger/` 等 VM 相关文件
- **VXOBJ v4 替代 VXCO v1**：中间文件格式升级为 `VXOBJ V4`，magic 为 `VXOBJ`，version 为 4
  - 编译器默认输出 `.vxobj` 文件
  - 链接器仅支持 `native` 模式，自动适配目标平台并添加可执行文件特征
- **链接器外部函数处理**：未解析函数调用使用 `u32::MAX` 标记，外部函数声明在 AOT 阶段被跳过并动态导入
- **CLI 工具静默输出**：`vxcompiler`、`vxlinker`、`builder`、`vpm` 正常运行时不再输出调试/状态信息，仅保留错误与帮助

### LSP 与 IDE 支持增强

- **自动补全**：
  - 函数签名 snippet（参数占位符）
  - `struct` / `class` 的 `new Type(...)` 模板
  - `.` 和 `->` 成员访问补全
  - `import` 模块路径补全
  - 基于作用域深度的符号排序（局部变量优先）
- **悬停提示**：
  - 精确的 token range 高亮
  - 结构体/类字段与方法悬停
  - 调用点函数签名悬停
  - 内置函数（`out`、`sys_argv`、`len`、`panic`）hover 兜底
- **跳转定义**：
  - 作用域内最近定义查找
  - 支持函数参数、局部变量、`import` 别名、方法内精确跳转

### 文档

- `README.md` 更新为仅描述原生编译路径、VXOBJ v4、Rust 内存模型
- `CHANGELOG.md` 新增 v1.6.0 条目
- 新增 `CONTRIBUTING.md` 贡献指南

### 验证结果

- `cargo check` 通过
- `cargo test --features aot` 全部通过
- `test_native_compilation.sh` 10/10 通过

### 修改文件列表

```
Cargo.toml                — 移除 VM/调试器二进制目标与 lz4_flex 依赖
src/bytecode.rs           — VXOBJ v4 容器格式
src/compiler_module.rs    — 输出 .vxobj，外部依赖写入 VXOBJ v4
src/compiler_typeir.rs    — 未解析调用使用 u32::MAX
src/aot_backend.rs        — 跳过外部函数声明，动态导入外部符号
src/vxlinker.rs           — 仅保留 native 模式，静默输出
src/builder.rs            — 静默输出
src/pm.rs                 — 静默输出
src/ipt.rs                — 静默输出，支持 --target
src/lsp/backend.rs        — 补全触发字符扩展为 . 和 ->
src/lsp/completion.rs     — snippet、成员补全、import 补全、作用域排序
src/lsp/hover.rs          — 精确 range、字段/方法/调用 hover、内置函数
src/lsp/goto.rs           — 作用域内精确跳转定义
README.md                 — 原生编译路径与 VXOBJ v4 文档
CHANGELOG.md              — 新增 v1.6.0 条目
CONTRIBUTING.md           — 新增贡献指南
```

---

## v1.5.0 — 纯静态类型系统 (2026-06-28)

### 类型系统

- **移除 `var` 动态类型与类型推断**
  - `var x = ...` 语法现在产生编译错误
  - `x: var = ...` 类型注解被禁止
  - 类字段仅支持 `name: Type = value`，不再允许无类型默认值

- **强制显式类型声明**
  - 所有变量声明必须带 `: Type` 注解：`x: int = 1`
  - 函数参数必须显式声明类型：`func add(a: int, b: int) -> int`
  - 结构体/类/联合字段必须显式声明类型
  - 编译器 `VarDecl` 分支拒绝缺失类型注解的 AST

- **类型名映射统一**
  - 新增 `Compiler::type_name_to_known_type` 统一解析 `int`/`float`/`double`/`bool`/`string`/`pointer`/`void`
  - `compiler_module::parse_param_type` 复用该映射

### LSP 适配

- 内置类型补全移除 `var`，新增 `pointer`
- `var` 关键字悬停/补全提示标记为“已移除”
- 变量符号导航现在展示显式类型注解
- 变量悬停信息格式改为 `name: Type`

### 文档

- `README.md` 新增“静态类型系统”章节，更新内存模型示例为显式类型
- `CHANGELOG.md` 新增 v1.5.0 条目

### 修改文件列表

```
src/parser/expr.rs        — parse_type 拒绝 var 类型
src/parser/stmt.rs        — 移除 parse_var_decl_inferred；类字段必须带类型；var 语句报错
src/compiler_core.rs      — 新增 type_name_to_known_type
src/compiler_stmt.rs      — VarDecl 强制使用显式声明类型，拒绝缺失注解
src/compiler_module.rs    — parse_param_type 复用统一映射
src/compiler_ownership.rs — 默认类型 fallback 改为 unknown；for 循环变量类型改为 int
src/lsp/completion.rs     — 移除 var 类型补全，更新 var 关键字说明
src/lsp/hover.rs          — var 悬停提示更新，变量悬停格式更新
src/lsp/symbols.rs        — 变量符号携带类型注解，for 循环变量类型 int
README.md                 — 新增静态类型系统章节，修正示例
CHANGELOG.md              — 新增 v1.5.0 条目
tests/fixtures/*.vx       — 将 var 推断改为显式类型
tests/integration_test.rs — 新增 var 拒绝测试
```

---

## v1.4.0 — 原生编译增强 (2026-06-26)

### 原生编译系统

- **VXCO 中间文件格式**：新增跨平台中间文件格式，支持编译器与链接器解耦
  - 编译器输出 `.vxco` 文件，包含 TypeIR 和外部依赖信息
  - 链接器自动检测外部依赖，智能选择静态/动态链接策略
  - 支持 Linux、macOS、Windows 三平台自动适配

- **链接器优化**：
  - **Linux**：静态链接使用 `_start` 入口点 + `exit` syscall，动态链接使用 `cc` 并自动链接 C 运行时库
  - **macOS**：静态链接使用 `_main` 入口点，动态链接自动链接 `-lSystem`
  - **Windows**：静态链接使用 `mainCRTStartup` 入口点，动态链接使用 MSVC 运行时库
  - 所有平台均添加 `-O2` 优化选项，Linux 静态链接添加 `--gc-sections` 移除未使用段

- **外部依赖追踪**：编译器自动识别 `import` 语句，将外部库依赖信息写入 VXCO 文件
  - 无外部依赖：默认静态链接，生成完全独立的可执行文件
  - 有外部依赖：自动切换动态链接，链接相应系统库

- **CLI 输出保持英文**：编译器和链接器 CLI 输出保持英文风格，统一国际化体验
  - `[OK] Compiled: xxx.vxco (VXCO v1)`
  - `[*] Native compilation: N functions`
  - `[+] Native linked: xxx (static=true/false)`

### 编译器改进

- **TypeIR 函数调用修复**：修复 `Call` 指令目标函数 ID 解析错误，通过函数名映射正确解析调用目标
- **返回类型推断**：函数有返回值但未指定类型时，默认推断为 `Int` 类型

### 验证结果

- 静态链接测试：`test_static.vx` (add(10, 20)) → 退出码 30 ✓
- 动态链接测试：`test_dynamic.vx` (import libc) → 退出码 42 ✓
- 跨平台适配：Linux 平台验证通过，macOS/Windows 代码已实现待验证

### 修改文件列表（8 文件，+450 / −120）

```
src/bytecode.rs           — 新增 VXCO 格式定义和序列化/反序列化
src/compiler_typeir.rs    — 修复 Call 指令函数 ID 解析
src/compiler_module.rs    — 添加外部依赖追踪和 TypeIR 生成优化
src/vxlinker.rs           — 跨平台链接器实现和 CLI 输出英文化
src/aot_backend.rs        — 清理调试输出
src/ipt.rs                — 编译器 CLI 输出英文化
README.md                 — 更新原生编译文档
CHANGELOG.md              — 新增 v1.4.0 条目
```

---

## v1.3.4 — 性能优化与语言扩展 (2026-06-23)

### 性能优化

- **Value 类型 Arc 化**：所有堆分配字段（String、Vec、HashMap）改用 `Arc<T>` 包装
  - `clone()` 仅增加引用计数（O(1)），消除 VM 热路径上的深拷贝开销
  - 实现 Copy-on-Write 语义：修改时通过 `Arc::make_mut()` 写时复制
  - 新增便捷构造器：`Value::string()`、`Value::array()`、`Value::map()`、`Value::instance()`、`Value::pointer()`
  - 新增只读访问器：`as_str()`、`as_array()`、`as_map()`，避免不必要的 clone

- **OpCode 查找表优化**：`TryFrom<u8>` 改用静态数组 `OP_LOOKUP_TABLE` 替代 `match` 表达式
  - 反序列化性能提升，代码更紧凑

- **VM 特化指令集**：新增 24 条类型特化指令，减少运行时类型分支
  - 整数特化：`AddInt`、`SubInt`、`MulInt`、`DivInt`、`ModInt`、`NegInt`、`EqInt`、`LtInt`、`GtInt`、`LeInt`、`GeInt`
  - 浮点特化：`AddFloat`、`SubFloat`、`MulFloat`、`DivFloat`、`NegFloat`、`EqFloat`、`LtFloat`、`GtFloat`、`LeFloat`、`GeFloat`
  - 逻辑特化：`Not`、`And`、`Or`
  - 整数运算内置溢出检查（`checked_add`、`checked_sub` 等），溢出时返回 `DispatchResult::Error`
  - 除零检查：整数和浮点除法/取模在除数为零时返回优雅错误

### 语言扩展

- **泛型参数语法**：`struct`、`class`、`func` 支持泛型参数列表
  - 语法：`func identity<T>(x: T) -> T { return x }`
  - AST 节点扩展：`FuncDecl`、`StructDecl`、`ClassDecl` 新增 `Vec<String>` 类型参数字段
  - LSP 模块（completion、goto、hover、symbols）全面适配泛型参数

- **match 表达式**：新增 `match` 语句解析支持
  - 语法：`match expr { pattern => { ... } ... }`
  - AST 新增 `MatchStmt` 节点

### 标准库扩展

- **VM 内建函数新增**：支持 VX 自举所需的常用函数
  - `ord(s)`：返回字符串首字符的 Unicode 码点
  - `chr(i)`：将整数转换为对应 Unicode 字符
  - `float(x)`：将 int/bool/string 转换为 float
  - `parse_int(s)` / `parse_float(s)`：字符串解析为数值，失败返回 nil
  - `file_read_bytes(path)`：读取文件内容为字节数组

### 修改文件列表（21 文件，+1220 / −359）

```
Cargo.toml                — 版本号 1.1.1 → 1.3.4
CHANGELOG.md              — 新增 v1.3.4 条目
README.md                 — 项目结构更新（macros.rs、docs）
src/value.rs              — Arc 化 + 便捷构造器 + 只读访问器
src/opcode.rs             — 新增 24 条特化指令 + 查找表优化
src/vm.rs                 — 新增内建函数 + Arc::make_mut 适配
src/vm_dispatch.rs        — 特化指令分发 + 溢出/除零检查宏
src/vm_exec.rs            — 适配 Arc 化 Value
src/compiler_core.rs      — KnownType 扩展 + 宏注册表集成
src/compiler_expr.rs      — 适配泛型参数 AST
src/compiler_module.rs    — 适配泛型参数 AST
src/parser/ast.rs         — MatchStmt + MacroDef/MacroCall + 泛型参数字段
src/parser/stmt.rs        — parse_match_stmt + parse_macro_def/call + 泛型解析
src/parser/mod.rs         — 泛型参数解析辅助函数
src/token.rs              — 适配宏关键字和 # Token
src/lsp/completion.rs     — 适配泛型参数字段
src/lsp/goto.rs           — 适配泛型参数字段
src/lsp/hover.rs          — 适配泛型参数字段
src/lsp/symbols.rs        — 适配泛型参数字段
std/README.md             — 标准库文档更新
tests/integration_test.rs — 适配 Arc 化 Value
```

---

## v1.3.0 — 编译时宏系统 (2026-06-21)

### 新增功能

- **编译时宏系统**：支持参数化宏定义和展开，零运行时开销
  - 语法：`macro name(params) { body }` 定义，`#name(args)` 调用
  - 智能缓存机制：基于参数签名的HashMap缓存，避免重复展开
  - 递归展开：支持嵌套宏和复杂表达式中的宏调用
  - 统计API：追踪展开次数、缓存命中率和性能指标

### 核心组件

- **macros.rs**：全新的宏系统模块
  - `Macro` 结构体：存储宏定义（名称、参数、body、位置）
  - `MacroRegistry` 类：管理宏的注册、查找和展开
  - 参数替换引擎：递归遍历AST并替换参数引用
  - 单元测试覆盖：注册、展开、错误处理、缓存验证

- **词法分析器扩展** (`token.rs`)
  - 新增 `Macro` 关键字Token
  - 新增 `Hash` (#) Token用于宏调用

- **语法分析器扩展** (`parser/stmt.rs`)
  - `parse_macro_def()`: 解析宏定义语法
  - `parse_macro_call_stmt()`: 解析宏调用语句

- **AST节点扩展** (`parser/ast.rs`)
  - `MacroDef`: 宏定义节点
  - `MacroCall`: 宏调用节点
  - 更新位置提取函数支持新节点

- **编译器集成** (`compiler_core.rs`)
  - `expand_macros()`: 在所有权检查前展开所有宏
  - `process_expr_for_macros()`: 递归处理嵌套表达式
  - 宏注册表集成到Compiler结构体

### 文档与示例

- **docs/macros.md**：完整的宏系统文档（语法、示例、最佳实践）
- **docs/macros_quickstart.md**：5分钟快速入门指南
- **examples/macro_demo.vx**：完整的宏系统演示程序
  - 基础宏示例（调试、重复、条件）
  - 日志系统宏
  - 断言和工具宏
  - 缓存机制演示

### 技术细节

- 宏在编译流水线的早期阶段展开（语法分析后，所有权检查前）
- 参数通过AST节点替换传递，保持类型安全
- 缓存键基于参数的结构化签名（Debug格式）
- 错误信息包含源码位置（行号、列号）
- 零外部依赖，纯Rust实现

### 兼容性

- 向后兼容：现有代码无需修改
- 宏为可选功能，不使用宏的代码不受影响
- 所有现有测试通过（68个测试用例）

---

## v1.2.0 — 编译器模块化拆分与 VM 安全加固 (2026-06-21)

### 编译器架构重构

- **compiler_core.rs 拆分**：将 1300+ 行的单体编译器拆分为 4 个职责单一的模块：
  - `compiler_core.rs` — 数据结构、构造函数、核心辅助方法
  - `compiler_expr.rs` — 表达式编译
  - `compiler_stmt.rs` — 语句编译
  - `compiler_module.rs` — 模块级编译（struct/class/enum/import/func）
  - `compiler_typeir.rs` — TypeIR 栈模拟器（从 compiler_core 迁出）
- **lib.rs 更新**：新增 `compiler_typeir`、`compiler_expr`、`compiler_stmt`、`compiler_module` 模块声明与重导出

### TypeIR 序列化增强

- `MakeArray` 序列化格式改为 `<base>,<arg0>,<arg1>,...`，携带基类型与元素 VarId
- `MakeMap` 序列化格式改为 `<k0>,<v0>,<k1>,<v1>,...`，携带完整键值对
- 反序列化同步更新，支持解析新格式
- `TypeModule` 实现 `Default` trait

### VXOBJ 写入优化

- 新增 `write_vxobj_from_module()`：直接从 `CompiledModule` 写入 VXOBJ v2，跳过中间 tuple 缓冲，减少内存分配
- 新增 `BytecodeInstructionTuple` / `VxobjFunctionRef` 类型别名，统一接口签名

### VM 安全加固

- **帧访问安全化**：`current_frame()` / `current_frame_mut()` / `current_fn()` 从 `expect()` panic 改为返回 `Option`
- **`try_frame!` 宏**：新增帧访问宏，将 panic 转换为 `DispatchResult::Error`，消除 VM 执行中的不可恢复崩溃点
- **消除 `unreachable!()`**：`vm_dispatch.rs` 中所有 `unreachable!()` 替换为带错误信息的 `DispatchResult::Error`
- **`Default` trait**：VM 实现 `Default` trait
- **`DebugHook` 类型别名**：提取 `DebugHook` 类型别名提升可读性
- **Value 清理**：移除冗余的 `Value::to_string()` 方法（已有 `Display` trait 实现）

### 词法分析器测试

- `token.rs` 新增单元测试模块，覆盖：整数/浮点数解析、科学计数法、关键字识别、已移除关键字降级为标识符、复合运算符、字符串转义

### 内存安全模块适配

- `memory_safety.rs` 适配 Option 帧访问接口，使用 `if let Some(frame)` 替代 `!self.frames.is_empty()` 检查

### 修改文件列表（23 文件，+715 / −1594）

```
Cargo.toml                — 版本号 1.0.0 → 1.1.1，描述更新
CHANGELOG.md              — 版本号格式修正
README.md                 — 项目结构更新、表格格式规范化
src/lib.rs                — 新增 compiler 子模块声明
src/compiler_core.rs      — 大规模拆分（−1300 行），仅保留核心数据结构
src/compiler_module.rs    — 新文件：模块级编译逻辑
src/bytecode.rs           — 新增 write_vxobj_from_module + 类型别名
src/type_ir.rs            — MakeArray/MakeMap 序列化增强 + Default derive
src/token.rs              — 新增单元测试模块（+118 行）
src/vm.rs                 — 帧访问 Option 化 + Default + DebugHook 别名
src/vm_dispatch.rs        — try_frame! 宏 + 消除 unreachable!
src/vm_exec.rs            — 适配 Option 帧访问
src/memory_safety.rs      — 适配 Option 帧访问
src/value.rs              — 移除冗余 to_string()
src/builder.rs            — 构建器适配
src/cache.rs              — 缓存模块适配
src/ipt.rs                — 编译器 CLI 适配
src/pm.rs                 — VPM 适配
src/lsp/completion.rs     — LSP 补全适配
src/lsp/hover.rs          — LSP 悬停适配
src/lsp/main.rs           — LSP 入口适配
src/parser/mod.rs         — 解析器适配
tests/integration_test.rs — 集成测试适配
```

---

## v1.1.1 — 关键字大规模裁减 (2026-06-19)

### 关键字精简：35 → 27（−14）

- 关键字总览：从 35 个内置关键字裁减至 **27 个** 核心关键字
- 词法分析哈希表缩小 23%，编译扫描速度提升
- 严格区分「VM 底层硬编码关键字」与「用户层标准库功能」

### 永久保留内置关键字（27 个）

**底层强制骨架核心（22 个，OpCode 绑定）**
`func` `return` `if` `else` `elif` `true` `false` `nil` `while` `for` `in` `break` `continue` `struct` `class` `enum` `union` `import` `as` `var` `new` `move`

**原生标量类型（5 个，硬件基础类型）**
`int` `float` `double` `bool` `void`

### 第一梯队裁减：移入标准库（2 个）

| 关键字 | 替代方案 |
|--------|---------|
| `string` | `std::String` 标准库类型标识符，字符串字面量自动展开 |
| `vector` | `std::Vec<T>` 标准库类型标识符，数组字面量自动展开 |

### 第二梯队裁减：改为注解 / 特殊语法（10 个）

| 关键字 | 替代方案 |
|--------|---------|
| `and` / `or` / `not` | `&&` / `\|\|` / `!` 符号运算符 |
| `public` / `private` / `protected` | `#[pub]` / `#[priv]` 属性注解 |
| `extends` / `implements` | 冒号继承语法 `class A : Parent, Trait` |
| `dirs` | `import("p1","p2") as mod` 可变参数导入 |
| `this` | 解析器语法糖，编译期自动替换为当前实例局部变量 |

### 第三梯队裁减：降级为标准库函数（2 个）

| 关键字 | 替代方案 |
|--------|---------|
| `newz` | `mem::zeroed<T>()` 标准库函数 |
| `free` | `mem::free(ptr)` 标准库函数 |

> **注意**：`new`、`move`、`var` 所有权核心关键字保留不变，Rust 风格借用检查语义不受影响。

### 底层 VM OpCode 不变

虽然关键字被裁减，但 VM 字节码指令 `Newz` / `Free` / `And` / `Or` / `Not` 等 **OpCode 继续存在**，因为标准库函数仍然需要这些底层指令来运行。

### 修改文件列表

```
src/token.rs                  — TokenType 枚举（-11 变体）+ KEYWORDS 表（-14 条目）
src/parser/ast.rs             — Expr AST 枚举（-3 变体）+ ImportStmt 签名更新
src/parser/expr.rs            — 移除 parse_newz_expr / parse_vector_literal + 精简 parse_type
src/parser/stmt.rs            — 冒号继承语法 + 可变参数导入 + 移除 parse_free_stmt
src/compiler_core.rs          — 移除 FreeStmt/VectorLiteral 编译 + 更新 ImportStmt
src/compiler_ownership.rs     — NewzExpr → NewExpr 统一堆分配检查
src/lsp/hover.rs              — 关键字悬停信息更新（裁减项标记）
src/lsp/completion.rs         — this 关键字检查适配
src/ipt.rs                    — 编译器提示字符串更新
```

### 测试

- 全量 69 个集成测试通过（`cargo test`）
- 零编译警告
