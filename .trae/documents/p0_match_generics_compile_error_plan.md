# P0 正确性修复计划：match 代码生成、泛型单态化、compile_stmt 兜底报错

## 背景与问题

VX 编译器当前采用「字节码 IR → TypeIRSimulator → TypeIR → AOT」的架构：

- [src/compiler_stmt.rs](file:///run/media/max4075/DOTNET/VX/VX-ToolChains/src/compiler_stmt.rs) 生成基于 `OpCode` 的字节码 IR；
- [src/compiler_expr.rs](file:///run/media/max4075/DOTNET/VX/VX-ToolChains/src/compiler_expr.rs) 处理表达式级节点；
- [src/compiler_module.rs](file:///run/media/max4075/DOTNET/VX/VX-ToolChains/src/compiler_module.rs) 处理模块级声明，并调用 `generate_type_ir` 得到 TypeIR；
- [src/compiler_typeir.rs](file:///run/media/max4075/DOTNET/VX/VX-ToolChains/src/compiler_typeir.rs) 的 `TypeIRSimulator` 把字节码翻译为 `TypedInstruction`；
- [src/type_ir.rs](file:///run/media/max4075/DOTNET/VX/VX-ToolChains/src/type_ir.rs) 定义 TypeIR，其中已有 `Type::Generic` 但无单态化机制；
- [all_features.vx](file:///run/media/max4075/DOTNET/VX/VX-ToolChains/all_features.vx) 已经包含 `match` 与 `Pair<int>` 的使用示例。

当前三个 P0 问题：

1. **match 已解析但无代码生成**：[compiler_stmt.rs:224](file:///run/media/max4075/DOTNET/VX/VX-ToolChains/src/compiler_stmt.rs#L224) 的 `_ => {}` 吞掉 `MatchStmt`；[compiler_expr.rs:248](file:///run/media/max4075/DOTNET/VX/VX-ToolChains/src/compiler_expr.rs#L248) 把 `Expr::MatchStmt` 列入静默忽略列表。
2. **泛型仅停留在解析层**：[compiler_module.rs:27](file:///run/media/max4075/DOTNET/VX/VX-ToolChains/src/compiler_module.rs#L27) 等处 `StructDecl/ClassDecl/FuncDecl` 的 `_type_params` 全部被忽略。
3. **compile_stmt 兜底过于宽松**：[compiler_stmt.rs:224](file:///run/media/max4075/DOTNET/VX/VX-ToolChains/src/compiler_stmt.rs#L224) 的 `_ => {}` 应改为显式 `Err`。

## 目标

- 让 `match` 语句生成可执行的比较/跳转指令，支持整型/布尔/字符串字面量、枚举变体（`Color.Red`）与 `_` 默认分支。
- 让带类型参数的 `struct/class/func` 在实例化点进行 AST 级单态化，生成具体变体后走现有代码生成路径。
- 让 `compile_stmt` 与 `compiler_module` 的兜底分支对未知节点返回明确错误，避免静默失效。

## 方案

### 1. match 语句代码生成

#### 1.1 修改点

- [src/compiler_core.rs](file:///run/media/max4075/DOTNET/VX/VX-ToolChains/src/compiler_core.rs)：在 `Compiler` 中新增 `enum_defs: HashMap<String, Vec<(String, i64)>>` 以保存枚举变体值，供 match 模式解析。
- [src/compiler_module.rs](file:///run/media/max4075/DOTNET/VX/VX-ToolChains/src/compiler_module.rs)：把当前 `Expr::EnumDecl(_, _, _, _) => {}` 改为记录变体信息到 `self.enum_defs`。
- [src/compiler_stmt.rs](file:///run/media/max4075/DOTNET/VX/VX-ToolChains/src/compiler_stmt.rs)：新增 `Expr::MatchStmt(subject, arms, _, _)` 分支，实现比较链；兜底 `_ => {}` 改为 `return Err(...)`。
- [src/compiler_expr.rs](file:///run/media/max4075/DOTNET/VX/VX-ToolChains/src/compiler_expr.rs)：把 `Expr::MatchStmt(..)` 从「静默忽略」联合分支中移除，单独返回 `Err`。

#### 1.2 生成逻辑（compiler_stmt.rs）

```rust
Expr::MatchStmt(subject, arms, _, _) => {
    // 计算被匹配值并存入临时 slot
    self.compile_expr(subject)?;
    self.pop_stack_type();
    let subject_slot = self.allocate_slot(&format!("__match_subj_{}", self.instructions.len()));
    self.emit(OpCode::StoreVar, BytecodeArg::Int(subject_slot as i32));

    let mut end_jumps: Vec<usize> = Vec::new();
    let mut default_body: Option<&Vec<Box<Expr>>> = None;

    for (pat, body) in arms {
        if self.is_default_pattern(pat) {
            default_body = Some(body);
            continue;
        }

        // 编译模式为右值
        self.compile_match_pattern(pat)?;
        self.emit(OpCode::LoadVar, BytecodeArg::Int(subject_slot as i32));
        self.emit(OpCode::BinaryEq, BytecodeArg::None);

        let next_arm = self.emit(OpCode::JumpIfFalse, BytecodeArg::None);
        self.emit(OpCode::Pop, BytecodeArg::None); // 弹出比较结果

        for st in body {
            self.compile_stmt(st)?;
        }
        end_jumps.push(self.emit(OpCode::Jump, BytecodeArg::None));

        self.patch(next_arm, self.instructions.len());
    }

    if let Some(body) = default_body {
        for st in body {
            self.compile_stmt(st)?;
        }
    }

    let end_pc = self.instructions.len();
    for j in end_jumps {
        self.patch(j, end_pc);
    }
}
```

#### 1.3 模式处理规则

新增 `compile_match_pattern`：

| 模式 | 处理 |
|---|---|
| `Identifier("_")` | 在 match 分支层识别为 default，不生成代码 |
| 字面量（int/bool/string/float/nil） | 直接 `compile_expr(pat)` |
| `PropertyAccess(Identifier(enum), variant)` | 在 `self.enum_defs` 查找 `enum.variant`，找到后 `LoadConst` 对应 i64；找不到返回错误 |
| `Identifier(name)`（非 `_`） | 按变量匹配，`compile_expr(pat)` |
| 其他 | 返回 `Err`："match 模式不支持: ..." |

> 注：枚举值的一般表达式（如 `let c = Color.Red`）不在本 P0 范围内，match 内部通过 `enum_defs` 单独处理枚举模式。

#### 1.4 表达式位置

解析器仅在语句位置生成 `MatchStmt`。在 `compile_expr` 中遇到 `Expr::MatchStmt` 时返回错误："match 只能作为语句使用"。

### 2. 泛型单态化（AST 级 monomorphization）

#### 2.1 设计原则

- 不改动 `KnownType`/`TypeIR` 的粗细粒度，继续以**字符串类型名**做替换。
- 在 `Compiler::compile()` 最开头对 AST 做一次单态化变换。
- 变换后，原 `StructDecl/ClassDecl/FuncDecl` 分支不需要任何修改即可处理具体变体。

#### 2.2 新增文件

- [src/compiler_monomorph.rs](file:///run/media/max4075/DOTNET/VX/VX-ToolChains/src/compiler_monomorph.rs)（新建）
- 在 [src/lib.rs](file:///run/media/max4075/DOTNET/VX/VX-ToolChains/src/lib.rs) 加入 `pub mod compiler_monomorph;`

#### 2.3 核心数据结构

```rust
#[derive(Clone, Debug, Eq, PartialEq, Hash)]
struct Specialization {
    base: String,       // 泛型名，如 "Pair"
    args: Vec<String>,  // 实参，如 ["int"]
}

struct GenericDecl {
    kind: GenericKind,  // Struct / Class / Func
    params: Vec<String>,
    node: Expr,         // 原始模板 clone
}
```

#### 2.4 单态化步骤

1. **收集模板**：遍历 AST，把 `_type_params` 非空的 `StructDecl/ClassDecl/FuncDecl` 登记到 `HashMap<String, GenericDecl>`。
2. **扫描实例化请求**：扫描所有类型名字符串出现的位置：
   - `VarDecl` 类型注解
   - `FuncDecl` 参数类型、返回类型、函数体
   - `StructDecl/ClassDecl` 字段类型与方法
   - `ClassDecl` 父类、接口列表
   - `NewExpr` 的 `type_name` 与类型实参
   - `CallExpr` 的 callee（用于泛型函数调用）

   对每个形如 `Name<A, B>` 的字符串，按括号深度拆出 `base` 与 `args`；若 `base` 是已知模板且参数个数匹配，记录 `Specialization`，并递归处理每个 `arg`。

3. **闭包求最小固定点**：反复扫描新产生的具体类型字符串，直到没有新的 `Specialization` 出现（处理 `Pair<Pair<int>>` 等嵌套）。

4. **生成具体变体**：对每个 `Specialization`：
   - 建立替换表 `{T -> int, U -> string}`；
   - `clone` 模板节点；
   - 将声明名改为与代码用法一致（如 `Pair<int>`）；
   - 清空 `_type_params`；
   - 用分词替换函数把所有类型名字符串中的形参替换为实参。

5. **重建 AST**：
   - 遇到泛型模板声明时，不保留模板本身，替换为它的所有具体变体；
   - 其他节点原样保留。

#### 2.5 字符串替换 helper

```rust
fn substitute_type_str(s: &str, map: &HashMap<String, String>) -> String {
    if let Some(r) = map.get(s) { return r.clone(); }
    let mut out = String::new();
    let mut cur = String::new();
    for ch in s.chars() {
        if ch == '<' || ch == '>' || ch == ',' || ch.is_whitespace() {
            if !cur.is_empty() {
                out.push_str(map.get(&cur).unwrap_or(&cur));
                cur.clear();
            }
            out.push(ch);
        } else {
            cur.push(ch);
        }
    }
    if !cur.is_empty() {
        out.push_str(map.get(&cur).unwrap_or(&cur));
    }
    out
}
```

该 helper 按 `<`, `>`, `,`, 空白分词，只对完整标识符做替换，避免破坏如 `Test`（包含 `T` 子串）这类名称。

#### 2.6 集成点

在 [compiler_module.rs](file:///run/media/max4075/DOTNET/VX/VX-ToolChains/src/compiler_module.rs) 的 `Compiler::compile()` 开头：

```rust
pub fn compile(&mut self, ast: &[Stmt]) -> Result<CompiledModule, String> {
    // ... 现有 clear ...
    self.enum_defs.clear();

    let ast = crate::compiler_monomorph::monomorphize_ast(ast.to_vec())?;
    for s in &ast { ... }
}
```

#### 2.7 关于泛型函数

- 当前解析器支持 `func f<T>()` 声明，但表达式上下文中的 `<` 被解析为二元比较运算符，因此 `f<int>()` 这类显式实例化调用**暂不支持的语法**。
- 单态化器仍会收集函数模板；若扫描到对泛型函数的 `CallExpr` 但无法推断或没有显式类型实参，返回错误："generic function `f<T>` requires explicit type arguments"。
- 本次 P0 优先保证 `struct Pair<T>` / `class Box<T>` 的实例化可用；泛型函数调用语法可在后续迭代中补充。

### 3. compile_stmt / compiler_module 兜底报错

#### 3.1 compiler_stmt.rs

把最后的：

```rust
_ => {}
```

改为：

```rust
other => {
    let (line, col) = crate::parser::pos(other);
    return Err(format!(
        "VX Error [line {}, col {}]: 未实现的语句节点: {:?}",
        line, col, other
    ));
}
```

#### 3.2 compiler_module.rs

当前模块级兜底：

```rust
_ => {
    self.compile_stmt(s)?;
}
```

改为显式白名单 + 错误：

```rust
_ => {
    match s {
        Expr::ExprStmt(..)
        | Expr::VarDecl(..)
        | Expr::Assign(..)
        | Expr::IfStmt(..)
        | Expr::MatchStmt(..)
        | Expr::WhileStmt(..)
        | Expr::ForStmt(..)
        | Expr::LoopStmt(..)
        | Expr::BreakStmt(..)
        | Expr::ContinueStmt(..)
        | Expr::ReturnStmt(..) => {
            self.compile_stmt(s)?;
        }
        other => {
            let (line, col) = crate::parser::pos(other);
            return Err(format!(
                "VX Error [line {}, col {}]: 模块级不允许的语句节点: {:?}",
                line, col, other
            ));
        }
    }
}
```

这样声明类节点已在前面显式处理，真正未实现的语句进入 `compile_stmt` 后会被其兜底 `Err` 捕获。

## 关键修改文件

- [src/compiler_stmt.rs](file:///run/media/max4075/DOTNET/VX/VX-ToolChains/src/compiler_stmt.rs)
- [src/compiler_expr.rs](file:///run/media/max4075/DOTNET/VX/VX-ToolChains/src/compiler_expr.rs)
- [src/compiler_module.rs](file:///run/media/max4075/DOTNET/VX/VX-ToolChains/src/compiler_module.rs)
- [src/compiler_core.rs](file:///run/media/max4075/DOTNET/VX/VX-ToolChains/src/compiler_core.rs)
- [src/compiler_monomorph.rs](file:///run/media/max4075/DOTNET/VX/VX-ToolChains/src/compiler_monomorph.rs)（新建）
- [src/lib.rs](file:///run/media/max4075/DOTNET/VX/VX-ToolChains/src/lib.rs)

## 验证计划

### 现有测试

```bash
cargo test
```

覆盖词法、语法、所有权、VXOBJ v4 序列化等回归测试。

### 新增测试（tests/compiler_codegen_test.rs）

至少覆盖：

1. `match` 整型 + `_` 默认分支编译成功。
2. `match` 枚举变体（`Color.Red`）编译成功。
3. `match` 布尔分支编译成功。
4. `struct Pair<T>` 实例化为 `Pair<int>` 后编译成功，且生成名为 `Pair<int>` 的构造函数。
5. `class Box<T>` 实例化为 `Box<int>` 后编译成功。
6. 未知 AST 节点进入 `compile_stmt` 时返回 `Err`。
7. `Expr::MatchStmt` 出现在表达式位置时返回 `Err`。

### 手动集成验证

编写最小 VX 程序 `examples/match_generic.vx`：

```vx
enum Color:
    Red
    Green
    Blue

struct Pair<T>:
    first: T
    second: T

func main()
    c: Color = Color.Red
    match c:
        Color.Red: sys_print("red")
        Color.Green: sys_print("green")
        _: sys_print("other")

    p: Pair<int>
    p.first = 1
    p.second = 2
```

执行：

```bash
cargo run --bin vxc -- examples/match_generic.vx -o out.vxobj
```

确认 `vxc` 正常退出、`out.vxobj` 生成；可用 `cargo run --bin vxde -- out.vxobj` 反编译查看 `JumpIfFalse`/`Jump` 与 `Pair<int>` 构造函数。
