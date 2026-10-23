// vxobj_test — VXOBJ v4 序列化测试：ModuleIR → .vxobj 字节流（自带断言宏）。
#include "vx/compiler/codegen.hpp"
#include "vx/compiler/diag.hpp"
#include "vx/compiler/lexer.hpp"
#include "vx/compiler/parser.hpp"
#include "vx/compiler/vxobj.hpp"
#include "vx/util/util.hpp"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

using namespace vx::compiler;
using vx::util::Arena;
namespace ir = vx::compiler::ir;
namespace obj = vx::compiler::obj;

std::optional<ir::ModuleIR> gen_src(std::string_view src) {
  static Arena arena;
  DiagBag bag;
  auto toks = tokenize(src, "<test>", arena, bag);
  auto prog = parse(toks, "<test>", arena, bag);
  if (!prog) {
    std::fprintf(stderr, "FAIL parse:\n%s", render_all(bag, src, false).c_str());
    ++g_failures;
    return std::nullopt;
  }
  auto mod = generate_module(*prog, "<test>", bag);
  if (!mod) {
    std::fprintf(stderr, "FAIL codegen:\n%s",
                 render_all(bag, src, false).c_str());
    ++g_failures;
    return std::nullopt;
  }
  return std::move(*mod);
}

// ---- 辅助：读取大端 u32 ----
std::uint32_t read_u32_be(const std::string &s, std::size_t pos) {
  return (static_cast<unsigned char>(s[pos]) << 24) |
         (static_cast<unsigned char>(s[pos + 1]) << 16) |
         (static_cast<unsigned char>(s[pos + 2]) << 8) |
         static_cast<unsigned char>(s[pos + 3]);
}

// ---- 用例 ----

void test_magic_header() {
  auto m = gen_src("func main():\n    return\n");
  CHECK(m);
  const std::string data = obj::serialize(*m, "test-target");
  // Magic
  CHECK(data.substr(0, 5) == "VXOBJ");
  // Version
  CHECK(read_u32_be(data, 5) == 4);
  // Flags
  CHECK(read_u32_be(data, 9) == 0);
  // Target string: length prefix + chars
  CHECK(read_u32_be(data, 13) == 11); // "test-target" length
  CHECK(data.substr(17, 11) == "test-target");
  // Section count
  std::size_t pos = 17 + 11;
  CHECK(read_u32_be(data, pos) == 1);
  pos += 4;
  // Section name
  CHECK(read_u32_be(data, pos) == 6); // "TypeIR" length
  CHECK(data.substr(pos + 4, 6) == "TypeIR");
  pos += 4 + 6;
}

void test_func_count() {
  auto m = gen_src(
      "func a():\n    return\n"
      "func b() -> int:\n    return 1\n");
  CHECK(m);
  CHECK(m->funcs.size() == 2);
  const std::string data = obj::serialize(*m, "t");
  // Header: magic(5)+ver(4)+flags(4)+target(4+len)+sections(4)
  std::size_t pos = 5 + 4 + 4 + (4 + 1) + 4;
  // Section info: name(4+7)+offset(4)+size(4)
  pos += (4 + 6) + 4 + 4;
  // TypeIR data: func_count u32
  CHECK(read_u32_be(data, pos) == 2);
  pos += 4;
  // struct_count u32
  CHECK(read_u32_be(data, pos) == 0);
}

void test_func_name_and_id() {
  auto m = gen_src("func hello() -> int:\n    return 42\n");
  CHECK(m);
  const std::string data = obj::serialize(*m, "t");
  // Skip to typeir data
  std::size_t pos = 5 + 4 + 4 + (4 + 1) + 4 + (4 + 6) + 4 + 4;
  pos += 4 + 4; // func_count + struct_count
  // Function: name string
  std::uint32_t name_len = read_u32_be(data, pos);
  pos += 4;
  CHECK(name_len == 5);
  CHECK(data.substr(pos, 5) == "hello");
  pos += 5;
  // id
  CHECK(read_u32_be(data, pos) == 0);
  pos += 4;
  // param_count
  CHECK(read_u32_be(data, pos) == 0);
  pos += 4;
}

void test_instruction_encoding() {
  auto m = gen_src("func f() -> int:\n    return 1 + 2\n");
  CHECK(m);
  const std::string data = obj::serialize(*m, "t");
  // We just verify the serialized data is non-empty and contains the magic
  CHECK(!data.empty());
  CHECK(data.substr(0, 5) == "VXOBJ");
  // Verify parse_header works
  auto hdr = obj::parse_header(data);
  CHECK(hdr.magic_ok);
  CHECK(hdr.version == 4);
  CHECK(hdr.target == "t");
  CHECK(hdr.section_count == 1);
}

void test_extern_linkage() {
  auto m = gen_src(
      "extern out(msg: string)\n"
      "func main():\n"
      "    out(\"hi\")\n"
      "    return\n");
  CHECK(m);
  CHECK(m->linkage.size() == 1);
  const std::string data = obj::serialize(*m, "t");
  CHECK(!data.empty());
  auto hdr = obj::parse_header(data);
  CHECK(hdr.magic_ok);
}

void test_empty_module() {
  // Module with no functions
  ir::ModuleIR mod;
  const std::string data = obj::serialize(mod, "empty-target");
  CHECK(data.substr(0, 5) == "VXOBJ");
  auto hdr = obj::parse_header(data);
  CHECK(hdr.magic_ok);
  CHECK(hdr.version == 4);
  CHECK(hdr.target == "empty-target");
  CHECK(hdr.section_count == 1);
}

void test_parse_header_bad_magic() {
  std::string data = "BADXX";
  auto hdr = obj::parse_header(data);
  CHECK(!hdr.magic_ok);
}

void test_round_trip_size() {
  auto m = gen_src("func main():\n    var x = 1\n    return\n");
  CHECK(m);
  const std::string data = obj::serialize(*m, "rt");
  // Just ensure it's reasonable size (at least header + some data)
  CHECK(data.size() > 50);
}

} // namespace

int main() {
  test_magic_header();
  test_func_count();
  test_func_name_and_id();
  test_instruction_encoding();
  test_extern_linkage();
  test_empty_module();
  test_parse_header_bad_magic();
  test_round_trip_size();

  if (g_failures == 0) {
    std::fprintf(stdout, "vxobj tests: all passed\n");
    return 0;
  }
  std::fprintf(stderr, "vxobj tests: %d failures\n", g_failures);
  return 1;
}
