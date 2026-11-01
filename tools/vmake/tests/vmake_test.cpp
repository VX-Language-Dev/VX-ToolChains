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

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                             \
      ++g_failures;                                                                                                    \
    }                                                                                                                  \
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
    std::fprintf(stderr, "  parse error: line %zu: %s\n", errors[0].line, errors[0].message.c_str());
  }
  return manifest.value_or(Manifest{});
}

bool fails_with(std::string_view text, std::string_view needle) {
  std::vector<ParseError> errors;
  const auto manifest = vmake::parse_manifest(text, errors);
  if (manifest) {
    std::fprintf(stderr, "  expected failure but parsed: %.*s\n", static_cast<int>(text.size()), text.data());
    return false;
  }
  if (errors.empty() || errors[0].line == 0)
    return false;
  const bool match = errors[0].message.find(needle) != std::string::npos;
  if (!match) {
    std::fprintf(stderr, "  expected '%s', got: %s\n", std::string(needle).c_str(), errors[0].message.c_str());
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
  const char* text = "exe(\"app\", sources=[\n"
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
  const char* text = "exe(\"app\", sources=[\"main.vx\"])\n"
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
  CHECK(vmake::default_output_name(TargetKind::Exe, "app", "windows") == "app.exe");
  CHECK(vmake::default_output_name(TargetKind::Dylib, "m", "macosx") == "libm.dylib");
  CHECK(vmake::default_output_name(TargetKind::Dylib, "m", "linux") == "libm.so");
  CHECK(vmake::default_output_name(TargetKind::Dylib, "m", "windows") == "m.dll");
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

// deps 字段：解析、前向引用、校验错误
void test_deps() {
  // 基本解析 + 前向引用（deps 引用后声明的目标）
  const std::string text = "exe(\"app\", sources=[\"main.vx\"], deps=[\"calc\", \"util\"])\n"
                           "dylib(\"calc\", sources=[\"calc.vx\"])\n"
                           "dylib(\"util\", sources=[\"util.vx\"])\n";
  const Manifest m = must_parse(text);
  CHECK(m.targets.size() == 3);
  if (m.targets.size() == 3) {
    CHECK(m.targets[0].deps.size() == 2);
    if (m.targets[0].deps.size() == 2) {
      CHECK(m.targets[0].deps[0] == "calc");
      CHECK(m.targets[0].deps[1] == "util");
    }
    CHECK(m.targets[1].deps.empty());
  }

  // deps 缺失/空元素/重复字段
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], deps=[\"missing\"])\n", "未声明的目标"));
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], deps=[\"a\"])\n", "不能依赖自身"));
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], deps=[\"\"])\n", "含空依赖名"));
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], deps=[\"b\"], deps=[\"b\"])\n", "字段 deps 重复"));
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], deps=\"b\")\n", "deps 期望字符串数组"));
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], deps=[\"b\"])\n"
                   "exe(\"b\", sources=[\"b.vx\"])\n",
                   "必须是 dylib 或 staticlib 目标"));
}

// plan_order：selected 自动展开依赖闭包（依赖 dylib 先建）
void test_plan_order_deps() {
  const char* text = "exe(\"app\", sources=[\"main.vx\"], deps=[\"core\"])\n"
                     "dylib(\"core\", sources=[\"core.vx\"])\n"
                     "exe(\"tool\", sources=[\"tool.vx\"])\n";
  const Manifest m = must_parse(text);
  const auto picked = vmake::plan_order(m, {"app"});
  CHECK(picked.size() == 2);
  if (picked.size() == 2) {
    CHECK(picked[0]->name == "core"); // 依赖先构建
    CHECK(picked[1]->name == "app");
  }
  // selected 未命中任何目标 → 空计划
  const auto none = vmake::plan_order(m, {"nope"});
  CHECK(none.empty());
}

