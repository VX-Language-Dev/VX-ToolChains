# VX Toolkit Installation Scripts

Two platform-specific installation scripts have been created to automate the build and installation process.

## Files Created

1. **unix-unixlike-install-vxtoolkit.bash** - For Unix and Unix-like systems (Linux, macOS, BSD, Solaris)
2. **nt-install-vxtoolkit.bat** - For Windows systems (Windows 7, 8, 10, 11, Server)

## Features

Both scripts implement the following functionality:

### 1. Platform Auto-Detection
- Automatically identifies the current operating system and architecture
- Unix script detects: Linux, macOS, FreeBSD, OpenBSD, NetBSD, Solaris
- Windows script detects: x86, x86_64, ARM64

### 2. Project Root Verification
- Checks for `Cargo.toml` in current directory
- Verifies `src/` directory exists
- Validates that the project is actually the VX Toolkit by checking for specific identifiers

### 3. Cargo Environment Check & Installation
- Detects if Cargo is already installed
- If installed: proceeds directly to build
- If not installed: automatically downloads and installs via rustup
  - Unix: Uses `curl` to download from https://sh.rustup.rs
  - Windows: Downloads rustup-init.exe from https://win.rustup.rs

### 4. Build Process
- Executes `cargo build --release` to compile all binaries
- Provides progress feedback and error handling

### 5. Artifact Installation
- Creates `toolkit/` directory in project root
- Moves all compiled binaries to `toolkit/`:
  - `vxcompiler` / `vxcompiler.exe`
  - `vxlinker` / `vxlinker.exe`
  - `vx_runtime` / `vx_runtime.exe`
  - `vpm` / `vpm.exe`
  - `vx-lsp` / `vx-lsp.exe`
  - `vxdbg` / `vxdbg.exe`
- Also moves library files:
  - Unix: `libvx_vm.so` / `libvx_vm.dylib`, `libvx_vm.a`
  - Windows: `vx_vm.dll`, `vx_vm.lib`

### 6. User Feedback
- Color-coded output (Unix) for better readability
- Clear success/error messages
- Post-installation summary with:
  - Installation location
  - PATH setup instructions
  - List of available commands

## Usage

### Unix/Linux/macOS

```bash
# Make the script executable (first time only)
chmod +x unix-unixlike-install-vxtoolkit.bash

# Run the installer
./unix-unixlike-install-vxtoolkit.bash
```

### Windows

```cmd
# Run from Command Prompt or PowerShell
nt-install-vxtoolkit.bat
```

## Error Handling

Both scripts include comprehensive error handling:

- **Project root not found**: Exits with clear message about running from correct directory
- **Cargo not found**: Automatically installs via rustup
- **Build failure**: Stops and displays compiler errors
- **Missing artifacts**: Warns about missing binaries but continues with available ones
- **Cleanup on error**: Unix script includes trap handler to clean up on failure

## Post-Installation

After successful installation, add the toolkit to your PATH:

### Unix/Linux/macOS
```bash
export PATH="$PATH:$(pwd)/toolkit"
```

### Windows
```cmd
set "PATH=%PATH%;%CD%\toolkit"
```

To make this permanent, add the export/set command to your shell profile or system environment variables.

## Requirements

- **Unix**: bash, curl (for Cargo installation)
- **Windows**: curl (for Cargo installation)
- Both scripts will install Cargo/Rust if not present

## Troubleshooting

### Unix Script
If you encounter permission errors:
```bash
chmod +x unix-unixlike-install-vxtoolkit.bash
```

### Windows Script
If Cargo is not found after installation, open a new Command Prompt window to refresh the PATH.

### Both Scripts
- Ensure you're in the project root directory (where Cargo.toml is located)
- Check that you have internet connectivity for Cargo installation
- Review error messages for specific build failures
