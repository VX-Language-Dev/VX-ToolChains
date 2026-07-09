#!/usr/bin/env bash
#
# VX Toolkit Installer for Unix / Unix-like Systems
# Supports: Linux, macOS, FreeBSD, OpenBSD, NetBSD, Solaris, etc.
#
set -euo pipefail

# ── Color helpers ────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m' # No Color

info()    { printf "${CYAN}[INFO]${NC}  %s\n" "$*"; }
success() { printf "${GREEN}[OK]${NC}    %s\n" "$*"; }
warn()    { printf "${YELLOW}[WARN]${NC}  %s\n" "$*"; }
error()   { printf "${RED}[ERROR]${NC} %s\n" "$*" >&2; }

# ── Cleanup on failure ──────────────────────────────────────────────────────
cleanup_on_error() {
    local exit_code=$?
    if [[ $exit_code -ne 0 ]]; then
        error "Installation failed (exit code: $exit_code)."
        error "Please review the error messages above and try again."
    fi
}
trap cleanup_on_error EXIT

# ── Step 1: Platform detection ──────────────────────────────────────────────
detect_platform() {
    local os arch
    os="$(uname -s)"
    arch="$(uname -m)"

    case "$os" in
        Linux*)   PLATFORM="linux"   ;;
        Darwin*)  PLATFORM="macos"   ;;
        FreeBSD*) PLATFORM="freebsd" ;;
        OpenBSD*) PLATFORM="openbsd" ;;
        NetBSD*)  PLATFORM="netbsd"  ;;
        SunOS*)   PLATFORM="solaris" ;;
        *)        PLATFORM="unknown" ;;
    esac

    case "$arch" in
        x86_64|amd64)   ARCH="x86_64"  ;;
        aarch64|arm64)  ARCH="aarch64" ;;
        armv7*)         ARCH="armv7"   ;;
        i686|i386)      ARCH="i686"    ;;
        *)              ARCH="$arch"   ;;
    esac

    info "Detected platform: ${BOLD}${os} (${arch})${NC}"
}

# ── Step 2: Locate or clone project repository ──────────────────────────────
REPO_URL="https://gitee.com/vx-language-dev/vx-tool-chains.git"
REPO_DIR="VX-ToolChains"

locate_or_clone_repo() {
    # If Cargo.toml exists in current directory, we are already in the project root
    if [[ -f "Cargo.toml" ]]; then
        success "Already in project root."
        return 0
    fi

    warn "Cargo.toml not found in current directory."

    # Check if the repo directory already exists alongside this script
    local script_dir
    script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

    if [[ -f "${script_dir}/Cargo.toml" ]]; then
        info "Project found at: ${script_dir}"
        cd "$script_dir"
        success "Switched to project root: $(pwd)"
        return 0
    fi

    # Check if the repo was already cloned in the current directory
    if [[ -d "${REPO_DIR}" && -f "${REPO_DIR}/Cargo.toml" ]]; then
        info "Found existing clone: ${REPO_DIR}/"
        cd "$REPO_DIR"
        success "Switched to project root: $(pwd)"
        return 0
    fi

    # Need to clone the repository
    info "Project source not found locally."
    info "Will clone from: ${REPO_URL}"

    if ! command -v git &>/dev/null; then
        error "git is not installed but is required to clone the repository."
        error "Please install git first, then re-run this script."
        exit 1
    fi

    info "Cloning VX Toolkit repository..."
    if ! git clone --depth 1 "$REPO_URL" "$REPO_DIR"; then
        error "Failed to clone repository."
        error "Please check your network connection and try again."
        exit 1
    fi

    cd "$REPO_DIR"
    success "Repository cloned and entered: $(pwd)"
}

# ── Step 3: Verify project root ─────────────────────────────────────────────
verify_project_root() {
    if [[ ! -f "src-zig/build.zig" ]]; then
        error "src-zig/build.zig not found after locating/cloning."
        error "Something went wrong. Please clone the repo manually."
        exit 1
    fi

    if [[ ! -d "src-zig/src" ]]; then
        error "'src-zig/src/' directory not found. This does not look like the project root."
        exit 1
    fi

    success "Project root verified."
}

