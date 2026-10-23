/* text 库实现 — 导出 VX ABI 符号：_vx_text_length / _vx_text_is_empty /
 * _vx_text_shout。覆盖 string 入参、int/bool 返回与 void 返回三种形态。 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int32_t _vx_text_length(const char *s) {
  return s ? (int32_t)strlen(s) : 0;
}

int32_t _vx_text_is_empty(const char *s) { return (s == NULL || *s == '\0'); }

void _vx_text_shout(const char *s) {
  if (!s) return;
  for (const char *p = s; *p; ++p) putchar(*p);
  putchar('\n');
}
