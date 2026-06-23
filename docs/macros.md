# VX 宏系统

## 概述

VX宏系统是一个编译时宏展开机制，允许开发者在编译阶段生成代码，提高代码复用性和开发效率。宏系统在词法分析、语法分析之后，所有权检查和字节码生成之前执行。

## 特性

- ✅ **编译时展开**：宏在编译阶段完全展开，无运行时开销
- ✅ **参数化宏**：支持带参数的宏定义，实现灵活的代码生成
- ✅ **智能缓存**：基于参数签名的缓存机制，避免重复展开相同宏调用
- ✅ **递归展开**：支持嵌套宏和递归宏调用
- ✅ **错误提示**：清晰的错误信息，包括参数数量不匹配和未定义宏

## 语法

### 宏定义

```vx
macro 宏名(参数1, 参数2, ...) {
    // 宏体
}
```

### 宏调用

```vx
#宏名(实参1, 实参2, ...)
```

## 使用示例

### 1. 简单的调试宏

```vx
// 定义调试打印宏
macro debug_print(var_name, value) {
    out("DEBUG: " + var_name + " = " + value)
}

// 使用宏
func main() {
    var x = 42
    #debug_print("x", x)  // 展开为: out("DEBUG: x = 42")
}
```

### 2. 代码重复宏

```vx
// 定义重复执行宏
macro repeat(n, body) {
    var i = 0
    while i < n {
        body
        i = i + 1
    }
}

// 使用宏
func main() {
    #repeat(5, out("Hello"))  // 输出5次"Hello"
}
```

### 3. 条件执行宏

```vx
// 定义条件执行宏
macro when(condition, action) {
    if condition {
        action
    }
}

// 使用宏
func main() {
    var flag = true
    #when(flag, out("Condition is true!"))
}
```

### 4. 复杂宏：日志系统

```vx
// 定义日志级别宏
macro log_info(message) {
    out("[INFO] " + message)
}

macro log_error(message) {
    out("[ERROR] " + message)
}

macro log_debug(message) {
    out("[DEBUG] " + message)
}

// 使用宏
func main() {
    #log_info("Application started")
    #log_error("Something went wrong")
    #log_debug("Variable x = 42")
}
```

## 标准库宏

VX 标准库已经内置一组常用宏，位于 `std::macros`，并通过 `prelude` 自动注入，无需手动定义即可使用：

```vx
import std

func main():
    var x = 42
    #assert(x > 0, "x 必须为正数")
    #debug_print("x", x)
    #log_info("服务启动完成")
    #repeat(3, out("Hello"))
```

可用宏列表参见 [标准库文档](../std/README.md)。

## 性能优化

### 宏缓存机制

宏系统实现了智能缓存机制：
- 相同的宏调用（相同参数）只展开一次
- 缓存基于参数的结构化签名
- 显著减少编译时间，特别是对于频繁使用的宏

### 统计信息

可以通过编译器API获取宏系统的统计信息：

```rust
let (expand_count, cache_hit_count, hit_rate) = compiler.get_macro_stats();
println!("宏展开次数: {}", expand_count);
println!("缓存命中次数: {}", cache_hit_count);
println!("缓存命中率: {:.2}%", hit_rate);
```

## 实现细节

### 架构组件

1. **词法分析器** (`token.rs`)
   - 新增 `Macro` 关键字Token
   - 新增 `Hash` (#) Token用于宏调用

2. **语法分析器** (`parser/stmt.rs`)
   - `parse_macro_def()`: 解析宏定义
   - `parse_macro_call_stmt()`: 解析宏调用语句

3. **AST节点** (`parser/ast.rs`)
   - `MacroDef`: 宏定义节点
   - `MacroCall`: 宏调用节点

4. **宏注册表** (`macros.rs`)
   - `MacroRegistry`: 管理宏的注册和查找
   - `Macro`: 宏定义结构体
   - 参数绑定和替换逻辑

5. **编译器集成** (`compiler_core.rs`)
   - `expand_macros()`: 在编译前展开所有宏
   - `process_expr_for_macros()`: 递归处理嵌套表达式中的宏

### 展开流程

```
源代码 
  ↓ [词法分析]
Token流
  ↓ [语法分析]
AST (包含MacroDef和MacroCall节点)
  ↓ [宏展开]
展开后的AST (无宏节点)
  ↓ [所有权检查]
类型化IR
  ↓ [字节码生成]
VXOBJ文件
```

## 限制和注意事项

1. **宏不是函数**：宏是编译时代码替换，不支持运行时调用
2. **参数传递**：宏参数通过文本替换传递，注意副作用
3. **作用域**：宏在定义处展开，遵循VX的作用域规则
4. **递归深度**：避免过深的宏递归，可能导致编译栈溢出

## 最佳实践

1. **命名规范**：宏名使用小写加下划线，如 `debug_print`
2. **简洁性**：保持宏体简洁，复杂逻辑拆分为多个宏
3. **文档化**：为宏添加注释说明用途和参数
4. **测试**：为重要宏编写测试用例

## 未来计划

- [ ] 支持可变参数宏 (`macro varargs(name, ...)` )
- [ ] 宏 hygiene（卫生宏，避免变量捕获）
- [ ] 编译期计算宏
- [ ] 条件编译宏 (`#ifdef`, `#ifndef`)

## 参考资料

- [VX语言规范](../README.md)
- [编译器架构](../docs/compiler_architecture.md)
- [标准库文档](../std/README.md)
