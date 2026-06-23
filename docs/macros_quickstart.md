# VX 宏系统快速入门

## 5分钟上手宏系统

### 1. 定义你的第一个宏

```vx
// 创建一个简单的问候宏
macro greet(name) {
    out("Hello, " + name + "!")
}
```

### 2. 使用宏

```vx
func main() {
    #greet("VX Developer")  // 输出: Hello, VX Developer!
}
```

### 3. 带多个参数的宏

```vx
macro add_and_print(a, b) {
    var result = a + b
    out(a + " + " + b + " = " + result)
}

func main() {
    #add_and_print(5, 3)  // 输出: 5 + 3 = 8
}
```

### 4. 代码生成宏

```vx
// 重复执行某段代码
macro repeat_3_times(body) {
    var i = 0
    while i < 3 {
        body
        i = i + 1
    }
}

func main() {
    #repeat_3_times(out("Repeated!"))
    // 输出三次 "Repeated!"
}
```

## 核心概念

### 宏 vs 函数

| 特性 | 宏 | 函数 |
|------|-----|------|
| 执行时机 | 编译时 | 运行时 |
| 参数传递 | 文本替换 | 值传递 |
| 性能 | 零开销（展开后无调用） | 有调用开销 |
| 灵活性 | 可生成任意代码 | 固定逻辑 |

### 缓存机制

宏系统会自动缓存相同参数的展开结果：

```vx
#debug_print("x", 42)  // 第一次：展开并缓存
#debug_print("x", 42)  // 第二次：从缓存获取（更快！）
#debug_print("x", 100) // 不同参数：重新展开
```

## 常见用例

### 1. 调试助手

```vx
macro debug_var(name, value) {
    out("[DEBUG] " + name + " = " + value)
}

macro trace(message) {
    out("[TRACE] " + message)
}
```

### 2. 断言和验证

```vx
macro assert_positive(value) {
    if value <= 0 {
        out("Error: Expected positive value")
    }
}
```

### 3. 模板代码

```vx
macro create_getter(field_name, field_type) {
    func get_{{field_name}}(): {{field_type}} {
        return self.{{field_name}}
    }
}
```

## 最佳实践

✅ **推荐做法**：
- 为宏添加清晰的注释
- 使用描述性的宏名称
- 保持宏体简洁
- 测试宏的各种用法

❌ **避免做法**：
- 过深的宏嵌套
- 宏中产生副作用
- 过度使用宏替代函数

## 下一步

- 📖 阅读 [完整文档](macros.md) 了解更多高级功能
- 💻 查看 [示例代码](../examples/macro_demo.vx)
- 🔧 尝试创建自己的实用宏库

## 获取帮助

遇到问题？查看：
- 编译器错误信息（包含行号和列号）
- [常见问题](../docs/faq.md)
- [项目README](../README.md)