// cflags / defines / ldflags / tests 字段：解析、staticlib 禁用、元素校验
void test_flags_and_tests() {
  const std::string text = "exe(\"app\", sources=[\"main.vx\"],\n"
                           "    cflags=[\"-Wall\", \"-Wextra\"],\n"
                           "    defines=[\"NDEBUG\", \"FOO=1\"],\n"
                           "    ldflags=[\"-Wl,-rpath,/opt/lib\"],\n"
                           "    tests=[\"tests/app_test.vx\"])\n"
                           "staticlib(\"util\", sources=[\"util.vx\"])\n";
  const Manifest m = must_parse(text);
  CHECK(m.targets.size() == 2);
  if (m.targets.size() == 2) {
    const Target& t = m.targets[0];
    CHECK(t.cflags.size() == 2);
    if (t.cflags.size() == 2) {
      CHECK(t.cflags[0] == "-Wall");
      CHECK(t.cflags[1] == "-Wextra");
    }
    CHECK(t.defines.size() == 2);
    if (t.defines.size() == 2) {
      CHECK(t.defines[0] == "NDEBUG");
      CHECK(t.defines[1] == "FOO=1");
    }
    CHECK(t.ldflags.size() == 1);
    if (t.ldflags.size() == 1)
      CHECK(t.ldflags[0] == "-Wl,-rpath,/opt/lib");
    CHECK(t.tests.size() == 1);
    if (t.tests.size() == 1)
      CHECK(t.tests[0] == "tests/app_test.vx");
  }

  // staticlib：无链接阶段 → 禁止 ldflags / libs / lib_dirs / deps；
  // 但允许 cflags / defines（供其 .c/.cpp 源的 clang -c 编译）与 tests
  CHECK(fails_with("staticlib(\"u\", sources=[\"u.vx\"], ldflags=[\"-s\"])\n", "staticlib 目标不允许"));
  CHECK(fails_with("staticlib(\"u\", sources=[\"u.vx\"], libs=[\"m\"])\n", "staticlib 目标不允许"));
  CHECK(must_parse("staticlib(\"u\", sources=[\"u.vx\"], cflags=[\"-O2\"])\n"
                   "staticlib(\"v\", sources=[\"v.vx\"], defines=[\"X\"])\n")
            .targets.size() == 2);
  CHECK(must_parse("staticlib(\"u\", sources=[\"u.vx\"], tests=[\"t.vx\"])\n").targets.size() == 1);

  // 重复字段 / 空元素 / 类型错误 / tests 校验
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], cflags=[\"-O2\"],"
                   " cflags=[\"-g\"])\n",
                   "字段 cflags 重复"));
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], defines=[\"X\"],"
                   " defines=[\"Y\"])\n",
                   "字段 defines 重复"));
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], ldflags=[\"-s\"],"
                   " ldflags=[\"-s\"])\n",
                   "字段 ldflags 重复"));
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], tests=[\"t.vx\"],"
                   " tests=[\"u.vx\"])\n",
                   "字段 tests 重复"));
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], cflags=[\"\"])\n", "含空 cflags 条目"));
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], defines=[\"\"])\n", "含空 defines 条目"));
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], ldflags=[\"\"])\n", "含空 ldflags 条目"));
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], tests=[])\n", "tests 不能为空"));
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], tests=[\"\"])\n", "含空测试文件路径"));
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], tests=[\"t.txt\"])\n", "测试文件必须是 .vx"));
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], tests=\"t.vx\")\n", "tests 期望字符串数组"));
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], cflags=\"-Wall\")\n", "cflags 期望字符串数组"));
}

