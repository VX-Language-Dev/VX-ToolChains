# VX Linker (Rust Version)

## 概述
这是 VX 语言链接器的 Rust 实现，对应原来的 C++ 版本 `vxllk.cpp`。

## 功能
- 读取 `.vxobj` 文件（VXOBJ V1/V2 格式）
- 验证 VXOBJ 头部（Magic + Version）
- 读取 x64 运行时存根（`vx_runtime_x64.exe`）
- 拼接存根和字节码载荷
- 在文件末尾写入载荷大小（8 字节 uint64）

## 构建
```bash
cargo build --bin vxlinker
```

Release 构建：
```bash
cargo build --bin vxlinker --release
```

## 使用方法
```bash
# 基本用法
./vxlinker input.vxobj

# 指定输出文件
./vxlinker input.vxobj -o output.exe

# 指定自定义存根
./vxlinker input.vxobj -s custom_stub.exe -o output.exe
```

## 参数
- `input.vxobj` - 输入的 .vxobj 文件路径（必需）
- `-o <path>` - 指定输出的 exe 路径（默认：与输入同名，扩展名为 .exe）
- `-s <path>` - 指定 x64 运行时存根路径（默认：vx_runtime_x64.exe）

## 与 C++ 版本的区别
1. **错误处理**：使用 Rust 的 `Result` 类型和自定义 `LinkerError` 枚举
2. **内存安全**：自动内存管理，无需手动释放
3. **错误处理**：更安全的错误处理，编译器强制检查
4. **跨平台**：Rust 版本可以更轻松地移植到其他平台

## 测试
```bash
cargo test --bin vxlinker
```

## 文件结构
- `src/vxlinker.rs` - Rust 链接器源码
- `Cargo.toml` - 添加 vxlinker 作为 binary target
