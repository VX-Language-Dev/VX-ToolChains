/* sys 库实现 — 执行 shell 命令，返回 libc system() 的退出状态。
 * 导出 VX ABI 符号：_vx_sys_system。 */
#include <stdint.h>
#include <stdlib.h>

int32_t _vx_sys_system(const char *cmd) {
  if (!cmd) return -1;
  return (int32_t)system(cmd);
}
