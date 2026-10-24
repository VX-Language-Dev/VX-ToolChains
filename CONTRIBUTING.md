# 贡献指南

感谢你对 VX-ToolKit 的关注！本文档说明如何参与项目贡献。

## 行为准则

- 尊重所有参与者，保持友善和建设性的沟通
- 专注于技术讨论，避免人身攻击或无关话题
- 使用中文或英文进行交流

## 如何贡献

### 报告问题

如果你发现 bug 或有功能建议，请通过 Gitee Issue 提交：

1. 检查是否已有重复 issue
2. 使用清晰的标题描述问题
3. 提供最小可复现示例（VX 源码片段、命令行步骤）
4. 说明预期行为和实际行为
5. 附上环境信息：操作系统、C++编译器版本、VX-ToolKit 版本/提交 hash

### 提交代码

1. **Fork 仓库** 并从 `master` 分支创建特性分支
2. **保持提交历史清晰**：每个 commit 聚焦一个逻辑变更
3. **编写测试**：新增功能请补充单元测试或集成测试
4. **确保格式正确**：`clang-format -i src/ include/ tests/` 不应有格式问题
5. **运行测试套件**：

```bash
make test               # 运行全部单元测试
make build              # Debug 构建验证（带调试信息）
make lint               # 代码质量检查
make check              # 本地 CI 全套检查
```

6. **提交 Pull Request** 到 `master` 分支，并关联相关 issue

## 代码风格

- 使用 `clang-format` 自动格式化代码（4 空格缩进，基于 LLVM 风格）
- 模块职责单一，公开 API 集中在 `include/` 目录中
- 使用 C++ 惯用的命名风格（PascalCase 用于类型，camelCase 用于函数/变量）
- 中文注释和文档与英文技术术语混用时保持一致性
- 优先使用 `std::expected` / 错误码处理可恢复错误，避免 `throw` 用于控制流
- 使用 RAII 管理资源，避免裸指针

## 项目结构

| 目录/文件     | 说明                         |
| ------------- | ---------------------------- |
| `src/`        | C++ 源码（util / compiler / backend / driver） |
| `include/`    | 公开头文件                   |
| `tests/`      | 测试代码（CTest 15 组）      |
| `std/`        | VX 标准库（.vx 源文件）      |
| `compiler/`   | VX 编译器前端源码（.vx 源文件） |
| `headers/`    | @imp/@cppimp 导入库（.vxh + .c/.cpp） |
| `tools/`      | 独立工具子项目（vmake、vxls、vx-tidy） |
| `tree-sitter-vx/` | Tree-sitter 语法（编辑器高亮） |
| `cpp_lsp/`    | 早期独立 LSP 原型（已由 tools/vxls 取代） |
| `docs/`       | 语言与格式文档               |
| `Makefile`    | Make 构建配置                |
| `CMakeLists.txt` | CMake 构建配置            |
| `ci/`         | CI 检查脚本                  |

## 分支策略

| 分支       | 用途                         |
| ---------- | ---------------------------- |
| `master` | 稳定主线，可发布版本         |
| `Beta`   | 集成测试分支，用于预发布验证 |

请向 `master` 提交 PR；维护者会根据需要合并到 `Beta` 进行回归测试。

## 版本号规则

采用语义化版本（SemVer）：

- `MAJOR`：不兼容的语言或 ABI 变更
- `MINOR`：向后兼容的新功能
- `PATCH`：bug 修复和小幅改进

## 许可证

贡献即表示你同意将代码以 AGPL-3.0 许可证授权发布。