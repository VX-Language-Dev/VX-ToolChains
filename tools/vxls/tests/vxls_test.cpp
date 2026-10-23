// vxls_test — vxls 测试：JSON 编解码 + 文档分析（诊断/符号/悬停/跳转）。
#include "analysis.hpp"
#include "json.hpp"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

namespace json = vxls::json;

void test_json_roundtrip() {
  // 基本类型解析
  auto v = json::parse(R"({"a":1,"b":"x","c":[true,null,2.5]})");
  CHECK(v.has_value());
  if (v) {
    CHECK(v->type() == json::Value::Type::Object);
    CHECK(v->int_or("a", 0) == 1);
    CHECK(v->str_or("b") == "x");
    const json::Value *c = v->find("c");
    CHECK(c != nullptr && c->type() == json::Value::Type::Array);
    if (c && c->as_array().size() == 3) {
      CHECK(c->as_array()[0].as_bool());
      CHECK(c->as_array()[1].is_null());
      CHECK(c->as_array()[2].as_number() == 2.5);
    }
    // dump → parse 往返
    const std::string text = v->dump();
    auto v2 = json::parse(text);
    CHECK(v2.has_value());
    if (v2) CHECK(v2->dump() == text);
  }
  // 转义与 Unicode
  CHECK(json::parse(R"("a\"b\nc")").has_value());
  auto u = json::parse(R"("\u4e16\u754c")");
  CHECK(u.has_value());
  if (u) CHECK(u->as_string() == "世界");
  // 非法输入
  CHECK(!json::parse("{").has_value());
  CHECK(!json::parse("[1,]").has_value());
  CHECK(!json::parse("tru").has_value());
  CHECK(!json::parse("").has_value());
}

void test_analysis_clean() {
  const std::string src =
      "func add(a: int, b: int) -> int:\n"
      "    return a + b\n"
      "\n"
      "class Point:\n"
      "    x: int\n"
      "    func get() -> int:\n"
      "        return self.x\n";
  auto a = vxls::analyze(src);
  CHECK(a.parse_ok);
  CHECK(a.diags.empty());
  CHECK(a.symbols.size() == 3); // add / Point / Point.get
  // 符号名与 kind
  CHECK(a.symbols[0].name == "add" && a.symbols[0].kind == 12);
  CHECK(a.symbols[1].name == "Point" && a.symbols[1].kind == 5);
  CHECK(a.symbols[2].name == "get" && a.symbols[2].kind == 6);
  CHECK(a.symbols[0].detail == "func add(a: int, b: int) -> int");
  // 补全含关键字、符号与内置
  bool has_kw = false, has_add = false, has_out = false;
  for (const auto &c : a.completions) {
    if (c.label == "while") has_kw = true;
    if (c.label == "add") has_add = true;
    if (c.label == "out_int") has_out = true;
  }
  CHECK(has_kw && has_add && has_out);
}

void test_analysis_diagnostics() {
  // 非法字符 → 词法诊断转 LSP（0 起始位置）
  const std::string src = "func main():\n    var x = $\n";
  auto a = vxls::analyze(src);
  CHECK(!a.diags.empty());
  if (!a.diags.empty()) {
    const auto d = vxls::to_lsp_diagnostic(a.diags[0], src);
    CHECK(d.severity == 1);
    CHECK(d.code == "E2001");
    CHECK(d.range.start.line == 1); // 0 起始
    CHECK(d.range.start.character == 12);
  }
  // 语法错误 → parse 失败且有诊断
  auto b = vxls::analyze("func main(:\n");
  CHECK(!b.parse_ok);
  CHECK(!b.diags.empty());
}

void test_position_conversion() {
  // UTF-8 源码：码点列 → UTF-16 字符列（"世界" 占 2 码点 2 UTF-16 码元）
  const std::string src = "var 世界 = 1\n";
  const auto p = vxls::diag_pos_to_lsp(src, 1, 5); // 第 5 个码点列
  CHECK(p.line == 0);
  CHECK(p.character == 4); // "var " 4 UTF-16 码元
}

void test_hover_and_definition() {
  const std::string src =
      "func add(a: int, b: int) -> int:\n"
      "    return a + b\n"
      "\n"
      "func main() -> int:\n"
      "    return add(1, 2)\n";
  // 悬停在 main 体内 add 调用上（第 5 行 "add" 位于 col 12-14）
  const std::string hover = vxls::hover_at(src, 4, 13);
  // 富化 hover：签名 + 参数表 + 返回类型
  CHECK(hover.find("```vx\nfunc add(a: int, b: int) -> int\n```") !=
        std::string::npos);
  CHECK(hover.find("**参数**") != std::string::npos);
  CHECK(hover.find("- `a: int`") != std::string::npos);
  CHECK(hover.find("- `b: int`") != std::string::npos);
  CHECK(hover.find("**返回** `int`") != std::string::npos);
  // 跳转到 add 声明（第 1 行 col 6）
  const auto r = vxls::definition_at(src, 4, 13);
  CHECK(r.start.line == 0);
  CHECK(r.start.character == 5);
  CHECK(r.end.character == 8);
  // 空白处无结果
  const auto bad = vxls::definition_at(src, 2, 0);
  CHECK(bad.start.line == -1);
}

// hover 丰富度：文档注释 / 局部变量 / 参数 / 类成员 / 内置运行时
void test_hover_rich() {
  const std::string src =
      "# 求两数之和\n"
      "func add(a: int, b: int) -> int:\n"
      "    return a + b\n"
      "\n"
      "class Point:\n"
      "    x: int\n"
      "    func get_x() -> int:\n"
      "        return self.x\n"
      "\n"
      "func main() -> int:\n"
      "    var unused = 1\n"
      "    out_int(add(1, 2))\n"
      "    return 0\n";
  // 文档注释进入 hover（第 12 行 out_int(add(...)) 的 add，0 起始 L11）
  const std::string h_add = vxls::hover_at(src, 11, 13);
  CHECK(h_add.find("> 求两数之和") != std::string::npos);
  // 局部变量：类型推断 + 初始值
  const std::string h_local = vxls::hover_at(src, 10, 8); // var unused
  CHECK(h_local.find("var unused") != std::string::npos);
  CHECK(h_local.find("**初始值** `1`") != std::string::npos);
  // 参数：类型 + 所属函数
  const std::string h_param = vxls::hover_at(src, 1, 10); // (a: int 的 a
  CHECK(h_param.find("**参数**（func add）") != std::string::npos);
  CHECK(h_param.find("**类型** `int`") != std::string::npos);
  // 类成员：字段与方法清单
  const std::string h_cls = vxls::hover_at(src, 4, 7); // class Point
  CHECK(h_cls.find("**成员**") != std::string::npos);
  CHECK(h_cls.find("`x: int`") != std::string::npos);
  CHECK(h_cls.find("func get_x() -> int") != std::string::npos);
  // 内置运行时：固定文档
  const std::string h_bi = vxls::hover_at(src, 11, 5); // out_int
  CHECK(h_bi.find("**内置运行时**") != std::string::npos);
  CHECK(h_bi.find("输出整数") != std::string::npos);
  // 空白处仍无结果
  CHECK(vxls::hover_at(src, 2, 0).empty());
}

// vx-tidy 诊断合并：VXTIDY 规则转 LSP，900 语法错误跳过
void test_tidy_diagnostics() {
  const std::string src =
      "func main() -> int:\n"
      "    var unused = 1\n"
      "    nosuch(1)\n"
      "    return 0\n";
  auto ds = vxls::tidy_diagnostics(src, "t.vx");
  CHECK(ds.size() == 2);
  bool has_unused = false, has_nosuch = false;
  for (const auto &d : ds) {
    CHECK(d.source == "vx-tidy");
    if (d.code == "VXTIDY002") {
      has_unused = true;
      CHECK(d.severity == 2);
      CHECK(d.range.start.line == 1); // 行级 range 覆盖整行
      CHECK(d.range.start.character == 0);
      CHECK(d.range.end.character > 0);
    }
    if (d.code == "VXTIDY001") has_nosuch = true;
  }
  CHECK(has_unused);
  CHECK(has_nosuch);
  // 语法错误源：VXTIDY900 被跳过（前端诊断负责）
  auto syn = vxls::tidy_diagnostics("func main(:\n", "t.vx");
  CHECK(syn.empty());
}

} // namespace

int main() {
  test_json_roundtrip();
  test_analysis_clean();
  test_analysis_diagnostics();
  test_position_conversion();
  test_hover_and_definition();
  test_hover_rich();
  test_tidy_diagnostics();

  if (g_failures == 0) {
    std::fputs("vxls tests: all passed\n", stdout);
    return 0;
  }
  std::fprintf(stderr, "vxls tests: %d failures\n", g_failures);
  return 1;
}
