# VXC 彻底重写 — Cargo 风格彩色输出 + 功能性错误修复

## 背景

当前 VXC 存在两类问题：
1. **输出问题**：`go/cmd/vxc/main.go` 使用 plain `fmt.Printf`/`fmt.Fprintln`，无颜色、无 Cargo 风格前缀
2. **功能性错误**：编译器指令映射缺失、命令行参数冲突、输出文件路径问题等

## 已识别的功能性错误

### Bug 1: `compiler.go` `compileInstruction` 缺失大量 opcode 映射

当前 `switch` 语句只处理了约 20 个 `typeir.Opcode`，但 `typeir.Opcode` 枚举有 50+ 个值。缺失的包括：
- `I64Add`, `I64Sub`, `I64Mul`, `I64Div`
- `I32Neq`, `I32Lt`, `I32Gt`, `I32Le`, `I32Ge`
- `F64Neq`, `F64Lt`, `F64Gt`, `F64Le`, `F64Ge`
- `I32Neg`, `F64Neg`, `StrEq`, `NeqInt`
- `I32And`, `I32Or`, `BoolAnd`, `BoolOr`
- `ReturnVoid`
- `PropertyGet`, `PointerMember`, `IndexGet`, `IndexSet`
- `FieldLoad`, `FieldStore`, `ArrayNew`, `MapNew`
- `SysArgv`, `System`, `FileRead`, `FileWrite`, `FileExists`
- `StrConcat`, `ArrayConcat`, `Iterate`, `Next`
- `MakeFunction`, `DefineVar`, `LoadTrue`, `LoadFalse`
- `Break`, `Continue`, `BinaryAdd`, `BinarySub`, `BinaryMul`, `BinaryDiv`, `BinaryMod`, `BinaryPow`
- `BinaryEq`, `BinaryNe`, `BinaryLt`, `BinaryGt`, `BinaryLe`, `BinaryGe`
- `MakeStruct`, `MakeClass`, `MakeEnum`, `MakeUnion`, `LoadField`, `StoreField`, `PropertySet`
- `BorrowCheck`, `AliveCheck`, `Halt`, `Import`

**影响**：任何包含这些指令的源码编译时都会返回 `"unsupported opcode"` 错误。

### Bug 2: `FunctionIR` `Body` 字段未设置

`typeir.FunctionIR` 有 `Body` 和 `Instructions` 两个字段（`Instructions` 是 `Body` 的 alias）。`typeirgen.compileFunction` 只设置了 `Instructions`，未设置 `Body`。`serialize.go` 有 workaround 但不够干净。

**影响**：序列化时依赖 workaround，代码可读性和可维护性差。

### Bug 3: `main.go` `-v` 标志冲突

在 VX 源码中 `-v` 表示 verbose（详细模式），但 Go `main.go` 中 `-v` 表示 "打印版本"。这导致用户无法使用 `-v` 开启详细模式。

**修复**：`-v` 应表示 verbose，`--version` 应表示打印版本。

### Bug 4: `main.go` `-h` 退出码错误

```go
if flag.NArg() == 0 {
    flag.Usage()
    os.Exit(1)  // 应为 0
}
```

**修复**：退出码应为 0。

### Bug 5: `writeFile` 不创建父目录

如果输出路径包含不存在的目录（如 `out/foo.vxobj`），`os.WriteFile` 会失败。

**修复**：在写入前调用 `os.MkdirAll` 创建父目录。

### Bug 6: `compiler.go` `hasReturn` 逻辑不检查 `ReturnVoid`

```go
hasReturn := len(instructions) > 0
if hasReturn {
    lastOp := instructions[len(instructions)-1].Op
    hasReturn = lastOp == opcode.Return  // 不检查 ReturnVoid
}
```

**修复**：应同时检查 `opcode.ReturnVoid`。

