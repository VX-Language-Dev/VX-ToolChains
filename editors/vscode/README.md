# VX Language for VS Code

VS Code extension for the VX programming language, providing syntax highlighting and LSP integration.

## Features

- Syntax highlighting for `.vx` files
- Bracket/comment configuration
- LSP client that connects to the official `vx-lsp` language server
  - Diagnostics
  - Hover information
  - Go to definition
  - Document symbols
  - Workspace symbols
  - Completion

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
