// util_test — Util 层单元测试（自带断言宏，不依赖第三方框架；
// MinSizeRel 下 NDEBUG 会禁用 assert，故不用 <cassert>）。
#include "vx/util/util.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
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

void test_arena() {
  vx::util::Arena arena(64); // 故意用小块容量，覆盖扩容路径

  // 基本分配与读写
  int *a = arena.create<int>(42);
  int *b = arena.create<int>(7);
  CHECK(*a == 42);
  CHECK(*b == 7);
  CHECK(a != b);

  // 对齐分配
  void *p = arena.allocate(1, 64);
  CHECK(p != nullptr);
  CHECK(reinterpret_cast<std::uintptr_t>(p) % 64 == 0);

  // 超出单块容量的大分配
  void *big = arena.allocate(1024);
  CHECK(big != nullptr);
  std::memset(big, 0xAB, 1024);
  CHECK(static_cast<unsigned char *>(big)[1023] == 0xAB);

  // 构造非平凡聚合
  struct Pair {
    int x;
    int y;
  };
  Pair *pr = arena.create<Pair>(Pair{1, 2});
  CHECK(pr->x == 1);
  CHECK(pr->y == 2);

  // reset 后可继续分配使用
  arena.reset();
  int *c = arena.create<int>(99);
  CHECK(*c == 99);

  // reset 后内存从块头复用（同一地址可再次分配）
  vx::util::Arena fresh(64);
  void *q1 = fresh.allocate(8);
  fresh.reset();
  void *q2 = fresh.allocate(8);
  CHECK(q1 == q2);
}

void test_str() {
  using vx::util::repeat;
  using vx::util::split;
  using vx::util::trim;

  CHECK(trim("  hello \n") == "hello");
  CHECK(trim("\t\r\n 世界  ") == "世界");
  CHECK(trim("") == "");
  CHECK(trim("   \t ") == "");

  auto parts = split("a,b,,c", ',');
  CHECK(parts.size() == 4);
  CHECK(parts[0] == "a");
  CHECK(parts[1] == "b");
  CHECK(parts[2].empty());
  CHECK(parts[3] == "c");

  auto no_delim = split("abc", ',');
  CHECK(no_delim.size() == 1);
  CHECK(no_delim[0] == "abc");

  CHECK(repeat("ab", 3) == "ababab");
  CHECK(repeat("x", 0) == "");
  CHECK(repeat("", 5) == "");
}

void test_fs() {
  namespace fs = std::filesystem;
  using vx::util::read_file;
  using vx::util::write_file;

  const fs::path dir = fs::temp_directory_path() / "vx_util_layer_test";

  // write_file 自动创建多级父目录
  const fs::path file = dir / "nested" / "deep" / "hello.txt";
  auto w = write_file(file, "你好，VX\n");
  CHECK(w.has_value());

  auto r = read_file(file);
  CHECK(r.has_value());
  if (r) {
    CHECK(*r == "你好，VX\n");
  }

  // 读取不存在的文件应返回错误
  auto missing = read_file(dir / "no_such_file.txt");
  CHECK(!missing.has_value());
  if (!missing) {
    CHECK(!missing.error().empty());
  }

  fs::remove_all(dir);
}

