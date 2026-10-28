// importer.cpp — 库导入解析实现：定位 headers/、解析 .vxh 声明、装载符号与依赖。
#include "vx/compiler/importer.hpp"

#include "vx/compiler/lexer.hpp"
#include "vx/compiler/vxabi.hpp"
#include "vx/util/arena.hpp"
#include "vx/util/fs.hpp"

#include <algorithm>
#include <initializer_list>
#include <utility>

namespace fs = std::filesystem;

namespace vx::compiler {
namespace {

// 文件是否存在（普通文件或目录均可）；命名加前缀避免与 std::filesystem::exists
// 因 ADL 产生重载歧义。
bool path_exists(const fs::path &p) {
  std::error_code ec;
  return fs::exists(p, ec);
}

// 依次尝试 stem+ext，返回第一个存在的绝对路径字符串；都不存在返回空。
std::string probe_first(const fs::path &stem, std::initializer_list<const char *> exts) {
  for (const char *e : exts) {
    fs::path p = stem;
    p += e;
    if (path_exists(p)) return p.lexically_normal().string();
  }
  return {};
}

// 绝对化 + 规范化（不解析符号链接，避免破坏尚未创建的路径）
fs::path normalize(const fs::path &p) {
  std::error_code ec;
  fs::path a = fs::absolute(p, ec);
  if (ec) a = p;
  return a.lexically_normal();
}

// 库引用 → 基准路径（未加扩展名）
fs::path ref_base(const ImportContext &ctx, std::string_view ref) {
  std::string_view r = ref;
  // 前导 '/' 视为「相对 headers 根」的显式标记，去掉后拼接
  while (!r.empty() && r.front() == '/') r.remove_prefix(1);
  const bool relative = !r.empty() && r.front() == '.';
  const fs::path base = relative ? ctx.source_dir : ctx.headers_root;
  return normalize(base / fs::path(r));
}

std::string_view alias_of(const std::string_view alias, std::string_view fallback) {
  return alias.empty() ? fallback : alias;
}

// 解析 .vxh 声明文件，收集其中的 extern 声明为导入符号。
bool collect_symbols(const fs::path &decl, std::string_view lib,
                     std::vector<ImportedSymbol> &out, std::string &err) {
  auto src = vx::util::read_file(decl);
  if (!src) {
    err = "无法读取库声明文件 " + decl.string() + ": " + src.error();
    return false;
  }
  vx::util::Arena arena;
  DiagBag bag;
  auto toks = tokenize(*src, decl.string(), arena, bag);
  auto prog = parse(toks, decl.string(), arena, bag);
  if (!prog || bag.has_error()) {
    err = "库声明文件语法错误 " + decl.string() + ":\n" +
          render_all(bag, *src, false);
    return false;
  }
  for (const Stmt *s : prog->stmts) {
    if (s->kind != NodeKind::ExternDecl) continue;
    const auto *e = static_cast<const ExternDecl *>(s);
    ImportedSymbol sym;
    sym.vx_name = std::string(e->name);
    sym.abi_symbol = abi::symbol(lib, e->name);
    sym.ret_type = std::string(e->ret_type);
    for (const Param &p : e->params) {
      sym.params.push_back(ir::AbiParam{std::string(p.name), std::string(p.type)});
    }
    out.push_back(std::move(sym));
  }
  return true;
}

} // namespace

const ImportedLib *ImportTable::lib_by_prefix(std::string_view prefix) const {
  for (const ImportedLib &l : libs) {
    if (l.alias == prefix || l.name == prefix) return &l;
  }
  // 同一库的额外别名（@imp(math) as m）→ 按 ref 反查主条目
  for (const auto &[al, ref] : aliases) {
    if (al != prefix) continue;
    for (const ImportedLib &l : libs) {
      if (l.ref == ref) return &l;
    }
  }
  return nullptr;
}

const ImportedSymbol *ImportTable::find_symbol(const ImportedLib &lib,
                                               std::string_view name) {
  for (const ImportedSymbol &s : lib.symbols) {
    if (s.vx_name == name) return &s;
  }
  return nullptr;
}

fs::path executable_dir(std::string_view argv0) {
  if (argv0.empty()) return {};
  fs::path p(argv0);
  std::error_code ec;
  fs::path canon = fs::canonical(p, ec);
  if (ec || canon.empty()) canon = fs::absolute(p, ec);
  return canon.parent_path();
}

fs::path resolve_ref_path(const ImportContext &ctx, std::string_view ref) {
  return ref_base(ctx, ref);
}

std::optional<ImportTable> build_import_table(const Program &prog,
                                              const ImportContext &ctx,
                                              std::string_view main_file,
                                              DiagBag &bag) {
  ImportTable table;
  const auto fail = [&](std::string msg, int line) {
    bag.add(make_diag(diag_code::sem_import, Severity::Error, std::move(msg),
                      std::string(main_file), line, 0));
  };

  for (const Stmt *s : prog.stmts) {
    if (s->kind == NodeKind::ImpDecl) {
      const auto *d = static_cast<const ImpDecl *>(s);
      const std::string ref(d->lib);
      // 去重：同一引用只加载一次；但重复声明可能带新别名（@imp(math) as m），
      // 别名记入 aliases 以便 lib_by_prefix 解析。
      const std::string_view name = abi::lib_name_of(d->lib);
      auto dup = std::find_if(table.libs.begin(), table.libs.end(),
                              [&](const ImportedLib &l) { return l.ref == ref; });
      if (dup != table.libs.end()) {
        const std::string alias(alias_of(d->alias, name));
        if (alias != dup->alias && alias != dup->name &&
            !std::any_of(table.aliases.begin(), table.aliases.end(),
                         [&](const auto &a) { return a.first == alias; })) {
          table.aliases.emplace_back(alias, ref);
        }
        continue;
      }

      fs::path base = ref_base(ctx, d->lib);
      fs::path decl = base;
      decl += ".vxh";
      if (!path_exists(decl)) {
        fail("找不到库 '" + ref + "'（期望声明文件 " + decl.string() +
                 "；根目录库直接引用文件名，子目录库用 /子目录/库名）",
             d->line);
        return std::nullopt;
      }
      ImportedLib lib;
      lib.ref = ref;
      lib.name = std::string(name);
      lib.alias = std::string(alias_of(d->alias, name));
      lib.decl_file = normalize(decl).string();
      lib.impl_file = probe_first(base, {".c", ".a", ".dylib"});
      std::string err;
      if (!collect_symbols(decl, lib.name, lib.symbols, err)) {
        fail(err, d->line);
        return std::nullopt;
      }
      table.libs.push_back(std::move(lib));
    } else if (s->kind == NodeKind::CppImpDecl) {
      const auto *d = static_cast<const CppImpDecl *>(s);
      const std::string ref(d->header);
      auto dup = std::find_if(table.libs.begin(), table.libs.end(),
                              [&](const ImportedLib &l) { return l.ref == ref; });
      if (dup != table.libs.end()) {
        const std::string cpp_name =
            fs::path(ref).stem().string(); // 库名 = 头文件主名
        const std::string alias(alias_of(d->alias, cpp_name));
        if (alias != dup->alias && alias != dup->name &&
            !std::any_of(table.aliases.begin(), table.aliases.end(),
                         [&](const auto &a) { return a.first == alias; })) {
          table.aliases.emplace_back(alias, ref);
        }
        continue;
      }
      fs::path header = ref_base(ctx, d->header);
      // 伴生声明文件：同目录、同主名、扩展名 .vxh
      fs::path decl = header.parent_path() / (header.stem().string() + ".vxh");
      if (!path_exists(header)) {
        fail("@cppimp 头文件不存在: " + header.string(), d->line);
        return std::nullopt;
      }
      if (!path_exists(decl)) {
        fail("@cppimp 缺少伴生声明文件 " + decl.string() +
                 "（与 C++ 头文件同目录、同主名、扩展名 .vxh）",
             d->line);
        return std::nullopt;
      }
      ImportedLib lib;
      const std::string lib_name = header.stem().string();
      lib.ref = ref;
      lib.name = lib_name;
      lib.alias = std::string(alias_of(d->alias, lib_name));
      lib.decl_file = normalize(decl).string();
      lib.from_cpp = true;
      lib.cpp_header = normalize(header).string();
      lib.impl_file = probe_first(header.parent_path() / header.stem(),
                                  {".cpp", ".cc", ".cxx"});
      std::string err;
      if (!collect_symbols(decl, lib_name, lib.symbols, err)) {
        fail(err, d->line);
        return std::nullopt;
      }
      table.libs.push_back(std::move(lib));
    }
  }
  return table;
}

} // namespace vx::compiler
