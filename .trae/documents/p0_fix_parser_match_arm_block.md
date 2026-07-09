# P0 修复：match 解析器 parse_match_arm 中 parse_block() 过度消费 token

## 问题

三个 P0 修复（match 代码生成、泛型单态化、compile_stmt 兜底报错）的代码已经全部实现。但 codegen 测试 `test_match_int_with_default`、`test_match_bool`、`test_match_enum_variant` 全部失败，错误为 `"意外token: Colon"`。

**根因**：[src/parser/stmt.rs:406](file:///run/media/max4075/DOTNET/VX/VX-ToolChains/src/parser/stmt.rs#L406) 中 `parse_match_arm()` 调用 `parse_block()` 来解析分支体。`parse_block()` 会消费所有 token 直到遇到 Dedent，但在 match 语句中，所有分支共享同一个 Indent/Dedent 层级——各分支之间没有独立的 Indent/Dedent 对。因此 `parse_block()` 会吞掉后续分支的模式和冒号，导致解析失败。

**token 流示例**（`match x: 0: sys_print("zero") 1: sys_print("one") _: sys_print("other")`）：

```
... match x : Indent 0 : sys_print("zero") Newline 1 : sys_print("one") Newline _ : sys_print("other") Newline Dedent ...
```

`parse_match_arms()` 消费外层 Indent 后，`parse_match_arm()` 解析 `0 :`，然后调用 `parse_block()`：
- `parse_block()` 解析 `sys_print("zero")` → OK
- `parse_block()` 消费 Newline，看到 `1`（Int），不是 Dedent → 继续循环
- `parse_block()` 把 `1` 当语句解析 → 然后遇到 `:` → 报错 "意外token: Colon"

## 修复方案

### 修改文件

- [src/parser/stmt.rs](file:///run/media/max4075/DOTNET/VX/VX-ToolChains/src/parser/stmt.rs) — `parse_match_arm()` 方法（第 403-408 行）

### 具体改动

将 `parse_match_arm()` 中的 `self.parse_block()?` 替换为手动处理两种情况：

```rust
fn parse_match_arm(&mut self) -> Result<(Box<Expr>, Vec<Box<Stmt>>)>, VXError> {
    let pattern = self.parse_expression()?;
    self.expect(TokenType::Colon, Some("期望分支模式后的 ':'"))?;
    self.skip_newlines();
    let mut body = vec![];
    if self.current().kind == TokenType::Indent {
        // 多行分支体：有自己的 Indent/Dedent 块
        self.advance(); // 消费 Indent
        while !self.match_kind(&[TokenType::Dedent, TokenType::EOF]) {
            self.skip_newlines();
            if self.match_kind(&[TokenType::Dedent, TokenType::EOF]) {
                break;
            }
            body.push(Box::new(self.parse_statement()?));
        }
        if self.current().kind == TokenType::Dedent {
            self.advance(); // 消费分支体自己的 Dedent
        }
    } else {
        // 单行分支体：同一行或下一行的单条语句
        body.push(Box::new(self.parse_statement()?));
    }
    Ok((Box::new(pattern), body))
}
```

**关键逻辑**：
- `skip_newlines()` 后检查 `Indent`：如果有，说明是多行分支体（有自己独立的 Indent/Dedent 对），消费后按语句循环解析直到 Dedent，然后消费该 Dedent
- 如果没有 `Indent`：说明是单行分支体，直接调用 `parse_statement()` 解析一条语句。`parse_statement()` 开头的 `skip_newlines()` 会处理行尾 Newline

## 验证

```bash
cd /run/media/max4075/DOTNET/VX/VX-ToolChains
cargo test
```

预期结果：
- `test_match_int_with_default` — 通过
- `test_match_bool` — 通过
- `test_match_enum_variant` — 通过
- `test_generic_struct_pair_int` — 通过
- `test_generic_class_box_int` — 通过
- `test_generic_nested_instantiation` — 通过
- `test_generic_wrong_arg_count_errors` — 通过
- `test_compile_stmt_unknown_node_errors` — 通过
- 所有现有集成测试无回归
