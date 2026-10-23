/* math 库实现 — 导出 VX ABI 符号（_vx_<lib>_<name>）。
 * 与 headers/math.vxh 的声明一一对应，由 vlnk 在链接期并入。 */
#include <stdint.h>

int32_t _vx_math_add(int32_t a, int32_t b) { return a + b; }
int32_t _vx_math_mul(int32_t a, int32_t b) { return a * b; }
