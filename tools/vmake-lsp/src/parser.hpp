// parser.hpp — VMakeSet.vmake 轻量解析器（带行/列位置，供 LSP 使用）。
// 语法（Python 风格函数调用）：
//   exe("hello", sources=["src/main.vx", "src/util.vx"])
//   dylib("mylib", sources=["lib.vx"], output="libmylib.dylib")
//   staticlib("util", sources=["util.vx"])
//   exe("app", sources=["app.vx"], deps=["mylib", "util"],
//        libs=["ssl"], lib_dirs=["/usr/local/lib"])
//   exe("app", sources=["app.vx"], cflags=["-Wall"], defines=["NDEBUG"],
//        ldflags=["-Wl,-rpath,/opt/lib"], tests=["tests/app_test.vx"])
// 支持注释、尾随逗号、括号内换行、\uXXXX 转义。
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vmake_lsp {

struct Position {
  int line = 0;      // 0-based
  int character = 0; // 0-based，行内字节偏移（ASCII 场景下等同于字符偏移）
};

struct Range {
  Position start;
  Position end; // 不含 end
};

enum class TargetKind { Exe, Dylib, StaticLib };

// 附加字符串数组字段（cflags / defines / ldflags / tests）及其位置
struct ArrayField {
  std::string key; // 字段名
  std::vector<std::string> values;
  std::vector<Range> value_ranges; // 每个元素字符串值范围
  Range kw_range;                  // 字段名范围
  Range range;                     // 整个 [...] 数组范围
};

// 单个目标声明及其各部分的位置（用于 hover / definition / completion）
struct VTarget {
  TargetKind kind = TargetKind::Exe;
  Range kind_range;      // "exe" / "dylib" / "staticlib" 关键字范围
  std::string name;
  Range name_range;      // name 字符串值范围（不含引号）
  std::vector<std::string> sources;
  Range sources_range;   // 整个 [...] 数组范围
  Range sources_kw_range; // "sources" 字段名范围
  std::vector<Range> source_ranges; // 每个源字符串值范围
  std::optional<std::string> output;
  Range output_range;    // output 字符串值范围
  Range output_kw_range; // "output" 字段名范围
  std::vector<std::string> deps;
  bool has_deps = false; // 是否声明了 deps 字段（区分未设置与默认范围）
  Range deps_range;      // 整个 [...] 数组范围
  Range deps_kw_range;   // "deps" 字段名范围
  std::vector<Range> dep_ranges; // 每个依赖名字符串值范围
  // libs / lib_dirs：外部库支持
  std::vector<std::string> libs;
  bool has_libs = false;
  Range libs_range;
  Range libs_kw_range;
  std::vector<Range> lib_ranges;
  std::vector<std::string> lib_dirs;
  bool has_lib_dirs = false;
  Range lib_dirs_range;
  Range lib_dirs_kw_range;
  std::vector<Range> lib_dir_ranges;
  // cflags / defines / ldflags / tests：链接期 clang 参数与测试源文件
  std::vector<ArrayField> extra_arrays;
  Range call_range;      // 整个 exe(...) / dylib(...) / staticlib(...) 调用范围
};

// 一条诊断
struct Diagnostic {
  Range range;
  int severity = 1; // 1=Error
  std::string message;
};

// 解析结果
struct ParseResult {
  std::vector<VTarget> targets;
  std::vector<Diagnostic> diagnostics;
  bool ok = false;
};

// 目标类型关键字文本（"exe" / "dylib" / "staticlib"）。
const char *kind_name(TargetKind kind);

// 解析 VMakeSet.vmake 文本，返回目标列表与诊断。
ParseResult parse_vmake(std::string_view text);

// 根据 0-based 行/列查找命中目标（用于 hover/definition/补全上下文）。
// 返回命中的目标索引及命中部分（kind/name/sources/output/deps/libs/lib_dirs/
// cflags/defines/ldflags/tests/arg_sep/none）。
struct Hit {
  std::size_t target_index = 0;
  enum class Part {
    None, Kind, Name, Sources, Output, SourceElem,
    Deps, DepElem, Libs, LibElem, LibDirs, LibDirElem,
    ExtraField, // 附加数组字段名或整个 [...]（cflags/defines/ldflags/tests）
    ExtraElem   // 附加数组字段的元素
  };
  Part part = Part::None;
  std::size_t extra_index = 0; // ExtraField/ExtraElem 命中的 extra_arrays 下标
  std::size_t elem_index = 0;  // ExtraElem 命中的元素下标
};

Hit hit_test(const std::vector<VTarget> &targets, int line, int character);

} // namespace vmake_lsp