// 用真实 VX 源码驱动 Util 层：read_file / write_file / split / trim / Arena
void test_vx_sources() {
  namespace fs = std::filesystem;
  using vx::util::read_file;
  using vx::util::split;
  using vx::util::trim;
  using vx::util::write_file;

  // 从当前目录向上定位仓库根（以 compiler/lexer.vx 为标志）
  fs::path root = fs::current_path();
  while (!root.empty() && !fs::exists(root / "compiler" / "lexer.vx")) {
    root = root.parent_path();
  }
  CHECK(!root.empty());
  if (root.empty()) {
    return;
  }

  // 收集 compiler/、std/、tests/ 下全部 .vx 源码
  std::vector<fs::path> sources;
  for (const char *dir : {"compiler", "std", "tests"}) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root / dir, ec), end; it != end;
         it.increment(ec)) {
      if (ec) {
        break;
      }
      if (it->is_regular_file() && it->path().extension() == ".vx") {
        sources.push_back(it->path());
      }
    }
  }
  CHECK(sources.size() >= 10);

  vx::util::Arena arena;
  std::size_t total_files = 0;
  std::size_t total_lines = 0;
  std::size_t total_lexemes = 0;

  for (const auto &path : sources) {
    auto r = read_file(path);
    CHECK(r.has_value());
    if (!r) {
      continue;
    }
    const std::string &content = *r;
    CHECK(!content.empty());
    CHECK(content.size() == fs::file_size(path));

    // 逐行切分（模拟诊断行定位）
    auto lines = split(content, '\n');
    CHECK(!lines.empty());
    total_lines += lines.size();
    for (std::string_view ln : lines) {
      std::string_view t = trim(ln);
      if (t.empty()) {
        continue;
      }
      // 词素副本放入 Arena（模拟 token 存储），内容必须与源码视图一致
      auto *copy = arena.create<std::string>(t.substr(0, t.find(' ')));
      CHECK(!copy->empty());
      CHECK(std::string_view(*copy) == t.substr(0, t.find(' ')));
      ++total_lexemes;
    }
    ++total_files;
  }
  CHECK(total_files == sources.size());
  CHECK(total_lines > 0);
  CHECK(total_lexemes > 0);

  // 已知源码内容抽查
  auto lexer_src = read_file(root / "compiler" / "lexer.vx");
  CHECK(lexer_src.has_value());
  if (lexer_src) {
    CHECK(lexer_src->find("func tokenize") != std::string::npos);
  }

  // 最大源码文件回环：write_file → read_file 内容一致
  const fs::path *largest = &sources.front();
  for (const auto &p : sources) {
    if (fs::file_size(p) > fs::file_size(*largest)) {
      largest = &p;
    }
  }
  auto big = read_file(*largest);
  CHECK(big.has_value());
  if (big) {
    const fs::path out =
        fs::temp_directory_path() / "vx_util_vx_sources" / "roundtrip.vx";
    auto w = write_file(out, *big);
    CHECK(w.has_value());
    auto back = read_file(out);
    CHECK(back.has_value());
    if (back) {
      CHECK(*back == *big);
    }
    fs::remove_all(out.parent_path());
  }

  // 读目录应返回错误
  CHECK(!read_file(root / "compiler").has_value());
}

void test_platform() {
  const std::string arch = vx::util::host_arch();
  const std::string os = vx::util::host_os();
  const std::string triple = vx::util::host_triple();

  // 架构取值在已知集合内
  CHECK(arch == "arm64" || arch == "x86_64" || arch == "arm" ||
        arch == "i386" || arch == "riscv64" || arch == "unknown");
  // 系统取值在已知集合内
  CHECK(os == "macosx" || os == "linux" || os == "windows" || os == "unix" ||
        os == "unknown");

  // triple = arch-vendor-os 的规范拼接
  if (os == "macosx") {
    CHECK(triple == arch + "-apple-macosx");
  } else if (os == "linux") {
    // Linux 上 arm64 规范名为 aarch64
    CHECK(triple ==
          std::string(arch == "arm64" ? "aarch64" : arch) + "-unknown-linux-gnu");
  } else if (os == "windows") {
    CHECK(triple == arch + "-pc-windows-msvc");
  } else {
    CHECK(triple == arch + "-unknown-" + os);
  }

  // 人读名非空且含架构
  const std::string name = vx::util::host_name();
  CHECK(!name.empty());
  CHECK(name.find(arch) != std::string::npos);
}

} // namespace

int main() {
  test_arena();
  test_str();
  test_fs();
  test_vx_sources();
  test_platform();

  if (g_failures != 0) {
    std::fprintf(stderr, "%d 项检查失败\n", g_failures);
    return 1;
  }
  std::puts("Util 层测试全部通过");
  return 0;
}
