# Bash completion for vx
_vx_completions() {
    local cur="${COMP_WORDS[COMP_CWORD]}"
    # write-settings 的第二个参数是编辑器名
    if [[ "$COMP_CWORD" -eq 2 && "${COMP_WORDS[1]}" == "write-settings" ]]; then
        COMPREPLY=($(compgen -W "nvim neovim vim helix hx emacs --dry-run" -- "$cur"))
        return
    fi
    local commands="compile c link l build b run test fmt f lsp vmake-lsp clean write-settings"
    COMPREPLY=($(compgen -W "$commands" -- "$cur"))
}
complete -F _vx_completions vx
