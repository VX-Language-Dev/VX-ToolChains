// main.cpp — vxc002：VX 编译器前端（read → lex → parse → codegen）。
// 002 = TypeIR 代码生成已就绪；VXOBJ writer 待 vxc003。
// CLI 对齐自举 compiler/vxc.vx 的参数风格。
#include "vx/compiler/codegen.hpp"
#include "vx/compiler/diag.hpp"
#include "vx/compiler/importer.hpp"
#include "vx/compiler/lexer.hpp"
#include "vx/compiler/parser.hpp"
#include "vx/compiler/vxobj.hpp"
#include "vx/util/fs.hpp"
#include "vx/util/platform.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;
using namespace vx::compiler;
using vx::util::Arena;

static constexpr std::string_view VERSION = "0.4.0";

struct Options {
  std::string source;
  std::string output;
  std::string color = "auto"; // auto | always | never
  std::vector<std::string> includes; // -I 头文件搜索目录（按序，可多次）
  bool verbose = false;
  bool help = false;
  bool version = false;
  bool check = false;
  bool dump = false;
};

// auto 模式下仅当 stderr 是终端时才上色
static bool want_color(const Options &opts) {
  if (opts.color == "always") return true;
  if (opts.color == "never") return false;
  return isatty(fileno(stderr)) != 0;
}

static void print_help() {
  std::fprintf(
      stdout,
      "VX Compiler v%s\n"
      "Usage: vxc [options] <source.vx>\n"
      "Stages: read → lex → parse → codegen → vxobj\n"
      "Options:\n"
      "  -o <file>       Output VXOBJ file (default: <source>.vxobj)\n"
      "  -I <dir>        Add header search directory for @imp/@cppimp\n"
      "                  (repeatable; searched before the default headers/;\n"
      "                  also accepts -I<dir>)\n"
      "  --check         Type-check only; do not write output\n"
      "  --dump          Print module summary to stdout\n"
      "  -v, --verbose   Verbose output\n"
      "  --color <when>  Colorize diagnostics: auto|always|never (default auto;\n"
      "                  also accepts --color=<when>)\n"
      "  --version       Print version\n"
      "  -h, --help      Show this help\n",
      VERSION.data());
}

static int parse_args(int argc, char **argv, Options &opts) {
  for (int i = 1; i < argc; ++i) {
    std::string_view a = argv[i];
    if (a == "-h" || a == "--help") {
      opts.help = true;
    } else if (a == "--version") {
      opts.version = true;
    } else if (a == "-v" || a == "--verbose") {
      opts.verbose = true;
    } else if (a == "--color" || a.starts_with("--color=")) {
      if (a == "--color") {
        if (i + 1 >= argc) {
          std::fprintf(stderr, "error: --color requires an argument\n");
          return 2;
        }
        opts.color = argv[++i];
      } else {
        opts.color = std::string(a.substr(8));
      }
      if (opts.color != "auto" && opts.color != "always" &&
          opts.color != "never") {
        std::fprintf(stderr, "error: invalid --color value: %s\n",
                     opts.color.c_str());
        return 2;
      }
    } else if (a == "--check") {
      opts.check = true;
    } else if (a == "--dump") {
      opts.dump = true;
    } else if (a == "-o") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: -o requires an argument\n");
        return 2;
      }
      opts.output = argv[++i];
    } else if (a == "-I" || a.starts_with("-I")) {
      if (a == "-I") {
        if (i + 1 >= argc) {
          std::fprintf(stderr, "error: -I requires a directory argument\n");
          return 2;
        }
        opts.includes.emplace_back(argv[++i]);
      } else {
        opts.includes.emplace_back(std::string(a.substr(2)));
      }
    } else if (a.starts_with("-")) {
      std::fprintf(stderr, "error: unknown option: %s (try --help)\n", argv[i]);
      return 2;
    } else {
      if (!opts.source.empty()) {
        std::fprintf(stderr, "error: unexpected extra argument: %s\n", argv[i]);
        return 2;
      }
      opts.source = argv[i];
    }
  }
  return 0;
}

// 统计 AST 顶层各类声明的数量
struct AstStats {
  int funcs = 0;
  int classes = 0;
  int structs = 0;
  int externs = 0;
  int imports = 0;
};

static AstStats count_toplevel(Program &prog) {
  AstStats s;
  for (auto stmt : prog.stmts) {
    switch (stmt->kind) {
      case NodeKind::FuncDecl:    ++s.funcs; break;
      case NodeKind::ClassDecl:   ++s.classes; break;
      case NodeKind::StructDecl:  ++s.structs; break;
      case NodeKind::ExternDecl:  ++s.externs; break;
      case NodeKind::Import:      ++s.imports; break;
      default: break;
    }
  }
  return s;
}

