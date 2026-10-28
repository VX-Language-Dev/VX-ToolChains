// main.cpp — vx: VX toolchain meta-command.
// Dispatches subcommands to the appropriate tool.
//
// Usage:
//   vx compile <source.vx> [options]   → vxc
//   vx link <input.vxobj>... [opts]   → vlnk
//   vx build [target...]              → vmake build
//   vx tidy <file.vx>...             → vx-tidy
//   vx lsp                           → vxls
//   vx vmake-lsp                     → vmake-lsp
//   vx --version                     → show toolchain version
//   vx --help                        → show help
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

static constexpr const char *kVersion = "1.12.0";

static std::string find_sibling(const std::string &name) {
    // Try next to this executable, searching up to 3 parent directories
    char buf[4096];
    uint32_t size = sizeof(buf);
    fs::path exe_dir;
#if defined(__APPLE__)
    if (_NSGetExecutablePath(buf, &size) == 0) {
        exe_dir = fs::path(buf).parent_path();
    }
#else
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        exe_dir = fs::path(buf).parent_path();
    }
#endif
    fs::path dir = exe_dir;
    for (int i = 0; i < 4; ++i) {
        fs::path cand = dir / name;
        if (fs::is_regular_file(cand)) return cand.string();
        if (!dir.has_parent_path()) break;
        fs::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    // Fallback: search PATH
    const char *path_env = std::getenv("PATH");
    if (path_env) {
        std::string p(path_env);
        size_t start = 0;
        while (start <= p.size()) {
            size_t end = p.find(':', start);
            std::string dir = (end == std::string::npos) ? p.substr(start) : p.substr(start, end - start);
            if (!dir.empty()) {
                fs::path cand = fs::path(dir) / name;
                if (fs::is_regular_file(cand)) return cand.string();
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    return name; // hope it's in PATH
}

static void print_help() {
    std::fputs(
        "vx — VX Toolchain\n"
        "Usage: vx <command> [options]\n"
        "\n"
        "Commands:\n"
        "  compile, c    Compile .vx source to .vxobj (vxc)\n"
        "  link, l       Link .vxobj to native executable (vlnk)\n"
        "  build, b      Build project (vmake)\n"
        "  tidy, t       Static analysis (vx-tidy)\n"
        "  lsp           Start VX language server (vxls)\n"
        "  vmake-lsp     Start VMake language server (vmake-lsp)\n"
        "  clean         Clean build directory (vmake clean)\n"
        "\n"
        "Options:\n"
        "  -V, --version  Print version\n"
        "  -h, --help     Show this help\n"
        "\n"
        "Environment:\n"
        "  VX_VXC, VX_VLNK  Override tool paths\n"
        "  VX_HEADERS       Override import library root\n",
        stdout);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_help();
        return 0;
    }

    std::string cmd = argv[1];

    if (cmd == "-h" || cmd == "--help") {
        print_help();
        return 0;
    }
    if (cmd == "-V" || cmd == "--version") {
        std::printf("vx %s\n", kVersion);
        return 0;
    }

    // Map subcommand to tool name
    std::string tool;
    std::vector<std::string> extra_args;

    if (cmd == "compile" || cmd == "c") {
        tool = "vxc";
    } else if (cmd == "link" || cmd == "l") {
        tool = "vlnk";
    } else if (cmd == "build" || cmd == "b") {
        tool = "vmake";
        extra_args.push_back("build");
    } else if (cmd == "clean") {
        tool = "vmake";
        extra_args.push_back("clean");
    } else if (cmd == "tidy" || cmd == "t") {
        tool = "vx-tidy";
    } else if (cmd == "lsp") {
        tool = "vxls";
    } else if (cmd == "vmake-lsp") {
        tool = "vmake-lsp";
    } else {
        std::fprintf(stderr, "error: unknown command: %s (try --help)\n", cmd.c_str());
        return 2;
    }

    // Find the tool
    std::string tool_path = find_sibling(tool);
    // Also check common install locations
    if (!fs::is_regular_file(tool_path)) {
        // Try ~/.local/bin
        const char *home = std::getenv("HOME");
        if (home) {
            fs::path cand = fs::path(home) / ".local" / "bin" / tool;
            if (fs::is_regular_file(cand)) tool_path = cand.string();
        }
    }

    // Build argv for exec
    std::vector<char *> new_argv;
    new_argv.push_back(const_cast<char *>(tool_path.c_str()));
    for (auto &a : extra_args) new_argv.push_back(const_cast<char *>(a.c_str()));
    for (int i = 2; i < argc; ++i) new_argv.push_back(argv[i]);
    new_argv.push_back(nullptr);

    execvp(tool_path.c_str(), new_argv.data());
    std::perror("execvp");
    return 127;
}