# 代码布局与大文件拆分现状

> 状态：**已执行**（parser / semantics / backend emitter / vxls analysis 已拆分；codegen 待拆分）
> 适用范围：`src/` 与 `include/`（`tools/` 另附）
> 快照：2026-11-15 · vx 1.21.1（行数为该快照下的参考值，会随代码演进变化）

本文档记录 `src/` 下大文件的实际拆分结果、所采用的约定，以及尚未完成的收尾项。
它不是一次性计划书：**新增/修改拆分时请同步更新本文件**。

---

## 1. 当前布局

### 1.1 编译核心（`src/compiler/`）

| 入口文件 | 行数（约） | 内部实现头 | 职责 |
|----------|-----------|------------|------|
| `parser.cpp` | 95 | `parser_util.hpp` / `parser_decl.hpp` / `parser_stmt.hpp` / `parser_expr.hpp` | 语法分析：辅助函数、`Parser` 声明、语句解析、表达式解析 |
| `semantics.cpp` | 132 | `semantics_builtin.hpp` / `semantics_types.hpp` / `semantics_checker.hpp` / `semantics_checker_pass.hpp` / `semantics_checker_stmt.hpp` / `semantics_checker_expr.hpp` | 语义分析：内置表、类型关系、`TypeChecker` 声明、声明收集、语句检查、表达式检查 |
| `codegen.cpp` | 1343 | —（**未拆分**） | AST → TypeIR |
| `lexer.cpp` | 702 | — | 词法（缩进 / INDENT / DEDENT） |
| `importer.cpp` | 621 | — | `@imp` / `@cppimp` / `import` 解析与头文件探测 |
| `diag.cpp` / `vxabi.cpp` / `vxobj.cpp` | 260 / 61 / 265 | — | 诊断、ABI 命名、VXOBJ 序列化 |

### 1.2 后端（`src/backend/`）

| 入口文件 | 行数（约） | 内部实现头 | 职责 |
|----------|-----------|------------|------|
| `llvm_emitter.cpp` | 23 | `emitter_decl.hpp` / `emitter_value.hpp` / `emitter_text.hpp` / `emitter_inst.hpp` / `emitter_call.hpp` / `emitter_func.hpp` / `emitter_scaffold.hpp` | `Emitter` 声明、值/符号表、文本工具、指令发射、调用发射、函数/入口发射、运行时脚手架 |
| `link_driver.cpp` | 874 | — | `llc` + `cc/c++` + `lld`（缺失时回退 clang） |
| `vxobj_reader.cpp` | 318 | — | VXOBJ → `ModuleIR` |

### 1.3 LSP / 静态分析（已拆分为独立仓库）

`tools/vxls/`（LSP 服务器）与 `tools/vx-tidy/`（静态分析引擎）已拆分为
独立仓库 `dry` 与 `drut`，不再包含在本仓库；其内部拆分记录见各自仓库。

---

## 2. 拆分约定（实际采用的方案）

与最初「拆成多个 `.cpp`」的方案不同，实际落地采用**内部实现头 + 单一入口 `.cpp`**：

1. **实现头放在 `src/<layer>/` 下**，命名 `<name>_<phase>.hpp`，文件头注明「内部实现头，仅供 `<entry>.cpp` 使用」。
2. **只被对应的一个 `.cpp` 包含**，内容位于**匿名 namespace** 内，保持内部链接。
3. **不进入 `include/`，不参与 `install()`**；`include/vx/...` 的公开 API 与签名不变。
4. **编译单元数量不变**：仍然是一个 `.cpp` 一个 TU，因此不引入跨 TU 的类可见性问题，也无需改 `CMakeLists.txt`。
5. 纯辅助函数写成 `inline`，类声明集中在 `<name>_decl.hpp` 或 `<name>_checker.hpp`。

### 关键约束

实现头**禁止被第二个 TU 包含**。否则每个 TU 会各得到一份匿名 namespace 下的类定义，
轻则代码膨胀，重则出现重复符号或「同名不同类型」的链接/ODR 问题。

