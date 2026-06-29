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
    if [[ ! -f "Cargo.toml" ]]; then
        error "Cargo.toml not found after locating/cloning."
        error "Something went wrong. Please clone the repo manually."
        exit 1
    fi

    if [[ ! -d "src" ]]; then
        error "'src/' directory not found. This does not look like the project root."
        exit 1
    fi

    # Verify this is actually the VX project
    if ! grep -q 'vx_language_toolkit\|vxcompiler\|vxlinker' Cargo.toml 2>/dev/null; then
        error "Cargo.toml does not appear to belong to the VX Toolkit project."
        exit 1
    fi

    success "Project root verified."
}

# ── Step 3: Check & install Cargo ───────────────────────────────────────────
ensure_cargo() {
    if command -v cargo &>/dev/null; then
        local cargo_ver
        cargo_ver="$(cargo --version)"
        success "Cargo is already installed: ${cargo_ver}"
        return 0
    fi

    warn "Cargo is not installed."

    # Check for curl (needed for rustup)
    if ! command -v curl &>/dev/null; then
        error "curl is required to install Cargo but was not found."
        error "Please install curl first, then re-run this script."
        exit 1
    fi

    info "Installing Cargo via rustup..."
    info "This will run the official Rust installer in non-interactive mode."

    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y

    # Source the cargo environment so 'cargo' is available in this session
    if [[ -f "$HOME/.cargo/env" ]]; then
        # shellcheck disable=SC1091
        source "$HOME/.cargo/env"
    fi

    if command -v cargo &>/dev/null; then
        success "Cargo installed successfully: $(cargo --version)"
    else
        error "Cargo installation appeared to fail."
        error "Try opening a new terminal and running this script again."
        exit 1
    fi
}

# ── Step 4: Build the project ───────────────────────────────────────────────
build_project() {
    info "Building VX Toolkit in release mode..."
    info "This may take several minutes on first build."

    if ! cargo build --release 2>&1; then
        error "Build failed. Please check the compiler errors above."
        exit 1
    fi

    success "Build completed successfully."
}

# ── Step 5: Move artifacts to toolkit/ ──────────────────────────────────────
install_artifacts() {
    local toolkit_dir="./toolkit"
    local release_dir="./target/release"

    mkdir -p "$toolkit_dir"

    # List of expected binaries
    local binaries=(
        "vxcompiler"
        "vxlinker"
        "vx_runtime"
        "vpm"
        "vx-lsp"
        "vxdbg"
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

    # Also move the library if it exists
    local lib_ext
    case "$PLATFORM" in
        macos)  lib_ext="dylib" ;;
        *)      lib_ext="so"    ;;
    esac

    for lib in "${release_dir}"/libvx_vm."${lib_ext}" "${release_dir}"/libvx_vm.a; do
        if [[ -f "$lib" ]]; then
            mv "$lib" "$toolkit_dir/"
            success "Installed: $(basename "$lib")"
            ((moved++))
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
    ensure_cargo
    build_project
    install_artifacts
    print_summary
}

main "$@"
