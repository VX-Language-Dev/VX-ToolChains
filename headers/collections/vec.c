/* collections/vec 库实现 — 导出 _vx_vec_sum_to。 */
#include <stdint.h>

int32_t _vx_vec_sum_to(int32_t n) {
  int32_t s = 0;
  for (int32_t i = 1; i <= n; ++i) s += i;
  return s;
}
