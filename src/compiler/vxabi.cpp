// vxabi.cpp — VX ABI 符号命名与类型映射实现。
#include "vx/compiler/vxabi.hpp"

namespace vx::compiler::abi {

std::string sanitize(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_';
    out.push_back(ok ? c : '_');
  }
  return out;
}

std::string_view lib_name_of(std::string_view ref) {
  // 去掉尾部 '/'，再取最后一个 '/' 之后的部分（无 '/' 时即整个引用）
  std::size_t end = ref.size();
  while (end > 0 && ref[end - 1] == '/') {
    --end;
  }
  const std::size_t slash = ref.rfind('/', end == 0 ? 0 : end - 1);
  if (slash == std::string_view::npos) {
    return ref.substr(0, end);
  }
  return ref.substr(slash + 1, end - slash - 1);
}

std::string symbol(std::string_view lib, std::string_view name) {
  std::string out = "_vx_";
  out += sanitize(lib);
  out += "_";
  out += sanitize(name);
  return out;
}

std::string_view c_type_of(std::string_view vx_type) {
  if (vx_type == "int") return "int32_t";
  if (vx_type == "float") return "double";
  if (vx_type == "bool") return "int32_t";
  if (vx_type == "string") return "const char*";
  if (vx_type.empty() || vx_type == "void") return "void";
  return "int32_t"; // var/未知：规范整型
}

std::string_view llvm_type_of(std::string_view vx_type) {
  if (vx_type == "float") return "double";
  if (vx_type == "string") return "ptr";
  if (vx_type == "i64") return "i64";
  return "i32"; // int/bool/var/void/空
}

} // namespace vx::compiler::abi
