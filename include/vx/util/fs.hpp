// fs.hpp — 文件读写：统一的错误返回（std::expected），供各模块加载/输出源码。
#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace vx::util {

// 读取整个文件为字符串；失败时 error 为原因描述
std::expected<std::string, std::string>
read_file(const std::filesystem::path &path);

// 写入整个文件（自动创建父目录）；失败时 error 为原因描述
std::expected<void, std::string> write_file(const std::filesystem::path &path,
                                            std::string_view content);

} // namespace vx::util
