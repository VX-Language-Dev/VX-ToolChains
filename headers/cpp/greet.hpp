// greet.hpp — @cppimp 示例：普通 C++ 头文件（C++ ABI，无 extern "C"）。
// vlnk 在链接期自动生成 shim，把 C++ 符号 greet 包装为 VX ABI 符号
// _vx_greet_greet（见同目录伴生声明 greet.vxh）。
#pragma once

const char *greet(const char *name);
