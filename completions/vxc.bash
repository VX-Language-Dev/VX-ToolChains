# Bash completion for vxc
_vxc_completions() {
    local opts="-o -I --check --dump -v --verbose --color --version -h --help"
    COMPREPLY=($(compgen -W "$opts" -- "${COMP_WORDS[COMP_CWORD]}"))
}
complete -F _vxc_completions vxc