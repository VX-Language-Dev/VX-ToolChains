// vxobj_reader.cpp — VXOBJ v4 反序列化：字节流 → ModuleIR。
// 线格式必须与 src/compiler/vxobj.cpp 的 serialize() 严格对称。
#include "vx/backend/vxobj_reader.hpp"

#include <cstdint>
#include <utility>

namespace vx::backend {
namespace {

namespace ir = vx::compiler::ir;

// 带边界检查的读取游标
struct Cursor {
  std::string_view data;
  std::size_t pos = 0;
  bool bad = false;
  std::string error;

  explicit Cursor(std::string_view d) : data(d) {}

  bool need(std::size_t n) {
    if (bad) return false;
    if (pos + n > data.size()) {
      bad = true;
      if (error.empty()) error = "VXOBJ 数据截断";
      return false;
    }
    return true;
  }
  std::uint8_t u8() {
    if (!need(1)) return 0;
    return static_cast<std::uint8_t>(data[pos++]);
  }
  std::uint32_t u32() {
    if (!need(4)) return 0;
    std::uint32_t v = (static_cast<std::uint32_t>(
                           static_cast<unsigned char>(data[pos])) << 24) |
                      (static_cast<std::uint32_t>(
                           static_cast<unsigned char>(data[pos + 1])) << 16) |
                      (static_cast<std::uint32_t>(
                           static_cast<unsigned char>(data[pos + 2])) << 8) |
                      static_cast<std::uint32_t>(
                          static_cast<unsigned char>(data[pos + 3]));
    pos += 4;
    return v;
  }
  std::string str() {
    const std::uint32_t n = u32();
    if (!need(n)) return {};
    std::string s(data.substr(pos, n));
    pos += n;
    return s;
  }
  void skip(std::size_t n) {
    if (need(n)) pos += n;
  }
};

// 类型标签 → 类型名（vxobj.cpp type_tag_for 的逆映射）
std::string_view tag_to_type(std::uint8_t tag) {
  switch (tag) {
    case ir::TYPE_INT:    return "int";
    case ir::TYPE_FLOAT:  return "float";
    case ir::TYPE_BOOL:   return "bool";
    case ir::TYPE_STRING: return "string";
    case ir::TYPE_VOID:   return "void";
    default:              return "var";
  }
}

} // namespace

VxobjReadResult read_vxobj(std::string_view data) {
  VxobjReadResult r;
  Cursor c{data};

  // ---- 文件头 ----
  if (data.size() < 5 || data.substr(0, 5) != "VXOBJ") {
    r.error = "不是 VXOBJ 文件（magic 不匹配）";
    return r;
  }
  c.pos = 5;
  const std::uint32_t version = c.u32();
  if (version != 4) {
    r.error = "不支持的 VXOBJ 版本: " + std::to_string(version);
    return r;
  }
  (void)c.u32(); // flags
  r.target = c.str();
  const std::uint32_t section_count = c.u32();

  // ---- 定位 TypeIR / Deps section ----
  std::size_t typeir_off = 0, typeir_size = 0;
  std::size_t deps_off = 0, deps_size = 0;
  bool found = false, found_deps = false;
  for (std::uint32_t i = 0; i < section_count; ++i) {
    const std::string name = c.str();
    const std::uint32_t off = c.u32();
    const std::uint32_t sz = c.u32();
    if (c.bad) break;
    if (name == "TypeIR") {
      typeir_off = off;
      typeir_size = sz;
      found = true;
    } else if (name == "Deps") {
      deps_off = off;
      deps_size = sz;
      found_deps = true;
    }
  }
  if (c.bad) {
    r.error = c.error;
    return r;
  }
  if (!found) {
    r.error = "VXOBJ 缺少 TypeIR section";
    return r;
  }
  if (typeir_off + typeir_size > data.size()) {
    r.error = "TypeIR section 越界";
    return r;
  }

  // ---- 解析 TypeIR 数据段 ----
  Cursor t{data.substr(typeir_off, typeir_size)};
  const std::uint32_t func_count = t.u32();
  const std::uint32_t struct_count = t.u32();
  if (t.bad) {
    r.error = t.error;
    return r;
  }

  auto intern = [&r](std::string_view s) -> std::string_view {
    r.pool.emplace_back(s);
    return r.pool.back();
  };

  for (std::uint32_t i = 0; i < struct_count; ++i) {
    ir::StructLayout sl;
    sl.name = std::string(t.str());
    const std::uint32_t fcount = t.u32();
    for (std::uint32_t j = 0; j < fcount; ++j) {
      ir::StructField sf;
      sf.name = std::string(t.str());
      const std::uint8_t tag = t.u8();
      sf.type = std::string(tag_to_type(tag));
      sl.fields.push_back(std::move(sf));
    }
    r.module.structs.push_back(std::move(sl));
  }

  for (std::uint32_t i = 0; i < func_count; ++i) {
    ir::FuncIR f;
    f.name = t.str();
    f.id = t.u32();
    const std::uint32_t param_count = t.u32();
    const std::uint8_t has_return = t.u8();
    const std::uint8_t ret_tag = t.u8();
    f.ret_type = has_return ? std::string(tag_to_type(ret_tag)) : "void";

    for (std::uint32_t k = 0; k < param_count; ++k) {
      vx::compiler::Param p;
      p.name = intern(t.str());
      p.type = intern(tag_to_type(t.u8()));
      f.params.push_back(p);
    }
    f.local_count = t.u32();

    const std::uint32_t local_types_count = t.u32();
    t.skip(static_cast<std::size_t>(local_types_count) * 5); // {u32 var_id, u8 tag}

    const std::uint32_t inst_count = t.u32();
    for (std::uint32_t k = 0; k < inst_count; ++k) {
      ir::Inst inst;
      inst.op = t.u8();
      const std::uint32_t args_len = t.u32();
      if (!t.need(args_len)) break;
      inst.args.assign(t.data.data() + t.pos, args_len);
      t.pos += args_len;
      f.instructions.push_back(std::move(inst));
    }
    if (t.bad) break;
    r.module.funcs.push_back(std::move(f));
  }
  if (t.bad) {
    r.error = t.error;
    return r;
  }

  // ---- 链接表 ----
  const std::uint32_t link_count = t.u32();
  for (std::uint32_t i = 0; i < link_count; ++i) {
    ir::LinkEntry e;
    (void)t.u32();  // func_id (unused for extern)
    (void)t.u8();   // is_external
    e.name = t.str();
    e.abi_symbol = t.str();
    e.lib = t.str();
    e.from_cpp = t.u8() != 0;
    const std::uint8_t has_return = t.u8();
    const std::uint8_t ret_tag = t.u8();
    e.ret_type = has_return ? std::string(tag_to_type(ret_tag)) : "void";
    const std::uint32_t param_count = t.u32();
    for (std::uint32_t k = 0; k < param_count; ++k) {
      ir::AbiParam p;
      p.name = t.str();
      p.type = tag_to_type(t.u8());
      e.params.push_back(std::move(p));
    }
    if (t.bad) break;
    r.module.linkage.push_back(std::move(e));
  }
  if (t.bad) {
    r.error = t.error;
    return r;
  }

  // ---- 导入库依赖（Deps section，可选）----
  if (found_deps) {
    if (deps_off + deps_size > data.size()) {
      r.error = "Deps section 越界";
      return r;
    }
    Cursor d{data.substr(deps_off, deps_size)};
    const std::uint32_t dep_count = d.u32();
    for (std::uint32_t i = 0; i < dep_count; ++i) {
      ir::LibDep dep;
      dep.lib = d.str();
      dep.ref = d.str();
      dep.impl_file = d.str();
      dep.from_cpp = d.u8() != 0;
      dep.cpp_header = d.str();
      if (d.bad) break;
      r.module.libs.push_back(std::move(dep));
    }
    if (d.bad) {
      r.error = d.error;
      return r;
    }
  }

  r.ok = true;
  return r;
}

} // namespace vx::backend
