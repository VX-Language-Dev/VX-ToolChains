// vxobj.cpp — VXOBJ v4 二进制序列化器（vxc003）。
#include "vx/compiler/vxobj.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vx::compiler::obj {
namespace {

using ir::ModuleIR;

// ---- 大端整数写入 ----
void append_u8(std::string& buf, std::uint8_t v) {
  buf.push_back(static_cast<char>(v));
}

void append_u32_be(std::string& buf, std::uint32_t v) {
  buf.push_back(static_cast<char>((v >> 24U) & 0xFFU));
  buf.push_back(static_cast<char>((v >> 16U) & 0xFFU));
  buf.push_back(static_cast<char>((v >> 8U) & 0xFFU));
  buf.push_back(static_cast<char>(v & 0xFFU));
}

void append_write_string(std::string& buf, std::string_view s) {
  append_u32_be(buf, static_cast<std::uint32_t>(s.size()));
  buf.append(s.data(), s.size());
}

// ---- 类型名 → 类型标签（对齐 vxobj.vx get_type_tag）----
std::uint8_t type_tag_for(std::string_view name) {
  if (name == "int")
    return ir::TYPE_INT;
  if (name == "float")
    return ir::TYPE_FLOAT;
  if (name == "bool")
    return ir::TYPE_BOOL;
  if (name == "string" || name == "str")
    return ir::TYPE_STRING;
  if (name == "void")
    return ir::TYPE_VOID;
  if (name == "ptr")
    return ir::TYPE_POINTER;
  if (name == "var")
    return ir::TYPE_UNKNOWN;
  return ir::TYPE_UNKNOWN;
}

// ---- 序列化单条指令 ----
void serialize_inst(std::string& buf, const ir::Inst& inst) {
  append_u8(buf, static_cast<std::uint8_t>(inst.op));
  append_u32_be(buf, static_cast<std::uint32_t>(inst.args.size()));
  buf.append(inst.args.data(), inst.args.size());
}

// ---- 序列化函数 ----
void serialize_func(std::string& buf, const ir::FuncIR& f) {
  append_write_string(buf, f.name);
  append_u32_be(buf, f.id);
  append_u32_be(buf, static_cast<std::uint32_t>(f.params.size()));

  // has_return + 返回类型标签
  const bool has_return = !(f.ret_type.empty() || f.ret_type == "void");
  append_u8(buf, has_return ? 1 : 0);
  append_u8(buf, type_tag_for(f.ret_type));

  // 参数
  for (const vx::compiler::Param& p : f.params) {
    append_write_string(buf, p.name);
    append_u8(buf, type_tag_for(p.type));
  }

  // var_count (local_count)
  append_u32_be(buf, f.local_count);

  // local_types: 条目 {slot u32, tag u8}；仅写静态可确定的槽位
  std::uint32_t local_type_count = 0;
  for (const std::uint8_t tag : f.local_types) {
    if (tag != 0xFF && tag != ir::TYPE_UNKNOWN && tag != ir::TYPE_VOID)
      ++local_type_count;
  }
  append_u32_be(buf, local_type_count);
  for (std::size_t i = 0; i < f.local_types.size(); ++i) {
    const std::uint8_t tag = f.local_types[i];
    if (tag == 0xFF || tag == ir::TYPE_UNKNOWN || tag == ir::TYPE_VOID)
      continue;
    append_u32_be(buf, static_cast<std::uint32_t>(i));
    append_u8(buf, tag);
  }

  // 指令数量 + 指令序列
  append_u32_be(buf, static_cast<std::uint32_t>(f.instructions.size()));
  for (const ir::Inst& inst : f.instructions) {
    serialize_inst(buf, inst);
  }
}

// ---- 序列化链接表 ----
// 条目：func_id(u32) + is_external(u8) + name(string) +
//       abi_symbol(string) + lib(string) + from_cpp(u8) +
//       has_return(u8) + ret_tag(u8) + param_count(u32) + params
// （params 为 write_string(name)+type_tag(u8)）
void serialize_linkage(std::string& buf, const std::vector<ir::LinkEntry>& linkage) {
  append_u32_be(buf, static_cast<std::uint32_t>(linkage.size()));
  for (const ir::LinkEntry& e : linkage) {
    append_u32_be(buf, 0); // func_id = 0 (unused for extern)
    append_u8(buf, 1);     // is_external = true
    append_write_string(buf, e.name);
    append_write_string(buf, e.abi_symbol);
    append_write_string(buf, e.lib);
    append_u8(buf, e.from_cpp ? 1 : 0);
    const bool has_return = !(e.ret_type.empty() || e.ret_type == "void");
    append_u8(buf, has_return ? 1 : 0);
    append_u8(buf, type_tag_for(e.ret_type));
    append_u32_be(buf, static_cast<std::uint32_t>(e.params.size()));
    for (const ir::AbiParam& p : e.params) {
      append_write_string(buf, p.name);
      append_u8(buf, type_tag_for(p.type));
    }
  }
}

// ---- 序列化导入库依赖段 ----
// 条目：lib(string) + ref(string) + impl_file(string) + from_cpp(u8) +
//       cpp_header(string)
void serialize_deps(std::string& buf, const std::vector<ir::LibDep>& libs) {
  append_u32_be(buf, static_cast<std::uint32_t>(libs.size()));
  for (const ir::LibDep& d : libs) {
    append_write_string(buf, d.lib);
    append_write_string(buf, d.ref);
    append_write_string(buf, d.impl_file);
    append_u8(buf, d.from_cpp ? 1 : 0);
    append_write_string(buf, d.cpp_header);
  }
}

} // namespace

