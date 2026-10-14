# tree-sitter-vx

为 [VX 语言](..)（adVanced eXecutable language）编写的 [Tree-Sitter](https://tree-sitter.github.io/) 语法，可用于编辑器语法高亮、代码导航与 [LSP](https://microsoft.github.io/language-server-protocol/) 语义分析（如 vxls）。

语法规则依据 VX-ToolChains 参考实现制定：

- `src/compiler/parser.cpp` —— C++ 参考解析器
- `compiler/parser.vx` —— VX 自举解析器
- `src/compiler/lexer.cpp` —— 缩进规则（INDENT/DEDENT）与词法

## 语法特性

| 特性 | 说明 |
|------|------|
| 缩进块 | Python 风格 INDENT/DEDENT，由外部扫描器 `src/scanner.c` 产出 |
| 注释 | `#` 行注释；空行与整行注释不参与缩进比较 |
| 块头 | `func` / `if` / `while` / `for` / `class` / `struct` / `enum` 后必须有 `:` |
| 同行块 | `:` 后可跟同行语句，用 `;` 分隔 |
| 软关键字 | `loop` / `match` / `enum` / `union` / `macro` / `move` 在表达式位置回退为标识符 |
| 类型标注 | 支持泛型 `Vec<int>` 与点分类型 `vec.Vec`；`var` / `int` / `float` / `bool` / `void` 为内建类型 |
| 花括号宏 | `macro name(params) { ... }`，宏体内 `if` / `while` / `for` 使用花括号块 |
| 字符串 | 单双引号、`\n` `\t` `\\` `\'` `\"` 转义 |

## 使用

```bash
# 生成解析器
tree-sitter generate

# 运行语料库测试
tree-sitter test

# 解析 VX 源码查看语法树
tree-sitter parse path/to/file.vx
```

## 目录结构

```
tree-sitter-vx/
├── grammar.js          # 语法定义（DSL）
├── src/scanner.c       # 外部扫描器：_newline / _indent / _dedent
├── src/parser.c        # 生成的解析器（tree-sitter generate 产物）
├── queries/highlights.scm  # 语法高亮查询
└── test/corpus/vx.txt  # 语料库测试（18 组用例）
```

## 缩进处理设计

与 tree-sitter-python 相同的方案：

1. 所有空白（含换行）与 `#` 注释作为 `extras`，在任意 token 之间消费；
2. 外部扫描器仅在语法接受 `_newline` / `_indent` / `_dedent` 的位置被调用：
   - `_newline`：语句边界（扫描器跨过空行与注释行后再判定）；
   - `_indent` / `_dedent`：与缩进栈顶比较后产出，可连续多次 dedent；
3. 括号 `(` `[` `{` 内部语法不接受任何外部 token，因此换行自然被
   extras 消费，与参考实现"括号内续行不产生 INDENT"的规则一致；
4. 三个 token 均为零宽，被扫描器跳过的空白随后由内部 lexer 重新扫描。

## 状态

- 语料库测试 18/18 通过
- VX-ToolChains 全部真实源码（`std/` 54 个 + `compiler/` 7 个 `.vx` 文件）100% 无 ERROR 解析
  （`tests/*.decompiled.vx` 为编译器输出工件，使用 `//` 注释，不属于 VX 源码语法）
