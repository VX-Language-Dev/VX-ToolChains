// settings.hpp — `vx write-settings`：把 VX 语言注册写入编辑器设置文件。
// 只负责生成注册块与幂等合并文本，文件读写由调用方完成（便于单测）。
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace vx::settings {

// 受支持的编辑器
enum class Editor { Neovim, Vim, Helix, Emacs };

// 解析命令行里的编辑器名（nvim/neovim、vim、helix/hx、emacs）；未知返回 nullopt。
std::optional<Editor> editor_from_name(std::string_view name);

// 规范名（诊断输出用）："neovim" / "vim" / "helix" / "emacs"。
std::string editor_name(Editor e);

// 该编辑器的用户设置文件路径（home 为 $HOME 的值）。
std::string settings_path(Editor e, std::string_view home);

// 要写入设置文件的注册块：以标记行开始、结束，内容为该编辑器语法。
std::string settings_block(Editor e);

// 幂等合并：已存在标记块则整块替换，否则在既有内容末尾追加（必要时补空行）。
// 重复对同一内容调用两次，结果与调用一次相同。
std::string merge_block(std::string_view existing, std::string_view block);

} // namespace vx::settings