# ── Step 3: Check & install Zig ─────────────────────────────────────────────
ensure_zig() {
    if command -v zig &>/dev/null; then
        local zig_ver
        zig_ver="$(zig version)"
        success "Zig is already installed: ${zig_ver}"
        return 0
    fi

    warn "Zig is not installed."

    # Check for curl (needed for Zig download)
    if ! command -v curl &>/dev/null; then
        error "curl is required to install Zig but was not found."
        error "Please install curl first, then re-run this script."
        exit 1
    fi

    info "Installing Zig 0.13+..."
    local zig_url
    case "$PLATFORM" in
        linux)
            case "$ARCH" in
                x86_64)  zig_url="https://ziglang.org/download/0.13.0/zig-linux-x86_64-0.13.0.tar.xz" ;;
                aarch64) zig_url="https://ziglang.org/download/0.13.0/zig-linux-aarch64-0.13.0.tar.xz" ;;
                *)       error "Unsupported architecture for Zig: $ARCH"; exit 1 ;;
            esac
            ;;
        macos)
            case "$ARCH" in
                x86_64)  zig_url="https://ziglang.org/download/0.13.0/zig-macos-x86_64-0.13.0.tar.xz" ;;
                aarch64) zig_url="https://ziglang.org/download/0.13.0/zig-macos-aarch64-0.13.0.tar.xz" ;;
                *)       error "Unsupported architecture for Zig: $ARCH"; exit 1 ;;
            esac
            ;;
        *)
            error "Automatic Zig installation is not supported for $PLATFORM."
            error "Please install Zig manually: https://ziglang.org/download/"
            exit 1
            ;;
    esac

    local tmp_dir
    tmp_dir="$(mktemp -d)"
    info "Downloading Zig from: $zig_url"
    curl -fSL "$zig_url" | tar -xJ -C "$tmp_dir"

    local zig_dir
    zig_dir="$(find "$tmp_dir" -maxdepth 1 -name 'zig-*' -type d | head -1)"
    if [[ -z "$zig_dir" ]]; then
        error "Failed to extract Zig archive."
        rm -rf "$tmp_dir"
        exit 1
    fi

    sudo mkdir -p /usr/local/bin
    sudo cp "$zig_dir/zig" /usr/local/bin/zig
    sudo chmod +x /usr/local/bin/zig
    rm -rf "$tmp_dir"

    if command -v zig &>/dev/null; then
        success "Zig installed successfully: $(zig version)"
    else
        error "Zig installation appeared to fail."
        exit 1
    fi
}

# ── Step 4: Build the project ───────────────────────────────────────────────
build_project() {
    info "Building VX Toolkit in release mode..."
    info "This may take several minutes on first build."

    cd src-zig
    if ! zig build -Doptimize=ReleaseSafe 2>&1; then
        error "Build failed. Please check the compiler errors above."
        exit 1
    fi
    cd ..

    success "Build completed successfully."
}

# ── Step 5: Move artifacts to toolkit/ ──────────────────────────────────────
install_artifacts() {
    local toolkit_dir="./toolkit"
    local release_dir="./src-zig/zig-out/bin"

    mkdir -p "$toolkit_dir"

    # List of expected binaries (Zig build outputs)
    local binaries=(
        "vxc"
        "vlnk"
        "vpm"
    )

    local moved=0
    local failed=0

    for bin in "${binaries[@]}"; do
        local src="${release_dir}/${bin}"
        if [[ -f "$src" ]]; then
            mv "$src" "$toolkit_dir/"
            chmod +x "$toolkit_dir/$bin"
            success "Installed: ${bin}"
            ((moved++))
        else
            warn "Binary not found: ${bin} (skipped)"
            ((failed++))
        fi
    done

    echo ""
    if [[ $moved -gt 0 ]]; then
        success "Installation complete! ${moved} artifact(s) moved to ${BOLD}${toolkit_dir}/${NC}"
    else
        error "No artifacts were found to install."
        exit 1
    fi

    if [[ $failed -gt 0 ]]; then
        warn "${failed} binary(ies) were not found in the build output."
    fi
}

# ── Step 6: Post-install summary ────────────────────────────────────────────
print_summary() {
    echo ""
    printf "${BOLD}══════════════════════════════════════════════════════════${NC}\n"
    printf "${GREEN}${BOLD}  VX Toolkit has been installed successfully!${NC}\n"
    printf "${BOLD}══════════════════════════════════════════════════════════${NC}\n"
    echo ""
    info "Installed binaries are located in: ${BOLD}./toolkit/${NC}"
    info "You can add them to your PATH with:"
    echo ""
    printf "    ${CYAN}export PATH=\"\$PATH:$(pwd)/toolkit\"${NC}\n"
    echo ""
    info "Available commands:"
    echo "    vxcompiler  - VX language compiler"
    echo "    vxlinker    - VX linker"
    echo "    vx_runtime  - VX runtime (VM)"
    echo "    vpm         - VX package manager"
    echo "    vx-lsp      - VX language server"
    echo "    vxdbg       - VX debugger"
    echo ""
    info "Platform: ${PLATFORM} (${ARCH})"
    echo ""
}

# ── Main ─────────────────────────────────────────────────────────────────────
main() {
    echo ""
    printf "${BOLD}  VX Toolkit Installer (Unix/Unix-like)${NC}\n"
    echo ""

    detect_platform
    locate_or_clone_repo
    verify_project_root
    ensure_zig
    build_project
    install_artifacts
    print_summary
}

main "$@"
