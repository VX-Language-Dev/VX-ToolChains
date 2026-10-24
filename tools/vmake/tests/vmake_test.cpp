// vmake_test — VMake 配置解析 / 构建计划 / 产物命名单元测试（自带断言宏）。
#include "build.hpp"
#include "config.hpp"

#include <cstdio>
#include <filesystem>
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

using vmake::Manifest;
using vmake::ParseError;
using vmake::Target;
using vmake::TargetKind;

Manifest must_parse(std::string_view text) {
  std::vector<ParseError> errors;
  auto manifest = vmake::parse_manifest(text, errors);
  CHECK(manifest.has_value());
  if (!manifest && !errors.empty()) {
    std::fprintf(stderr, "  parse error: line %zu: %s\n", errors[0].line,
                 errors[0].message.c_str());
  }
  return manifest.value_or(Manifest{});
}

bool fails_with(std::string_view text, std::string_view needle) {
  std::vector<ParseError> errors;
  const auto manifest = vmake::parse_manifest(text, errors);
  if (manifest) {
    std::fprintf(stderr, "  expected failure but parsed: %.*s\n",
                 static_cast<int>(text.size()), text.data());
    return false;
  }
  if (errors.empty() || errors[0].line == 0)
    return false;
  const bool match = errors[0].message.find(needle) != std::string::npos;
  if (!match) {
    std::fprintf(stderr, "  expected '%s', got: %s\n",
                 std::string(needle).c_str(), errors[0].message.c_str());
  }
  return match;
}

void test_basic_targets() {
  const std::string text = "# demo\n"
                           "exe(\"hello\", sources=[\"src/main.vx\", \"src/util.vx\"])\n"
                           "\n"
                           "dylib(\"mylib\", sources=['lib.vx'], output=\"libmylib.dylib\")\n";
  const Manifest m = must_parse(text);
  CHECK(m.targets.size() == 2);
  if (m.targets.size() != 2)
    return;
  CHECK(m.targets[0].name == "hello");
  CHECK(m.targets[0].kind == TargetKind::Exe);
  CHECK(m.targets[0].sources.size() == 2);
  CHECK(m.targets[0].sources[1] == "src/util.vx");
  CHECK(!m.targets[0].output.has_value());
  CHECK(m.targets[1].name == "mylib");
  CHECK(m.targets[1].kind == TargetKind::Dylib);
  CHECK(m.targets[1].sources.at(0) == "lib.vx");
  CHECK(m.targets[1].output.value_or("") == "libmylib.dylib");
  CHECK(m.targets[0].line == 2);
  CHECK(m.targets[1].line == 4);
}

void test_multiline_array_and_escapes() {
  const char *text = "exe(\"app\", sources=[\n"
                     "  \"src/\\u006dain.vx\",  # 入口\n"
                     "  'src/util.vx',\n"
                     "])\n";
  const Manifest m = must_parse(text);
  CHECK(m.targets.size() == 1);
  if (m.targets.empty())
    return;
  CHECK(m.targets[0].sources.size() == 2);
  CHECK(m.targets[0].sources[0] == "src/main.vx");
  CHECK(m.targets[0].sources[1] == "src/util.vx");
}

void test_parse_errors() {
  CHECK(fails_with("foo()\n", "未知目标类型"));
  CHECK(fails_with("name = \"x\"\n", "未知目标类型"));
  CHECK(fails_with("exe(sources=[\"a.vx\"])\n", "缺少 name"));
  CHECK(fails_with("exe(\"a\")\n", "缺少 sources"));
  CHECK(fails_with("exe(\"a\", sources=[])\n", "不能为空"));
  CHECK(fails_with("exe(\"a\", sources=[\"a.txt\"])\n", "必须是 .vx"));
  CHECK(fails_with("exe(\"a\", name=\"b\", sources=[\"a.vx\"])\n", "name 重复"));
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], version=1)\n", "未知字段"));
  CHECK(fails_with("exe(\"bad name\", sources=[\"a.vx\"])\n", "非法"));
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], output=\"/abs/out\")\n", "相对构建目录"));
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], output=\"../out\")\n", "不得包含"));
  CHECK(fails_with("", "未声明任何目标"));
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"])\n"
                   "dylib(\"a\", sources=[\"b.vx\"])\n",
                   "重复"));
}

void test_plan_order() {
  const char *text = "exe(\"app\", sources=[\"main.vx\"])\n"
                     "dylib(\"core\", sources=[\"core.vx\"])\n"
                     "exe(\"tool\", sources=[\"tool.vx\"])\n"
                     "dylib(\"extra\", sources=[\"extra.vx\"])\n";
  const Manifest m = must_parse(text);
  const auto all = vmake::plan_order(m, {});
  CHECK(all.size() == 4);
  if (all.size() == 4) {
    CHECK(all[0]->name == "core");
    CHECK(all[1]->name == "extra");
    CHECK(all[2]->name == "app");
    CHECK(all[3]->name == "tool");
  }
  const auto picked = vmake::plan_order(m, {"app"});
  CHECK(picked.size() == 1);
  if (!picked.empty())
    CHECK(picked[0]->name == "app");
}

void test_output_names() {
  CHECK(vmake::default_output_name(TargetKind::Exe, "app", "macosx") == "app");
  CHECK(vmake::default_output_name(TargetKind::Exe, "app", "linux") == "app");
  CHECK(vmake::default_output_name(TargetKind::Exe, "app", "windows") ==
        "app.exe");
  CHECK(vmake::default_output_name(TargetKind::Dylib, "m", "macosx") ==
        "libm.dylib");
  CHECK(vmake::default_output_name(TargetKind::Dylib, "m", "linux") ==
        "libm.so");
  CHECK(vmake::default_output_name(TargetKind::Dylib, "m", "windows") ==
        "m.dll");
}

void test_paths() {
  Target t;
  t.name = "app";
  t.kind = TargetKind::Exe;
  const std::filesystem::path build = "/tmp/vmake-build";

  const auto default_path = vmake::artifact_path(build, t);
  CHECK(default_path == build / "app");

  t.output = "bin/custom";
  CHECK(vmake::artifact_path(build, t) == build / "bin" / "custom");

  const auto o0 = vmake::object_path(build, t, 0, "src/main.vx");
  const auto o1 = vmake::object_path(build, t, 1, "src/util.vx");
  CHECK(o0.filename().string() == "0_main.vxobj");
  CHECK(o1.filename().string() == "1_util.vxobj");
  CHECK(o0.parent_path() == build / "obj" / "app");
}

} // namespace

int main() {
  test_basic_targets();
  test_multiline_array_and_escapes();
  test_parse_errors();
  test_plan_order();
  test_output_names();
  test_paths();

  if (g_failures == 0) {
    std::fprintf(stdout, "vmake tests: all passed\n");
    return 0;
  }
  std::fprintf(stderr, "vmake tests: %d failures\n", g_failures);
  return 1;
}
