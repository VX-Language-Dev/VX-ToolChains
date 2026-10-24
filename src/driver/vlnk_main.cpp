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
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ir = vx::compiler::ir;

static constexpr std::string_view VERSION = "vlnk 0.2.0";

struct Options {
  std::vector<std::string> inputs;
  std::string output = "a.out";
  std::string target; // 空 = 自动检测 host；非空 = 显式目标平台（交叉编译）
  bool emit_ir = false;
  bool dump = false;
  bool shared = false;
  bool verbose = false;
  bool help = false;
  bool version = false;
};

static void print_help() {
  std::fprintf(
      stdout,
      "VX Backend Linker %s\n"
      "Usage: vlnk [options] <input.vxobj>...\n"
      "Stages: read → merge → llvm_emitter → link (clang)\n"
      "Options:\n"
      "  -o <file>         Output file (default: a.out)\n"
      "  --shared          Emit a shared library (clang -shared -fPIC;\n"
      "                    no @main entry wrapper)\n"
      "  --target <triple> Target platform triple (default: auto-detect host;\n"
      "                    non-host targets are cross-compiled via clang -target)\n"
      "  --emit-ir         Emit LLVM IR to stdout and stop (no link)\n"
      "  --dump            Print restored ModuleIR summary\n"
      "  -v, --verbose     Verbose output\n"
      "  --version         Print version\n"
      "  -h, --help        Show this help\n"
      "\n"
      "Multiple .vxobj inputs are merged into one module (functions, linkage,\n"
      "library deps), enabling multi-file VX targets.\n",
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
    } else if (a == "--shared" || a == "-shared") {
      opts.shared = true;
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
    } else {
      opts.inputs.emplace_back(a);
    }
  }
  return 0;
}

static void dump_module(const ir::ModuleIR &m, std::string_view target) {
  std::fprintf(stdout, "Module: target=%.*s funcs=%zu linkage=%zu globals=%zu\n",
               static_cast<int>(target.size()), target.data(), m.funcs.size(),
               m.linkage.size(), m.global_vars.size());
  for (const ir::FuncIR &f : m.funcs) {
    std::fprintf(stdout,
                 "  func %s ret=%s params=%zu locals=%u insts=%zu\n",
                 f.name.c_str(),
                 f.ret_type.empty() ? "void" : f.ret_type.c_str(),
                 f.params.size(), f.local_count, f.instructions.size());
  }
  for (const ir::LinkEntry &e : m.linkage) {
    std::fprintf(stdout, "  extern %s%s%s\n", e.name.c_str(),
                 e.abi_symbol.empty() ? "" : " → ",
                 e.abi_symbol.empty() ? "" : e.abi_symbol.c_str());
  }
  for (const ir::LibDep &d : m.libs) {
    std::fprintf(stdout, "  lib %s (%s)%s impl=%s\n", d.lib.c_str(),
                 d.ref.c_str(), d.from_cpp ? " [cpp]" : "",
                 d.impl_file.empty() ? "-" : d.impl_file.c_str());
  }
}

