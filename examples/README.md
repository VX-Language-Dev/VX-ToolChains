# 示例程序

每个示例都是完整的可编译程序，覆盖一项常用能力。全部示例在仓库根目录执行，
以便 `vxc` 默认找到根目录的 `headers/` 导入库。

## 一键运行

```bash
# 先构建工具链
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build

# 编译、链接并运行全部示例
bash examples/run.sh
```

## 手动运行单个示例

```bash
./build/vxc examples/03_class.vx -o build/examples/03_class.vxobj
./build/vlnk build/examples/03_class.vxobj -o build/examples/03_class
./build/examples/03_class
```

## 示例列表

| 文件 | 演示内容 | 预期输出 |
| ---- | -------- | -------- |
| `01_hello.vx` | 最小程序、内置 `out` / `out_int` | `Hello, VX!` / `3` |
| `02_fibonacci.vx` | 函数、递归、`while` 循环 | `0 1 1 2 3 5 8 13 21 34` |
| `03_class.vx` | 类、构造器 `new`、方法 | `11` / `12` |
| `04_imports.vx` | `@imp` 导入、`as` 别名、子目录库 | `5` / `42` / `55` |
| `05_strings.vx` | `text` 库字符串查找/截取/trim/转换 | `6` / `cde` / `hi` / `-42` |
| `06_cppimp.vx` | `@cppimp` 接入 C++ 头文件 | `Hello, VX!` |
| `07_threads.vx` | `proc` 库线程 `spawn2` / `join_int` | `42` |

## 说明

- 这些示例只使用当前工具链**已经支持**的语法；`docs/SYNTAX.md` 是语法的完整参考。
- `05_strings.vx` 中返回 `string` 的函数由运行时分配内存，示例里显式 `free`。
- 需要自定义导入库目录时，给 `vxc` 传 `-I <dir>`（可多次）。
