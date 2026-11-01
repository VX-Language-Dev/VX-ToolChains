// settings_test — `vx write-settings` 设置块生成与幂等合并测试（自带断言宏）。
#include "settings.hpp"

#include <cstdio>
#include <string>
#include <string_view>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                             \
      ++g_failures;                                                                                                    \
    }                                                                                                                  \
  } while (0)

using vx::settings::Editor;

std::size_t count_of(std::string_view haystack, std::string_view needle) {
  std::size_t n = 0;
  std::size_t pos = 0;
  while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
    ++n;
    pos += needle.size();
  }
  return n;
}

// ---- 用例 ----

void test_editor_names() {
  CHECK(vx::settings::editor_from_name("nvim") == Editor::Neovim);
  CHECK(vx::settings::editor_from_name("neovim") == Editor::Neovim);
  CHECK(vx::settings::editor_from_name("vim") == Editor::Vim);
  CHECK(vx::settings::editor_from_name("helix") == Editor::Helix);
  CHECK(vx::settings::editor_from_name("hx") == Editor::Helix);
  CHECK(vx::settings::editor_from_name("emacs") == Editor::Emacs);
  // 未知 / 空名
  CHECK(!vx::settings::editor_from_name("vscode"));
  CHECK(!vx::settings::editor_from_name(""));
}

void test_settings_paths() {
  const std::string home = "/home/user";
  CHECK(vx::settings::settings_path(Editor::Neovim, home) == "/home/user/.config/nvim/init.lua");
  CHECK(vx::settings::settings_path(Editor::Vim, home) == "/home/user/.vimrc");
  CHECK(vx::settings::settings_path(Editor::Helix, home) == "/home/user/.config/helix/languages.toml");
  CHECK(vx::settings::settings_path(Editor::Emacs, home) == "/home/user/.emacs.d/init.el");
}

void test_blocks_register_vx() {
  // 每个块都必须含起止标记，并以对应编辑器语法注册 .vx
  for (const Editor e : {Editor::Neovim, Editor::Vim, Editor::Helix, Editor::Emacs}) {
    const std::string block = vx::settings::settings_block(e);
    CHECK(!block.empty());
    CHECK(block.find(">>> vx write-settings >>>") != std::string::npos);
    CHECK(block.find("<<< vx write-settings <<<") != std::string::npos);
    CHECK(!vx::settings::editor_name(e).empty());
  }

  CHECK(vx::settings::settings_block(Editor::Neovim).find("vim.filetype.add") != std::string::npos);
  CHECK(vx::settings::settings_block(Editor::Neovim).find("\"vx\"") != std::string::npos);

  CHECK(vx::settings::settings_block(Editor::Vim).find("*.vx") != std::string::npos);
  CHECK(vx::settings::settings_block(Editor::Vim).find("setfiletype vx") != std::string::npos);

  CHECK(vx::settings::settings_block(Editor::Helix).find("file-types = [\"vx\"]") != std::string::npos);
  CHECK(vx::settings::settings_block(Editor::Helix).find("comment-token = \"#\"") != std::string::npos);

  CHECK(vx::settings::settings_block(Editor::Emacs).find("vx-mode") != std::string::npos);
  CHECK(vx::settings::settings_block(Editor::Emacs).find("\\.vx") != std::string::npos);
}

void test_merge_into_empty() {
  // 空文件：结果就是注册块本身
  const std::string block = vx::settings::settings_block(Editor::Vim);
  CHECK(vx::settings::merge_block("", block) == block);
}

void test_merge_appends_existing() {
  const std::string block = vx::settings::settings_block(Editor::Helix);
  const std::string existing = "theme = \"dark\"\n";
  const std::string merged = vx::settings::merge_block(existing, block);
  // 既有内容保留在前，注册块位于其后，且只有一个标记块
  CHECK(merged.rfind(existing, 0) == 0);
  CHECK(merged.find(">>> vx write-settings >>>") != std::string::npos);
  CHECK(count_of(merged, ">>> vx write-settings >>>") == 1);
  CHECK(count_of(merged, "<<< vx write-settings <<<") == 1);
}

void test_merge_is_idempotent() {
  // 重复写入：结果与写一次相同（不重复追加标记块）
  for (const Editor e : {Editor::Neovim, Editor::Vim, Editor::Helix, Editor::Emacs}) {
    const std::string block = vx::settings::settings_block(e);
    const std::string once = vx::settings::merge_block("", block);
    const std::string twice = vx::settings::merge_block(once, block);
    CHECK(twice == once);
  }
}

void test_merge_replaces_stale_block() {
  // 已存在旧块（块内内容过期）时整块替换，而非追加第二份
  const std::string block = vx::settings::settings_block(Editor::Neovim);
  const std::string existing = "vim.o.number = true\n"
                               "-- >>> vx write-settings >>>\n"
                               "-- stale content\n"
                               "-- <<< vx write-settings <<<\n";
  const std::string merged = vx::settings::merge_block(existing, block);
  CHECK(merged.find("stale content") == std::string::npos);
  CHECK(merged.rfind("vim.o.number = true", 0) == 0);
  CHECK(count_of(merged, ">>> vx write-settings >>>") == 1);
  CHECK(merged.find("vim.filetype.add") != std::string::npos);
}

} // namespace

int main() {
  test_editor_names();
  test_settings_paths();
  test_blocks_register_vx();
  test_merge_into_empty();
  test_merge_appends_existing();
  test_merge_is_idempotent();
  test_merge_replaces_stale_block();

  if (g_failures == 0) {
    std::fputs("vx settings tests: all passed\n", stdout);
    return 0;
  }
  std::fprintf(stderr, "vx settings tests: %d failures\n", g_failures);
  return 1;
}
