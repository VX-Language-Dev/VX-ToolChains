# CHANGELOG

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
