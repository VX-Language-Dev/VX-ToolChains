#include "vx/util/fs.hpp"

#include <fstream>
#include <iterator>
#include <system_error>

namespace vx::util {

std::expected<std::string, std::string>
read_file(const std::filesystem::path &path) {
  std::error_code ec;
  if (std::filesystem::is_directory(path, ec)) {
    return std::unexpected("路径是目录而非文件: " + path.string());
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::unexpected("无法打开文件: " + path.string());
  }
  std::string content((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());
  if (file.bad()) {
    return std::unexpected("读取文件失败: " + path.string());
  }
  return content;
}

std::expected<void, std::string> write_file(const std::filesystem::path &path,
                                            std::string_view content) {
  const std::filesystem::path parent = path.parent_path();
  if (!parent.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      return std::unexpected("创建目录失败: " + parent.string() + ": " +
                             ec.message());
    }
  }

  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    return std::unexpected("无法创建文件: " + path.string());
  }
  file.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!file) {
    return std::unexpected("写入文件失败: " + path.string());
  }
  return {};
}

} // namespace vx::util