std::string serialize(const ir::ModuleIR& mod, std::string_view target) {
  // TypeIR 数据段
  std::string typeir_data;
  append_u32_be(typeir_data, static_cast<std::uint32_t>(mod.funcs.size()));
  append_u32_be(typeir_data, static_cast<std::uint32_t>(mod.structs.size()));
  for (const ir::StructLayout& sl : mod.structs) {
    append_write_string(typeir_data, sl.name);
    append_u32_be(typeir_data, static_cast<std::uint32_t>(sl.fields.size()));
    for (const ir::StructField& f : sl.fields) {
      append_write_string(typeir_data, f.name);
      append_u8(typeir_data, type_tag_for(f.type));
    }
  }
  for (const ir::FuncIR& f : mod.funcs) {
    serialize_func(typeir_data, f);
  }
  serialize_linkage(typeir_data, mod.linkage);

  // 段表：TypeIR 恒有；Deps 仅在有导入库时追加
  struct Section {
    std::string_view name;
    std::string data;
  };
  std::vector<Section> sections;
  sections.push_back({SECTION_TYPEIR, std::move(typeir_data)});
  if (!mod.libs.empty()) {
    std::string deps_data;
    serialize_deps(deps_data, mod.libs);
    sections.push_back({SECTION_DEPS, std::move(deps_data)});
  }
  // 行号段：源文件名 + 每个函数的指令行号（调试信息 / 诊断定位）。
  bool has_lines = false;
  for (const ir::FuncIR& f : mod.funcs) {
    if (!f.lines.empty()) {
      has_lines = true;
      break;
    }
  }
  if (has_lines || !mod.source_file.empty()) {
    std::string lines_data;
    append_write_string(lines_data, mod.source_file);
    append_u32_be(lines_data, static_cast<std::uint32_t>(mod.funcs.size()));
    for (const ir::FuncIR& f : mod.funcs) {
      append_write_string(lines_data, f.name);
      append_u32_be(lines_data, static_cast<std::uint32_t>(f.lines.size()));
      for (const int ln : f.lines) {
        append_u32_be(lines_data, static_cast<std::uint32_t>(ln));
      }
    }
    sections.push_back({SECTION_LINES, std::move(lines_data)});
  }

  // VXOBJ v4 头部
  // flags 低 3 位：优化级别编码（0=未指定，1..4 = -O0..-O3）；bit3：调试信息。
  const std::uint32_t opt_code = mod.opt_level >= 0 ? static_cast<std::uint32_t>(mod.opt_level + 1) : 0;
  const std::uint32_t flags = (opt_code & 0x7U) | (mod.debug ? 8U : 0U);

  // header_size = Magic(5) + Version(4) + Flags(4) + Target(string) + SectionCount(4)
  const std::uint32_t header_size = 5 + 4 + 4 + 4 + static_cast<std::uint32_t>(target.size()) + 4;

  // section_info_size = Σ( write_string(name) + offset(4) + size(4) )
  std::uint32_t section_info_size = 0;
  for (const Section& s : sections) {
    section_info_size += 4 + static_cast<std::uint32_t>(s.name.size()) + 4 + 4;
  }

  std::string result;
  // Magic
  result.append(VXOBJ_MAGIC.data(), VXOBJ_MAGIC.size());
  // Version
  append_u32_be(result, VXOBJ_VERSION);
  // Flags
  append_u32_be(result, flags);
  // Target
  append_write_string(result, target);
  // Section count
  append_u32_be(result, static_cast<std::uint32_t>(sections.size()));
  // Section table（数据偏移自 header + 段表之后顺序排布）
  std::uint32_t offset = header_size + section_info_size;
  for (const Section& s : sections) {
    append_write_string(result, s.name);
    append_u32_be(result, offset);
    append_u32_be(result, static_cast<std::uint32_t>(s.data.size()));
    offset += static_cast<std::uint32_t>(s.data.size());
  }
  // Section data
  for (const Section& s : sections) {
    result.append(s.data);
  }

  return result;
}

Header parse_header(std::string_view data) {
  Header h;
  if (data.size() < 5)
    return h;
  if (data.substr(0, 5) != VXOBJ_MAGIC)
    return h;
  h.magic_ok = true;
  if (data.size() < 5 + 4 + 4 + 4)
    return h;
  std::size_t pos = 5;
  auto read_u32 = [&](std::size_t& p) -> std::uint32_t {
    std::uint32_t v = (static_cast<std::uint32_t>(static_cast<unsigned char>(data[p])) << 24U) |
                      (static_cast<std::uint32_t>(static_cast<unsigned char>(data[p + 1])) << 16U) |
                      (static_cast<std::uint32_t>(static_cast<unsigned char>(data[p + 2])) << 8U) |
                      static_cast<std::uint32_t>(static_cast<unsigned char>(data[p + 3]));
    p += 4;
    return v;
  };
  h.version = read_u32(pos);
  h.flags = read_u32(pos);
  // target string
  std::uint32_t target_len = read_u32(pos);
  if (pos + target_len > data.size())
    return h;
  h.target.assign(data.data() + pos, target_len);
  pos += target_len;
  h.section_count = read_u32(pos);
  return h;
}

} // namespace vx::compiler::obj