## 现有参数清单（必须保持不变）

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-o <file>` | 输出 VXOBJ 文件 | `out.vxobj` |
| `--target <triple>` | 目标 triple | `x86_64-linux-gnu` |
| `--version` | 打印版本 | `false` |
| `-v` | 详细模式 | `false` |
| `-no-ownership` | 跳过所有权检查 | `false` |
| `-h, --help` | 显示帮助 | — |

## 设计决策

### 1. 新建 `go/internal/vxcout/` 输出格式化包

- 不修改现有 `logger` 包
- 新建专用包 `vxcout`，提供 Cargo 风格的编译器输出
- 核心函数：
  - `PrintStage(name, msg)` — 编译阶段输出 `[COMPILING]` 前缀
  - `PrintSuccess(msg)` — 绿色 `[FINISHED]` 前缀
  - `PrintError(msg)` — 红色 `[ERROR]` 前缀
  - `PrintWarning(msg)` — 黄色 `[WARNING]` 前缀
  - `PrintInfo(msg)` — 青色 `[INFO]` 前缀
  - `PrintStep(num, total, msg)` — 进度步骤
  - `PrintHelp()` — 帮助信息（Cargo 风格）
  - `PrintVersion()` — 版本信息
  - `EnableColor/DisableColor` — 颜色开关
  - `SetVerbose(verbose)` — 详细模式开关

### 2. ANSI 颜色方案

| 级别 | 前景色 |
|------|--------|
| Success (FINISHED) | Green `\x1b[32m` |
| Error | Red `\x1b[31m` |
| Warning | Yellow `\x1b[33m` |
| Info/Stage | Cyan `\x1b[36m` |
| Progress | Blue `\x1b[34m` |
| Help/Usage | White `\x1b[37m` |
| Dim/Note | Bright Black `\x1b[90m` |

### 3. Cargo 风格输出格式

```
[COMPILING] source.vx
   Compiling lexer...
   Compiling parser...
   Compiling semantic...
   Compiling ownership...
   Compiling typeirgen...
   Compiling bytecode...
[FINISHED] out.vxobj (12345 bytes)
```

### 4. 重写范围

- **新建**: `go/internal/vxcout/vxcout.go` — Cargo 风格输出包
- **重写**: `go/cmd/vxc/main.go` — 使用 `vxcout` + 修复命令行参数
- **修复**: `go/internal/compiler/compiler.go` — 补全所有 opcode 映射
- **修复**: `go/internal/typeirgen/typeirgen.go` — 设置 `Body` 字段
- **不修改**: `go/internal/logger`, `go/internal/bytecode`, `go/internal/importer`, `go/internal/parser`, `go/internal/semantic`, `go/internal/ownership`, `go/internal/lexer`, `go/internal/ast`, `go/internal/token`, `go/internal/typeir`, `go/internal/opcode`, `go/internal/builder`, `go/internal/translator`, `go/internal/llvmir`, `go/internal/vxsetting`, `go/internal/macros`, `go/internal/cache`, `go/internal/parallel_build`, `go/internal/lsp`, `go/internal/vfmt`, `go/internal/stubs`, `go/internal/typeirgen` (除 compileFunction 外)
- **不修改**: `go/go.mod`, `go/go.sum`, `Makefile`, `build.zig`

## 实施步骤

### Step 1: 修复 `go/internal/compiler/compiler.go` — 补全 opcode 映射

在 `compileInstruction` 的 `switch` 语句中添加所有缺失的 `typeir.Opcode` 到 `opcode.OpCode` 的映射：

```go
case typeir.I64Add: return compiler_bytecode.NewInstruction(opcode.AddInt), nil  // 等等，需要正确的 opcode 映射
```

注意：`typeir.Opcode` 和 `opcode.OpCode` 是不同的枚举。`typeir.I32Add` 映射到 `opcode.AddInt`，`typeir.I64Add` 也映射到 `opcode.AddInt`（因为字节码层面不区分 i32/i64 加法）。需要仔细核对 `opcode.go` 中的 opcode 定义和 `typeir.go` 中的 opcode 定义之间的对应关系。

**关键映射关系**（从 `opcode.go` 和 `typeir.go` 对比）：
- `typeir.I32Add` → `opcode.AddInt` (0x40)
- `typeir.I64Add` → `opcode.AddInt` (0x40) — 字节码层面统一
- `typeir.F64Add` → `opcode.AddFloat` (0x41)
- `typeir.I32Sub` → `opcode.SubInt` (0x42)
- `typeir.I64Sub` → `opcode.SubInt` (0x42)
- `typeir.F64Sub` → `opcode.SubFloat` (0x43)
- `typeir.I32Mul` → `opcode.MulInt` (0x44)
- `typeir.I64Mul` → `opcode.MulInt` (0x44)
- `typeir.F64Mul` → `opcode.MulFloat` (0x45)
- `typeir.I32Div` → `opcode.DivInt` (0x46)
- `typeir.I64Div` → `opcode.DivInt` (0x46)
- `typeir.F64Div` → `opcode.DivFloat` (0x47)
- `typeir.I32Mod` → `opcode.ModInt` (0x48)
- `typeir.I32Eq` → `opcode.EqInt` (0x49)
- `typeir.I32Neq` → `opcode.EqInt` + `opcode.Not` 或直接使用 `opcode.EqInt` 的取反
- `typeir.F64Eq` → `opcode.EqFloat` (0x4A)
- `typeir.I32Lt` → `opcode.LtInt` (0x4B)
- `typeir.I32Gt` → `opcode.GtInt` (0x4D)
- `typeir.I32Le` → `opcode.LeInt` (0x4F)
- `typeir.I32Ge` → `opcode.GeInt` (0x51)
- `typeir.F64Lt` → `opcode.LtFloat` (0x4C)
- `typeir.F64Gt` → `opcode.GtFloat` (0x4E)
- `typeir.F64Le` → `opcode.LeFloat` (0x50)
- `typeir.F64Ge` → `opcode.GeFloat` (0x52)
- `typeir.I32Neg` → `opcode.NegInt` (0x53)
- `typeir.F64Neg` → `opcode.NegFloat` (0x54)
- `typeir.BoolNot` → `opcode.Not` (0x55)
- `typeir.I32And` → `opcode.And` (0x56)
- `typeir.I32Or` → `opcode.Or` (0x57)
- `typeir.BoolAnd` → `opcode.And` (0x56)
- `typeir.BoolOr` → `opcode.Or` (0x57)
- `typeir.StrEq` → `opcode.EqInt` — 需要确认
- `typeir.NeqInt` → `opcode.EqInt` + `opcode.Not`
- `typeir.ReturnVoid` → `opcode.Return` — 需要确认
- `typeir.PropertyGet` → `opcode.PropertyGet` (0x2A)
- `typeir.PointerMember` → `opcode.PointerMember` (0x2E)
- `typeir.IndexGet` → `opcode.IndexGet` (0x28)
- `typeir.IndexSet` → `opcode.IndexSet` (0x29)
- `typeir.FieldLoad` → `opcode.LoadField` (0x24)
- `typeir.FieldStore` → `opcode.StoreField` (0x25)
- `typeir.ArrayNew` → `opcode.MakeArray` (0x26)
- `typeir.MapNew` → `opcode.MakeMap` (0x27)
- `typeir.SysArgv` → `opcode.SysArgv` (0x32)
- `typeir.System` → `opcode.System` (0x33)
- `typeir.FileRead` → `opcode.FileRead` (0x34)
- `typeir.FileWrite` → `opcode.FileWrite` (0x35)
- `typeir.FileExists` → `opcode.FileExists` (0x36)
- `typeir.StrConcat` → `opcode.StrConcat` — 需要确认 opcode 中是否有
- `typeir.ArrayConcat` → `opcode.ArrayConcat` — 需要确认 opcode 中是否有
- `typeir.Iterate` → `opcode.Iterate` (0x60)
- `typeir.Next` → `opcode.Next` (0x61)
- `typeir.Halt` → `opcode.Halt` (0x31)
- `typeir.Import` → `opcode.Import` (0x2F)
- `typeir.BorrowCheck` → `opcode.BorrowCheck` (0x3D)
- `typeir.AliveCheck` → `opcode.AliveCheck` (0x3E)
- `typeir.Break` → `opcode.Break` (0x0E)
- `typeir.Continue` → `opcode.Continue` (0x0F)
- `typeir.MakeStruct` → `opcode.MakeStruct` (0x20)
- `typeir.MakeClass` → `opcode.MakeClass` (0x21)
- `typeir.MakeEnum` → `opcode.MakeEnum` (0x22)
- `typeir.MakeUnion` → `opcode.MakeUnion` (0x23)
- `typeir.Newz` → `opcode.Newz` (0x39)
- `typeir.Free` → `opcode.Free` (0x3A)
- `typeir.OwnershipMove` → `opcode.OwnershipMove` (0x3B)
- `typeir.ScopeDrop` → `opcode.ScopeDrop` (0x3C)
- `typeir.Dup` → `opcode.Dup` (0x37)
- `typeir.Pop` → `opcode.Pop` (0x38)
- `typeir.AddressOf` → `opcode.AddressOf` (0x2C)
- `typeir.Deref` → `opcode.Deref` (0x2D)

注意：`opcode.go` 中没有 `StrConcat` 和 `ArrayConcat` 的定义。需要先在 `opcode.go` 中添加这些 opcode。

### Step 2: 修复 `go/internal/typeirgen/typeirgen.go` — 设置 `Body` 字段

在 `compileFunction` 的返回值中同时设置 `Body` 和 `Instructions`：

```go
return typeir.FunctionIR{
    Name:         fn.Name,
    ParamCount:   uint32(len(params)),
    HasReturn:    hasReturn,
    ReturnType:   retType,
    Params:       params,
    Instructions: c.Instructions,
    Body:         c.Instructions,  // 添加这一行
    Locals:       c.Locals,
    LocalTypes:   localTypes,
}
```

### Step 3: 修复 `go/internal/compiler/compiler.go` — `hasReturn` 逻辑

```go
hasReturn := len(instructions) > 0
if hasReturn {
    lastOp := instructions[len(instructions)-1].Op
    hasReturn = lastOp == opcode.Return || lastOp == opcode.ReturnVoid
}
```

### Step 4: 新建 `go/internal/vxcout/vxcout.go`

创建完整的 Cargo 风格输出包。

### Step 5: 重写 `go/cmd/vxc/main.go`

- 修复 `-v`/`--version` 冲突：`-v` 表示 verbose，`--version` 表示打印版本
- 修复 `-h` 退出码为 0
- 修复 `writeFile` 创建父目录
- 使用 `vxcout` 替换所有 `fmt.Printf`/`fmt.Fprintln`/`fmt.Fprintf(os.Stderr, ...)`
- 添加编译阶段进度输出

### Step 6: 编译验证

```bash
cd go && go build -o ../bin/vxc ./cmd/vxc/main.go
cd go && go vet ./...
cd go && go test ./internal/... -v -race
```

### Step 7: 功能测试

```bash
./bin/vxc -h          # 退出码 0
./bin/vxc --version   # 打印版本
./bin/vxc -v -o test.vxobj tests/test.vx  # 详细模式
./bin/vxc -o out/test.vxobj tests/test.vx  # 父目录自动创建
```

## 验证清单

- [ ] `compiler.go` 所有 `typeir.Opcode` 都有对应的 `opcode.OpCode` 映射
- [ ] `typeirgen.go` `FunctionIR.Body` 正确设置
- [ ] `compiler.go` `hasReturn` 正确检查 `ReturnVoid`
- [ ] `-v` 表示 verbose，`--version` 表示打印版本
- [ ] `-h` 退出码为 0
- [ ] `writeFile` 自动创建父目录
- [ ] 输出包含 Cargo 风格前缀和 ANSI 颜色
- [ ] `go build` 编译通过
- [ ] `go vet ./...` 通过
- [ ] `go test ./internal/...` 通过
- [ ] 现有 `go/internal/logger` 包未被修改
- [ ] `go/internal/opcode/opcode.go` 添加了 `StrConcat` 和 `ArrayConcat`（如果需要）

## 风险与缓解

| 风险 | 缓解 |
|------|------|
| `compiler.go` opcode 映射遗漏 | 逐一对比 `typeir.Opcode` 和 `opcode.OpCode` 枚举 |
| `opcode.go` 缺少 `StrConcat`/`ArrayConcat` | 先添加再映射 |
| `vxcout` 包引入循环依赖 | 仅依赖标准库 |
| 颜色输出在非终端环境混乱 | 检测 `isatty` 或允许 `-no-color` |
| `go test` 因输出格式变化失败 | 不修改测试文件；`vxcout` 仅影响 main.go |

## 不在范围内

- 不修改 `go/internal/logger` 包
- 不修改 `go/internal/builder` 包
- 不修改 `go/internal/translator` 包
- 不修改 `go/internal/bytecode` 包
- 不修改 `go/internal/importer` 包
- 不修改 `go/internal/parser` 包
- 不修改 `go/internal/semantic` 包
- 不修改 `go/internal/ownership` 包
- 不修改 `go/internal/lexer` 包
- 不修改 `go/internal/ast` 包
- 不修改 `go/internal/token` 包
- 不修改 `go/internal/typeir` 包（除 `FunctionIR` 结构体外）
- 不修改 `go/internal/vxsetting` 包
- 不修改 `go/internal/macros` 包
- 不修改 `go/internal/cache` 包
- 不修改 `go/internal/parallel_build` 包
- 不修改 `go/internal/lsp` 包
- 不修改 `go/internal/vfmt` 包
- 不修改 `go/internal/stubs` 包
- 不修改 `go/internal/llvmir` 包
- 不修改 `go/go.mod`
- 不修改 `go/go.sum`
- 不修改 `Makefile`
- 不修改 `build.zig`