若将来确实需要多 TU 共享，应改为：把类声明提升到非匿名的 `vx::<layer>` 命名空间
（`<name>_detail.hpp`），实现拆到多个 `.cpp` 并在 `CMakeLists.txt` 中登记。

---

## 3. 观察名单（未达阈值但可继续拆分）

| 文件 | 行数（约） | 备注 |
|------|-----------|------|
| `src/compiler/codegen.cpp` | 1343 | **超过 1000 行阈值**，下一批优先项：表达式 / 语句 / 函数注册三段 |
| `src/backend/link_driver.cpp` | 874 | 匿名 namespace 与 `vx::backend` 各有一份 `find_lld()`，可先去重 |
| `src/backend/emitter_inst.hpp` | 813 | 单条 `emit_inst()` 的 opcode switch，超过 800 行目标；如需可再分子方法 |
| `src/compiler/parser_stmt.hpp` | 657 | 语句族解析；接近观察阈值 |
| `src/compiler/importer.cpp` | 621 | 模块解析 + 头文件探测 + 系统 include 三段 |
| `src/compiler/lexer.cpp` | 702 | `class Lexer` 职责单一，暂不动 |

---

## 4. 剩余工作

1. **拆分 `codegen.cpp`**（唯一超过 1000 行的 `src/` 文件）。建议沿用现有约定：
   `codegen_decl.hpp`（类声明）/ `codegen_expr.hpp` / `codegen_stmt.hpp` / `codegen_func.hpp`。
2. **去重 `link_driver.cpp` 的 `find_lld()`**（两处实现完全相同）。
3. 视需要把 `emitter_inst.hpp` 的 opcode switch 按族拆成子方法。
4. 每次拆分后更新本文档的布局表与「快照」日期。

---

## 5. 验证

```bash
# 1) 构建
cmake --build build

# 2) 单元 / 集成测试（20 组，CTest 统一调度）
ctest --test-dir build --output-on-failure

# 3) 格式门禁（必须零违规）
cmake --build build --target check-format

# 4) 本地 CI 全套：多配置构建 + 格式 + clang-tidy 基线 + 全部测试
bash ci/check.sh
```

**产物一致性验证（关键）**：拆分前后 LLVM IR 文本必须逐字节相同。

```bash
./build/vxc hello.vx && ./build/vlnk hello.vxobj --emit-ir > /tmp/ir_baseline.ll
# 拆分后重跑并比对：
./build/vxc hello.vx && ./build/vlnk hello.vxobj --emit-ir > /tmp/ir_after.ll
diff /tmp/ir_baseline.ll /tmp/ir_after.ll
```

建议对每个阶段都跑一次；候选样例取自 `tests/` 的 e2e fixture 与 `std/` 中的 `.vx`
（覆盖 class、`@imp`、任务/线程/管道等 helper 发射路径）。

**静态检查**：`clang-tidy` 告警数不得超过基线 250（见 `ci/check.sh` 的
`TIDY_MAX_WARNINGS`）。搬移代码本身不新增告警；若出现 `unused-function`，
说明有 `static` 辅助函数被放进了头文件，应改为 `inline`。

---

## 6. 完成判据

| 判据 | 当前状态 |
|------|----------|
| `src/`、`include/` 下无超过 1000 行的文件 | ❌ `codegen.cpp` 1343 行 |
| 拆分后的单元不超过 800 行 | ⚠️ `emitter_inst.hpp` 813 行 |
| 每个新文件职责可用一句话说清 | ✅ |
| `ctest` 全绿 | ✅ 20/20 |
| `bash ci/check.sh` 全绿 | 需本地复跑 |
| 拆分前后 `vlnk --emit-ir` 逐字节一致 | 按阶段验证 |
| 公开头文件与测试代码零改动 | 拆分阶段遵守 |

---

## 7. 不在本次范围

- `tools/` 其余子项目（`vmake/src/build.cpp`、`vmake-lsp/src/main.cpp` 等）
- `tests/` 下的测试源码
- `std/` 标准库设计稿
