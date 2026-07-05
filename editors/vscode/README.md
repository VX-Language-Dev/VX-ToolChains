# VX Language for VS Code

VS Code extension for the VX programming language, providing syntax highlighting and LSP integration.

## Features

### Syntax & Formatting
- Syntax highlighting for `.vx` files
- Bracket/comment configuration

### LSP Server (`vx-lsp`) Features

#### Completion (自动补全)
- **Function signature snippets**: 参数占位符，Tab 切换
- **Struct/Class `new` templates**: `new Type(...)` 模板
- **Member access completion**: `.` 和 `->` 触发成员（字段/方法）补全
- **Import path completion**: `import` 语句模块路径补全
- **Scope-based sorting**: 局部变量优先于全局符号

#### Hover (悬停提示)
- **Precise token highlighting**: 精确的 token 范围高亮
- **Struct/Class member hover**: 字段与方法的类型信息
- **Function call signature**: 调用点显示函数签名
- **Builtin functions**: `out`, `sys_argv`, `len`, `panic` 内置函数说明

#### Go to Definition (跳转定义)
- **Scope-aware lookup**: 作用域内最近定义优先
- **Supports**: 函数参数、局部变量、`import` 别名、方法内精确跳转

#### Symbols (符号导航)
- Document symbols: 结构化大纲视图
- Workspace symbols: 全局符号搜索

## Prerequisites

- VS Code 1.74 or newer
- The `vx-lsp` binary built from the Rust toolchain in this repository

## Build the Language Server

From the repository root (one level above this folder):

```bash
cargo build --bin vx-lsp --release
```

The extension looks for the server at `target/release/vx-lsp` relative to the workspace root by default.

## Build the Extension

```bash
cd editors/vscode
npm install
npm run compile
```

## Run/Debug in VS Code

1. Open `editors/vscode` in VS Code.
2. Press `F5` to launch a new Extension Development Host window.
3. Open any `.vx` file to test highlighting and LSP features.

## Configuration

| Setting | Description |
|---------|-------------|
| `vx.languageServer.path` | Absolute path to the `vx-lsp` executable. If empty, the extension searches `target/release/vx-lsp` and `target/debug/vx-lsp` in the workspace root. |
| `vx.languageServer.trace` | When `true`, logs LSP client/server communication to the **VX Language** output channel. |

## Packaging

```bash
npm install -g @vscode/vsce
vsce package
```

This produces a `.vsix` file that can be installed manually.

## License

AGPL-3.0
