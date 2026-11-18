# Pull Request

## 变更内容

<!-- 一句话说明这个 PR 做了什么 -->

## 关联 Issue

<!-- 例如：Closes #123；无关联可写“无” -->

## 变更类型

- [ ] Bug 修复
- [ ] 新功能
- [ ] 重构 / 性能优化
- [ ] 文档 / 示例
- [ ] 构建 / CI

## 验证方式

<!-- 说明你实际运行了什么、结果如何，而不是“应该没问题” -->

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build
ctest --test-dir build --output-on-failure
bash ci/check.sh
```

- [ ] 本地 `ctest` 全部通过
- [ ] 本地 `bash ci/check.sh` 通过
- [ ] 新增/修改行为已补充测试
- [ ] 涉及 CLI 参数、帮助文本时已同步更新 README 与 `--help`

## 其他说明

<!-- 设计取舍、已知限制、需要维护者注意的点 -->
