# CHANGELOG

## v3.1 — 关键字大规模裁减 (2026-06-19)

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