// test_target_of：测试目标派生规则（exe 仅测试源；库并入目标源）
void test_test_target_of() {
  Target lib;
  lib.name = "util";
  lib.kind = TargetKind::StaticLib;
  lib.sources = {"util.vx"};
  lib.tests = {"tests/util_test.vx"};
  const auto lib_test = vmake::test_target_of(lib);
  CHECK(lib_test.has_value());
  if (lib_test) {
    CHECK(lib_test->name == "util-test");
    CHECK(lib_test->kind == TargetKind::Exe);
    CHECK(lib_test->sources.size() == 2);
    if (lib_test->sources.size() == 2) {
      CHECK(lib_test->sources[0] == "util.vx");
      CHECK(lib_test->sources[1] == "tests/util_test.vx");
    }
  }

  // exe：目标源含 main，与测试入口冲突 → 仅测试源，继承 deps/flags
  Target exe;
  exe.name = "app";
  exe.kind = TargetKind::Exe;
  exe.sources = {"src/main.vx"};
  exe.tests = {"tests/app_test.vx"};
  exe.deps = {"calc"};
  exe.libs = {"m"};
  exe.lib_dirs = {"/opt/lib"};
  exe.cflags = {"-Wall"};
  exe.defines = {"NDEBUG"};
  exe.ldflags = {"-Wl,-s"};
  const auto exe_test = vmake::test_target_of(exe);
  CHECK(exe_test.has_value());
  if (exe_test) {
    CHECK(exe_test->name == "app-test");
    CHECK(exe_test->sources.size() == 1);
    if (exe_test->sources.size() == 1)
      CHECK(exe_test->sources[0] == "tests/app_test.vx");
    CHECK(exe_test->deps.size() == 1 && exe_test->deps[0] == "calc");
    CHECK(exe_test->libs.size() == 1 && exe_test->libs[0] == "m");
    CHECK(exe_test->lib_dirs.size() == 1);
    CHECK(exe_test->cflags.size() == 1);
    CHECK(exe_test->defines.size() == 1);
    CHECK(exe_test->ldflags.size() == 1);
  }

  Target no_tests;
  no_tests.name = "x";
  no_tests.kind = TargetKind::Exe;
  no_tests.sources = {"x.vx"};
  CHECK(!vmake::test_target_of(no_tests).has_value());
}

// sources 混合 C/C++ 源：.vx/.c/.cpp/.cc/.cxx 均可，其它后缀报错
void test_native_sources() {
  const std::string text = "exe(\"app\", sources=[\"main.vx\", \"platform.c\", \"util.cpp\"])\n"
                           "staticlib(\"u\", sources=[\"a.c\", \"b.cc\"])\n"
                           "dylib(\"d\", sources=[\"c.cxx\"])\n";
  const Manifest m = must_parse(text);
  CHECK(m.targets.size() == 3);
  if (m.targets.size() == 3) {
    CHECK(m.targets[0].sources.size() == 3);
    CHECK(vmake::has_native_source(m.targets[0]));
    CHECK(vmake::has_native_source(m.targets[1]));
    CHECK(vmake::has_native_source(m.targets[2]));
  }
  // 纯 .vx 目标不含原生源
  const Manifest pure = must_parse("exe(\"p\", sources=[\"m.vx\"])\n");
  CHECK(!pure.targets.empty() && !vmake::has_native_source(pure.targets[0]));

  // 非法后缀
  CHECK(fails_with("exe(\"a\", sources=[\"a.txt\"])\n", "源文件必须是"));
  CHECK(fails_with("exe(\"a\", sources=[\"a.m\"])\n", "源文件必须是"));
  CHECK(fails_with("exe(\"a\", sources=[\"a\"])\n", "源文件必须是"));
  // tests 仍仅限 .vx
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], tests=[\"t.c\"])\n", "测试文件必须是 .vx"));
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], tests=[\"t.cpp\"])\n", "测试文件必须是 .vx"));
}

