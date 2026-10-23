// greet.cpp — @cppimp 示例实现（C++ 符号 greet，非 VX ABI）。
// vlnk 生成的 shim 调用本函数；字符串用静态缓冲返回以便 const char* 可用。
#include "greet.hpp"

#include <string>

const char *greet(const char *name) {
  static std::string msg;
  msg = std::string("Hello, ") + (name ? name : "") + "!";
  return msg.c_str();
}