// 合并多个 VXOBJ 模块：函数、链接表、库依赖、全局变量名。
// 函数按输入顺序拼接；重复函数名视为错误（避免静默覆盖入口）。
// 注意：FuncIR 的 Param string_view 指向各 read result 的 pool，
// 因此 results 必须在发射期间保持存活，且不得再增删元素。
static bool merge_inputs(std::vector<vx::backend::VxobjReadResult> &results,
                         ir::ModuleIR &out, std::string &error) {
  std::set<std::string> func_names;
  std::set<std::string> link_keys;
  std::set<std::string> lib_keys;
  std::set<std::string> globals;
  for (vx::backend::VxobjReadResult &rt : results) {
    for (ir::FuncIR &f : rt.module.funcs) {
      if (!func_names.insert(f.name).second) {
        error = "重复的函数定义 '" + f.name + "'（多输入合并）";
        return false;
      }
      out.funcs.push_back(std::move(f));
    }
    for (ir::LinkEntry &e : rt.module.linkage) {
      const std::string key = e.abi_symbol.empty() ? e.name : e.abi_symbol;
      if (link_keys.insert(key).second) out.linkage.push_back(std::move(e));
    }
    for (ir::LibDep &d : rt.module.libs) {
      // 同一实现文件（含同一声明头）只并入一次，避免 clang 重复符号
      const std::string key =
          d.impl_file.empty()
              ? "lib:" + d.lib + "|" + d.ref
              : "impl:" + d.impl_file + "|" + d.cpp_header +
                    (d.from_cpp ? "|cpp" : "|c");
      if (lib_keys.insert(key).second) out.libs.push_back(std::move(d));
    }
    for (std::string &g : rt.module.global_vars) {
      if (globals.insert(g).second) out.global_vars.push_back(std::move(g));
    }
  }
  return true;
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
  if (opts.inputs.empty()) {
    std::fprintf(stderr, "error: no input file\n\n");
    print_help();
    return 2;
  }

  // 读取全部输入；results 在发射/链接期间保持存活（Param string_view 指向其 pool）
  std::vector<vx::backend::VxobjReadResult> results;
  results.reserve(opts.inputs.size());
  std::set<std::string> targets;
  for (const std::string &input : opts.inputs) {
    auto data = vx::util::read_file(input);
    if (!data) {
      std::fprintf(stderr, "error: cannot read '%s': %s\n", input.c_str(),
                   data.error().c_str());
      return 1;
    }
    auto rt = vx::backend::read_vxobj(*data);
    if (!rt.ok) {
      std::fprintf(stderr, "error: %s: %s\n", input.c_str(), rt.error.c_str());
      return 1;
    }
    if (!rt.target.empty()) targets.insert(rt.target);
    results.push_back(std::move(rt));
  }

  // 合并模块：多文件目标（每个源文件各自 vxc 编译）在此汇合。
  ir::ModuleIR module;
  std::string merge_error;
  if (!merge_inputs(results, module, merge_error)) {
    std::fprintf(stderr, "error: %s\n", merge_error.c_str());
    return 1;
  }
  const std::string src_target = targets.empty() ? std::string{} : *targets.begin();
  if (targets.size() > 1) {
    std::fprintf(stderr,
                 "warning: 输入 .vxobj 的平台目标不一致（%zu 种），按首个 '%s' 发射\n",
                 targets.size(), src_target.c_str());
  }
  if (opts.dump) dump_module(module, src_target);

  // 目标平台选择：--target 显式指定 > 自动检测 host
  const std::string host = vx::backend::host_triple();
  const std::string target = opts.target.empty() ? host : opts.target;
  if (opts.verbose) {
    std::fprintf(stderr, "  host: %s (%s)\n  target: %s\n  inputs: %zu\n",
                 host.c_str(), vx::util::host_name().c_str(), target.c_str(),
                 opts.inputs.size());
  }
  if (!opts.target.empty()) {
    // 显式目标与 vxobj 内记录的编译目标不一致时提示来源差异
    if (!src_target.empty() && src_target != opts.target) {
      std::fprintf(stderr,
                   "warning: --target '%s' 与 vxobj 编译目标 '%s' 不一致\n",
                   opts.target.c_str(), src_target.c_str());
    }
  } else if (!src_target.empty() && src_target != host) {
    std::fprintf(stderr,
                 "warning: vxobj target '%s' 与 host '%s' 不一致，按 host 发射\n",
                 src_target.c_str(), host.c_str());
  }

  // 动态库不生成 @main 入口包装
  auto em = vx::backend::emit_llvm_ir(module, target, !opts.shared);
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
  auto lr = vx::backend::link_module(
      em.ir, module, opts.output,
      target == host ? std::string_view{} : std::string_view(target),
      opts.shared);
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