// ccflags / cxxflags / incs / std：解析、重复、空元素、staticlib 允许
void test_native_flags() {
  const std::string text = "exe(\"app\", sources=[\"m.vx\", \"n.cpp\"],\n"
                           "    ccflags=[\"-Wall\"],\n"
                           "    cxxflags=[\"-Wall\", \"-Wextra\"],\n"
                           "    incs=[\"include\", \"third_party/include\"],\n"
                           "    std=\"c++20\")\n";
  const Manifest m = must_parse(text);
  CHECK(m.targets.size() == 1);
  if (!m.targets.empty()) {
    const Target& t = m.targets[0];
    CHECK(t.ccflags.size() == 1 && t.ccflags[0] == "-Wall");
    CHECK(t.cxxflags.size() == 2);
    CHECK(t.incs.size() == 2 && t.incs[1] == "third_party/include");
    CHECK(t.std_.value_or("") == "c++20");
  }

  // staticlib 也允许编译期字段
  const Manifest sl = must_parse("staticlib(\"u\", sources=[\"a.c\"], ccflags=[\"-O2\"], incs=[\"inc\"],"
                                 " std=\"c17\")\n");
  CHECK(sl.targets.size() == 1);
  if (!sl.targets.empty()) {
    CHECK(sl.targets[0].ccflags.size() == 1);
    CHECK(sl.targets[0].std_.value_or("") == "c17");
  }

  // 重复字段
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], ccflags=[\"-W\"],"
                   " ccflags=[\"-g\"])\n",
                   "字段 ccflags 重复"));
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], cxxflags=[\"-W\"],"
                   " cxxflags=[\"-g\"])\n",
                   "字段 cxxflags 重复"));
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], incs=[\"i\"],"
                   " incs=[\"j\"])\n",
                   "字段 incs 重复"));
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], std=\"c17\","
                   " std=\"c++20\")\n",
                   "字段 std 重复"));
  // 类型错误
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], std=[\"c17\"])\n", "std 期望字符串"));
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], ccflags=\"-Wall\")\n", "ccflags 期望字符串数组"));
  // 空元素
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], ccflags=[\"\"])\n", "含空 ccflags 条目"));
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], cxxflags=[\"\"])\n", "含空 cxxflags 条目"));
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], incs=[\"\"])\n", "含空 incs 条目"));
  // 未知字段提示包含新字段名
  CHECK(fails_with("exe(\"a\", sources=[\"a.vx\"], bogus=1)\n", "ccflags"));
}

// object_path：.vx → .vxobj；.c/.cpp → .o（同目录同命名规则）
void test_native_object_paths() {
  Target t;
  t.name = "app";
  t.kind = TargetKind::Exe;
  const std::filesystem::path build = "/tmp/vmake-build";

  const auto vxobj = vmake::object_path(build, t, 0, "src/main.vx");
  CHECK(vxobj.filename().string() == "0_main.vxobj");
  const auto cobj = vmake::object_path(build, t, 1, "src/platform.c");
  CHECK(cobj.filename().string() == "1_platform.o");
  const auto cppobj = vmake::object_path(build, t, 2, "src/util.cpp");
  CHECK(cppobj.filename().string() == "2_util.o");
  CHECK(cobj.parent_path() == build / "obj" / "app");
}

// test_target_of：派生测试目标继承编译期字段
void test_test_target_inherits_native() {
  Target t;
  t.name = "app";
  t.kind = TargetKind::Exe;
  t.sources = {"src/main.vx", "src/util.cpp"};
  t.tests = {"tests/app_test.vx"};
  t.ccflags = {"-Wall"};
  t.cxxflags = {"-std=c++20"};
  t.incs = {"include"};
  t.std_ = "c++20";
  const auto tt = vmake::test_target_of(t);
  CHECK(tt.has_value());
  if (tt) {
    CHECK(tt->ccflags.size() == 1);
    CHECK(tt->cxxflags.size() == 1);
    CHECK(tt->incs.size() == 1);
    CHECK(tt->std_.value_or("") == "c++20");
  }
}

} // namespace

int main() {
  test_basic_targets();
  test_multiline_array_and_escapes();
  test_parse_errors();
  test_plan_order();
  test_deps();
  test_plan_order_deps();
  test_output_names();
  test_paths();
  test_flags_and_tests();
  test_test_target_of();
  test_native_sources();
  test_native_flags();
  test_native_object_paths();
  test_test_target_inherits_native();

  if (g_failures == 0) {
    std::fprintf(stdout, "vmake tests: all passed\n");
    return 0;
  }
  std::fprintf(stderr, "vmake tests: %d failures\n", g_failures);
  return 1;
}
