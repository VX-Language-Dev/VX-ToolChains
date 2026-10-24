// vmake-lsp — VMakeSet.vmake 语言服务器（LSP over stdio），C++23。
// 当前为骨架：仅响应 initialize / shutdown / exit，后续接入 vmake_core 解析与诊断。
#include <cstdio>
#include <string>

int main() {
  std::fprintf(stderr, "vmake-lsp: skeleton (not implemented)\n");
  return 0;
}