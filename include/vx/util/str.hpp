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

// 检查字符串是否以 prefix 开头
bool starts_with(std::string_view s, std::string_view prefix);

// 检查字符串是否以 suffix 结尾
bool ends_with(std::string_view s, std::string_view suffix);

// 检查字符串是否包含 substr 子串
bool contains(std::string_view s, std::string_view substr);

// 替换所有匹配的子串 from 为 to，返回新字符串
std::string replace(std::string_view s, std::string_view from,
                    std::string_view to);

} // namespace vx::util
