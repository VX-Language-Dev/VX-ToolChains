// vlnk_main.cpp — 后端链接器 CLI：.vxobj → 原生可执行文件。
// 流水线：read .vxobj → read_vxobj(反序列化) → emit_llvm_ir → link_executable(clang)。
// 参数风格对齐前端 vxc003。
#include "vx/backend/link_driver.hpp"
#include "vx/backend/llvm_emitter.hpp"
#include "vx/backend/vxobj_reader.hpp"
#include "vx/util/fs.hpp"
#include "vx/util/platform.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
namespace ir = vx::compiler::ir;

static constexpr std::string_view VERSION = "vlnk 0.1.0";

struct Options {
  std::string input;
  std::string output = "a.out";
  std::string target; // 空 = 自动检测 host；非空 = 显式目标平台（交叉编译）
  bool emit_ir = false;
  bool dump = false;
  bool verbose = false;
  bool help = false;
  bool version = false;
};

static void print_help() {
  std::fprintf(
      stdout,
      "VX Backend Linker %s\n"
      "Usage: vlnk [options] <input.vxobj>\n"
      "Stages: read → vxobj_reader → llvm_emitter → link (clang)\n"
      "Options:\n"
      "  -o <file>         Output executable (default: a.out)\n"
      "  --target <triple> Target platform triple (default: auto-detect host;\n"
      "                    non-host targets are cross-compiled via clang -target)\n"
      "  --emit-ir         Emit LLVM IR to stdout and stop (no link)\n"
      "  --dump            Print restored ModuleIR summary\n"
      "  -v, --verbose     Verbose output\n"
      "  --version         Print version\n"
      "  -h, --help        Show this help\n",
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
    } else if (a == "--emit-ir") {
      opts.emit_ir = true;
    } else if (a == "--dump") {
      opts.dump = true;
    } else if (a == "--target") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: --target requires an argument\n");
        return 2;
      }
      opts.target = argv[++i];
    } else if (a == "-o") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: -o requires an argument\n");
        return 2;
      }
      opts.output = argv[++i];
    } else if (a.starts_with("-")) {
      std::fprintf(stderr, "error: unknown option: %.*s\n",
                   static_cast<int>(a.size()), a.data());
      return 2;
    } else if (opts.input.empty()) {
      opts.input = std::string(a);
    } else {
      std::fprintf(stderr, "error: multiple inputs not supported\n");
      return 2;
    }
  }
  return 0;
}

static void dump_module(const vx::backend::VxobjReadResult &rt) {
  std::fprintf(stdout, "Module: target=%s funcs=%zu linkage=%zu globals=%zu\n",
               rt.target.c_str(), rt.module.funcs.size(),
               rt.module.linkage.size(), rt.module.global_vars.size());
  for (const ir::FuncIR &f : rt.module.funcs) {
    std::fprintf(stdout,
                 "  func %s ret=%s params=%zu locals=%u insts=%zu\n",
                 f.name.c_str(),
                 f.ret_type.empty() ? "void" : f.ret_type.c_str(),
                 f.params.size(), f.local_count, f.instructions.size());
  }
  for (const ir::LinkEntry &e : rt.module.linkage) {
    std::fprintf(stdout, "  extern %s%s%s\n", e.name.c_str(),
                 e.abi_symbol.empty() ? "" : " → ",
                 e.abi_symbol.empty() ? "" : e.abi_symbol.c_str());
  }
  for (const ir::LibDep &d : rt.module.libs) {
    std::fprintf(stdout, "  lib %s (%s)%s impl=%s\n", d.lib.c_str(),
                 d.ref.c_str(), d.from_cpp ? " [cpp]" : "",
                 d.impl_file.empty() ? "-" : d.impl_file.c_str());
  }
}

int main(int argc, char **argv) {
  Options opts;
  if (const int rc = parse_args(argc, argv, opts); rc != 0) return rc;
  if (opts.help) {
    print_help();
    return 0;
  }
  if (opts.version) {
    std::fprintf(stdout, "%s\n", VERSION.data());
    return 0;
  }
  if (opts.input.empty()) {
    std::fprintf(stderr, "error: no input file\n\n");
    print_help();
    return 2;
  }

  auto data = vx::util::read_file(opts.input);
  if (!data) {
    std::fprintf(stderr, "error: cannot read '%s': %s\n", opts.input.c_str(),
                 data.error().c_str());
    return 1;
  }

  auto rt = vx::backend::read_vxobj(*data);
  if (!rt.ok) {
    std::fprintf(stderr, "error: %s\n", rt.error.c_str());
    return 1;
  }
  if (opts.dump) dump_module(rt);

  // 目标平台选择：--target 显式指定 > 自动检测 host
  const std::string host = vx::backend::host_triple();
  const std::string target = opts.target.empty() ? host : opts.target;
  if (opts.verbose) {
    std::fprintf(stderr, "  host: %s (%s)\n  target: %s\n", host.c_str(),
                 vx::util::host_name().c_str(), target.c_str());
  }
  if (!opts.target.empty()) {
    // 显式目标与 vxobj 内记录的编译目标不一致时提示来源差异
    if (!rt.target.empty() && rt.target != opts.target) {
      std::fprintf(stderr,
                   "warning: --target '%s' 与 vxobj 编译目标 '%s' 不一致\n",
                   opts.target.c_str(), rt.target.c_str());
    }
  } else if (!rt.target.empty() && rt.target != host) {
    std::fprintf(stderr,
                 "warning: vxobj target '%s' 与 host '%s' 不一致，按 host 发射\n",
                 rt.target.c_str(), host.c_str());
  }

  auto em = vx::backend::emit_llvm_ir(rt.module, target);
  if (!em.ok) {
    std::fprintf(stderr, "error: %s\n", em.error.c_str());
    return 1;
  }

  if (opts.emit_ir) {
    std::fwrite(em.ir.data(), 1, em.ir.size(), stdout);
    return 0;
  }

  // target == host 走本机链接；否则给 clang 追加 -target（交叉编译）
  // 使用 link_module：依 vxobj 中的库依赖并入实现文件并生成 @cppimp shim。
  auto lr = vx::backend::link_module(em.ir, rt.module, opts.output,
                                     target == host ? std::string_view{}
                                                    : std::string_view(target));
  if (!lr.ok) {
    std::fprintf(stderr, "error: %s\n", lr.error.c_str());
    return 1;
  }
  if (opts.verbose) {
    std::fprintf(stdout, "%s\n", lr.command.c_str());
  }
  std::fprintf(stdout, "wrote %s\n", opts.output.c_str());
  return 0;
}
