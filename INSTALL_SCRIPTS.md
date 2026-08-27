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
- Checks for `go.mod` in current directory
- Verifies `go/` directory exists

### 3. Go Environment Check & Installation
- Detects if Go is already installed
- If installed: proceeds directly to build
- If not installed: automatically downloads and installs Go 1.21+
  - Unix: Downloads from go.dev and installs to /usr/local/go
  - Windows: Downloads MSI installer or zip archive and extracts locally

### 4. Build Process
- Executes `cd go && make build` to compile all binaries
- Provides progress feedback and error handling

### 5. Artifact Installation
- Creates `toolkit/` directory in project root
- Moves all compiled Go binaries to `toolkit/`:
  - `vxc` / `vxc.exe` — VX 编译器
  - `vlnk` / `vlnk.exe` — 原生链接器
  - `vpm` / `vpm.exe` — 包管理器
  - `vxde` / `vxde.exe` — 反编译器
  - `vfmt` / `vfmt.exe` — 格式化工具

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
- **Go not found**: Automatically installs Go 1.21+
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

- **Unix**: bash, curl (for Go installation)
- **Windows**: curl (for Go installation)
- Both scripts will install Go if not present

## Troubleshooting

### Unix Script
If you encounter permission errors:
```bash
chmod +x unix-unixlike-install-vxtoolkit.bash
```

### Windows Script
If Go is not found after installation, add the Go binary directory to your system PATH.

### Both Scripts
- Ensure you're in the project root directory (where `Makefile` is located)
- Check that you have internet connectivity for Go installation
- Review error messages for specific build failures