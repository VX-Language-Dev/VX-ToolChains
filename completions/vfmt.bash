# Bash completion for vfmt
_vfmt_completions() {
    local cur="${COMP_WORDS[COMP_CWORD]}"
    local opts="-w --write -c --check --indent --line-length --no-verify -h --help -V --version"
    COMPREPLY=($(compgen -W "$opts" -- "$cur"))
    if [[ "$cur" == -* ]]; then
        return
    fi
    COMPREPLY=($(compgen -f -X '!*.vx' -- "$cur"))
}
complete -F _vfmt_completions vfmt
