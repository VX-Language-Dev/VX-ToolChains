// settings.cpp — `vx write-settings` 的设置块生成与幂等合并实现。
#include "settings.hpp"

#include <string>

namespace vx::settings {
namespace {

// 标记块边界。具体注释符由各编辑器块的首行/末行给出，这里只匹配内容。
constexpr std::string_view kBeginMark = ">>> vx write-settings >>>";
constexpr std::string_view kEndMark = "<<< vx write-settings <<<";

// 在既有内容末尾追加注册块（空行分隔，避免与原配置粘连）。
std::string append_block(std::string_view existing, std::string_view block) {
  std::string out(existing);
  if (!out.empty()) {
    if (out.back() != '\n')
      out.push_back('\n');
    out.push_back('\n');
  }
  out.append(block);
  return out;
}

} // namespace

std::optional<Editor> editor_from_name(std::string_view name) {
  if (name == "nvim" || name == "neovim")
    return Editor::Neovim;
  if (name == "vim")
    return Editor::Vim;
  if (name == "helix" || name == "hx")
    return Editor::Helix;
  if (name == "emacs")
    return Editor::Emacs;
  return std::nullopt;
}

std::string editor_name(Editor e) {
  switch (e) {
    case Editor::Neovim:
      return "neovim";
    case Editor::Vim:
      return "vim";
    case Editor::Helix:
      return "helix";
    case Editor::Emacs:
      return "emacs";
  }
  return "";
}

std::string settings_path(Editor e, std::string_view home) {
  const std::string base(home);
  switch (e) {
    case Editor::Neovim:
      return base + "/.config/nvim/init.lua";
    case Editor::Vim:
      return base + "/.vimrc";
    case Editor::Helix:
      return base + "/.config/helix/languages.toml";
    case Editor::Emacs:
      return base + "/.emacs.d/init.el";
  }
  return "";
}

std::string settings_block(Editor e) {
  switch (e) {
    case Editor::Neovim:
      return R"LUA(-- >>> vx write-settings >>>
-- VX 语言（.vx）：文件类型与注释符注册（由 `vx write-settings nvim` 写入）
vim.filetype.add({ extension = { vx = "vx" } })
vim.api.nvim_create_autocmd("FileType", {
  pattern = "vx",
  callback = function()
    vim.bo.commentstring = "# %s"
  end,
})
-- <<< vx write-settings <<<
)LUA";
    case Editor::Vim:
      return R"VIM(" >>> vx write-settings >>>
" VX 语言（.vx）：文件类型与注释符注册（由 `vx write-settings vim` 写入）
augroup vx_language
  autocmd!
  autocmd BufRead,BufNewFile *.vx setfiletype vx
  autocmd FileType vx setlocal commentstring=#\ %s
augroup END
" <<< vx write-settings <<<
)VIM";
    case Editor::Helix:
      return R"TOML(# >>> vx write-settings >>>
# VX 语言（.vx）：文件类型与注释符注册（由 `vx write-settings helix` 写入）
[[language]]
name = "vx"
scope = "source.vx"
file-types = ["vx"]
comment-token = "#"
indent = { tab-width = 4, unit = "    " }
# <<< vx write-settings <<<
)TOML";
    case Editor::Emacs:
      return R"EL(;; >>> vx write-settings >>>
;; VX 语言（.vx）：主模式注册（由 `vx write-settings emacs` 写入）
(define-derived-mode vx-mode prog-mode "VX"
  "Major mode for VX (adVanced eXecutable language) source files."
  (setq-local comment-start "# ")
  (setq-local comment-end ""))
(add-to-list 'auto-mode-alist '("\\.vx\\'" . vx-mode))
;; <<< vx write-settings <<<
)EL";
  }
  return "";
}

std::string merge_block(std::string_view existing, std::string_view block) {
  const std::size_t begin = existing.find(kBeginMark);
  if (begin != std::string_view::npos) {
    const std::size_t end = existing.find(kEndMark, begin);
    if (end != std::string_view::npos) {
      // 整块替换：从起止标记所在行的行首到结束标记所在行的行尾
      const std::size_t line_start = existing.rfind('\n', begin) + 1;
      std::size_t after = existing.find('\n', end);
      after = (after == std::string_view::npos) ? existing.size() : after + 1;
      std::string out;
      out.reserve(existing.size() + block.size());
      out.append(existing.substr(0, line_start));
      out.append(block);
      out.append(existing.substr(after));
      return out;
    }
  }
  return append_block(existing, block);
}

} // namespace vx::settings
