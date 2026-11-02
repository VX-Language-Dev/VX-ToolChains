// str.hpp — 字符串工具：诊断渲染、源码切分等公共辅助函数。
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace vx::util {

// 去除首尾空白（空格、制表符、换行等）
std::string_view trim(std::string_view s);

// 按分隔符切分，保留空段
std::vector<std::string_view> split(std::string_view s, char delim);

// 将 s 重复拼接 n 次
std::string repeat(std::string_view s, std::size_t n);

} // namespace vx::util