int main(int argc, char **argv) {
  Options opts;
  int rc = parse_args(argc, argv, opts);
  if (rc != 0) return rc;

  if (opts.help) { print_help(); return 0; }
  if (opts.version) {
    std::fprintf(stdout, "VX Compiler %s\n", VERSION.data());
    return 0;
  }

  if (opts.source.empty()) {
    std::fprintf(stderr, "error: no source file specified\n");
    print_help();
    return 2;
  }

  // --- 阶段 1: 读文件 ---
  auto src_res = vx::util::read_file(opts.source);
  if (!src_res) {
    std::fprintf(stderr, "error: cannot read %s: %s\n",
                 opts.source.c_str(), src_res.error().c_str());
    return 1;
  }

  Arena arena;
  DiagBag bag;
  const bool color = want_color(opts);

  // 各阶段把诊断推入同一个 bag；触发错误即渲染全部诊断并终止（避免级联报错）

  // --- 阶段 2: 词法分析 ---
  std::span<const Token> toks = tokenize(*src_res, opts.source, arena, bag);
  if (bag.has_error()) {
    std::fputs(render_all(bag, *src_res, color).c_str(), stderr);
    return 1;
  }
  if (opts.verbose) {
    std::fprintf(stderr, "  lexer: %zu tokens\n", toks.size());
  }

  // --- 阶段 3: 语法分析 ---
  // fail-soft：解析器错误恢复后仍返回部分 AST；有错误即终止，不进入 codegen
  auto prog = parse(toks, opts.source, arena, bag);
  if (!prog || bag.has_error()) {
    std::fputs(render_all(bag, *src_res, color).c_str(), stderr);
    return 1;
  }

  AstStats stats = count_toplevel(*prog);
  if (opts.verbose) {
    std::fprintf(stderr, "  parser: %zu top-level stmts "
                 "(%d func, %d class, %d struct, %d extern, %d import)\n",
                 prog->stmts.size(),
                 stats.funcs, stats.classes, stats.structs,
                 stats.externs, stats.imports);
  }

  // --- 阶段 3.5: 库导入解析（@imp / @cppimp）---
  // 搜索顺序：-I 目录（按序）→ headers 根（VX_HEADERS 或 vxc 同级 headers/）
  // → @cppimp 追加系统 C/C++ 编译器头目录；相对引用只查源文件目录。
  fs::path headers_root;
  if (const char *env = std::getenv("VX_HEADERS")) {
    headers_root = env;
  } else {
    const fs::path exe_dir = executable_dir(argv[0]);
    headers_root = exe_dir.empty() ? fs::path("headers") : exe_dir / "headers";
  }
  ImportContext imp_ctx;
  imp_ctx.headers_root = headers_root;
  imp_ctx.source_dir = fs::path(opts.source).parent_path();
  std::error_code ec;
  for (const std::string &inc : opts.includes) {
    fs::path p = fs::absolute(inc, ec);
    imp_ctx.include_dirs.push_back(ec ? fs::path(inc) : p.lexically_normal());
    ec.clear();
  }
  std::optional<ImportTable> imports =
      build_import_table(*prog, imp_ctx, opts.source, bag);
  if (!imports) {
    std::fputs(render_all(bag, *src_res, color).c_str(), stderr);
    return 1;
  }
  if (opts.verbose) {
    std::fprintf(stderr, "  imports: %zu libs (headers root: %s",
                 imports->libs.size(), headers_root.string().c_str());
    for (const fs::path &d : imp_ctx.include_dirs) {
      std::fprintf(stderr, ", -I %s", d.string().c_str());
    }
    std::fprintf(stderr, ")\n");
  }

  // --- 阶段 4: TypeIR 代码生成 ---
  auto ir_res = generate_module(*prog, opts.source, bag, &*imports);
  if (!ir_res) {
    std::fputs(render_all(bag, *src_res, color).c_str(), stderr);
    return 1;
  }

  std::size_t total_insts = 0;
  for (const auto &f : ir_res->funcs) {
    total_insts += f.instructions.size();
  }
  if (opts.verbose) {
    std::fprintf(stderr, "  codegen: %zu funcs, %zu insts, %zu externs, "
                 "%zu globals\n",
                 ir_res->funcs.size(), total_insts, ir_res->linkage.size(),
                 ir_res->global_vars.size());
  }

  // --- 阶段 5: VXOBJ writer（vxc003 实现）---
  std::string out_path = opts.output;
  if (out_path.empty()) {
    std::string base = opts.source;
    const auto pos = base.find_last_of("/\\");
    const std::string dir = (pos == std::string::npos) ? std::string{} : base.substr(0, pos + 1);
    std::string name = (pos == std::string::npos) ? base : base.substr(pos + 1);
    const auto dot = name.find_last_of('.');
    if (dot != std::string::npos) name.erase(dot);
    out_path = dir + name + ".vxobj";
  }

  if (opts.dump) {
    std::fprintf(stdout, "funcs=%zu insts=%zu externs=%zu globals=%zu\n",
                 ir_res->funcs.size(), total_insts, ir_res->linkage.size(),
                 ir_res->global_vars.size());
  }

  if (opts.check) {
    std::fprintf(stdout, "OK %s: %zu funcs, %zu insts, %zu externs\n",
                 opts.source.c_str(), ir_res->funcs.size(), total_insts,
                 ir_res->linkage.size());
    return 0;
  }
  // 平台三元组：编译期自动检测宿主（架构 + 操作系统），如 "arm64-apple-macosx"
  const std::string target = vx::util::host_triple();
  if (opts.verbose) {
    std::fprintf(stderr, "  target: %s (%s)\n", target.c_str(),
                 vx::util::host_name().c_str());
  }
  const std::string vxobj_data = vx::compiler::obj::serialize(*ir_res, target);

  auto wr = vx::util::write_file(out_path, vxobj_data);
  if (!wr) {
    std::fprintf(stderr, "error: cannot write %s: %s\n",
                 out_path.c_str(), wr.error().c_str());
    return 1;
  }

  std::fprintf(stdout, "OK %s (%zu funcs, %zu insts, %zu externs)"
               " → %s\n",
               opts.source.c_str(), ir_res->funcs.size(), total_insts,
               ir_res->linkage.size(), out_path.c_str());
  return 0;
}
