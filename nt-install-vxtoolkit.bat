@echo off
setlocal enabledelayedexpansion

::
:: VX Toolkit Installer for Windows (NT-based)
:: Supports: Windows 7, 8, 10, 11, Server editions
::

echo.
echo   VX Toolkit Installer (Windows)
echo.

:: ── Step 1: Platform detection ─────────────────────────────────────────────
echo [INFO] Detecting platform...

set "OS_NAME=%OS%"
set "ARCH_NAME=%PROCESSOR_ARCHITECTURE%"

if "%ARCH_NAME%"=="AMD64" (
    echo [INFO] Detected platform: Windows x86_64
) else if "%ARCH_NAME%"=="ARM64" (
    echo [INFO] Detected platform: Windows ARM64
) else if "%ARCH_NAME%"=="x86" (
    echo [INFO] Detected platform: Windows x86
) else (
    echo [WARN] Unknown architecture: %ARCH_NAME%
)

:: ── Step 2: Locate or clone project repository ─────────────────────────────
set "REPO_URL=https://gitee.com/vx-language-dev/vx-tool-chains.git"
set "REPO_DIR=VX-ToolChains"

echo [INFO] Locating or cloning repository...

:: Check if we're already in the project root
if exist "Cargo.toml" (
    echo [OK]   Already in project root.
    goto :verify_project
)

echo [WARN] Cargo.toml not found in current directory.

:: Check if the repo directory already exists in current directory
if exist "%REPO_DIR%\Cargo.toml" (
    echo [INFO] Found existing clone: %REPO_DIR%\
    cd /d "%REPO_DIR%"
    echo [OK]   Switched to project root: %CD%
    goto :verify_project
)

:: Need to clone the repository
echo [INFO] Project source not found locally.
echo [INFO] Will clone from: %REPO_URL%

where git >nul 2>&1
if errorlevel 1 (
    echo [ERROR] git is not installed but is required to clone the repository.
    echo [ERROR] Please install git first, then re-run this script.
    goto :error_exit
)

echo [INFO] Cloning VX Toolkit repository...
git clone --depth 1 "%REPO_URL%" "%REPO_DIR%"
if errorlevel 1 (
    echo [ERROR] Failed to clone repository.
    echo [ERROR] Please check your network connection and try again.
    goto :error_exit
)

cd /d "%REPO_DIR%"
echo [OK]   Repository cloned and entered: %CD%

:verify_project
:: ── Step 3: Verify project root ────────────────────────────────────────────
echo [INFO] Verifying project root...

if not exist "Cargo.toml" (
    echo [ERROR] Cargo.toml not found after locating/cloning.
    echo [ERROR] Something went wrong. Please clone the repo manually.
    goto :error_exit
)

if not exist "src\" (
    echo [ERROR] 'src\' directory not found. This does not look like the project root.
    goto :error_exit
)

:: Verify this is actually the VX project
findstr /C:"vx_language_toolkit" /C:"vxcompiler" /C:"vxlinker" Cargo.toml >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Cargo.toml does not appear to belong to the VX Toolkit project.
    goto :error_exit
)

echo [OK]   Project root verified.

:: ── Step 3: Check & install Cargo ──────────────────────────────────────────
echo [INFO] Checking for Cargo installation...

where cargo >nul 2>&1
if %errorlevel% equ 0 (
    for /f "tokens=*" %%v in ('cargo --version') do set CARGO_VER=%%v
    echo [OK]   Cargo is already installed: !CARGO_VER!
    goto :build_step
)

echo [WARN] Cargo is not installed.

:: Check for curl (needed for rustup)
where curl >nul 2>&1
if errorlevel 1 (
    echo [ERROR] curl is required to install Cargo but was not found.
    echo [ERROR] Please install curl first, then re-run this script.
    goto :error_exit
)

echo [INFO] Installing Cargo via rustup...
echo [INFO] This will run the official Rust installer in non-interactive mode.

curl --proto =https --tlsv1.2 -sSf https://win.rustup.rs -o rustup-init.exe
if errorlevel 1 (
    echo [ERROR] Failed to download rustup installer.
    goto :error_exit
)

echo [INFO] Running rustup installer...
rustup-init.exe -y --default-toolchain stable
if errorlevel 1 (
    echo [ERROR] Rust installation failed.
    del rustup-init.exe >nul 2>&1
    goto :error_exit
)

del rustup-init.exe >nul 2>&1

:: Refresh environment variables to pick up cargo path
call refreshenv >nul 2>&1

:: Try to source cargo env manually
set "CARGO_HOME=%USERPROFILE%\.cargo"
set "PATH=%CARGO_HOME%\bin;%PATH%"

where cargo >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Cargo installation appeared to fail.
    echo [ERROR] Try opening a new Command Prompt and running this script again.
    goto :error_exit
)

for /f "tokens=*" %%v in ('cargo --version') do set CARGO_VER=%%v
echo [OK]   Cargo installed successfully: !CARGO_VER!

:: ── Step 4: Build the project ──────────────────────────────────────────────
:build_step
echo [INFO] Building VX Toolkit in release mode...
echo [INFO] This may take several minutes on first build.

cargo build --release
if errorlevel 1 (
    echo [ERROR] Build failed. Please check the compiler errors above.
    goto :error_exit
)

echo [OK]   Build completed successfully.

:: ── Step 5: Move artifacts to toolkit\ ─────────────────────────────────────
echo [INFO] Installing artifacts to toolkit\...

if not exist "toolkit\" mkdir toolkit

set /a MOVED=0
set /a FAILED=0

:: List of expected binaries
for %%b in (vxcompiler.exe vxlinker.exe vx_runtime.exe vpm.exe vx-lsp.exe vxdbg.exe) do (
    if exist "target\release\%%b" (
        move /y "target\release\%%b" "toolkit\" >nul
        echo [OK]   Installed: %%b
        set /a MOVED+=1
    ) else (
        echo [WARN] Binary not found: %%b ^(skipped^)
        set /a FAILED+=1
    )
)

:: Also move the library if it exists
if exist "target\release\vx_vm.dll" (
    move /y "target\release\vx_vm.dll" "toolkit\" >nul
    echo [OK]   Installed: vx_vm.dll
    set /a MOVED+=1
)
if exist "target\release\vx_vm.lib" (
    move /y "target\release\vx_vm.lib" "toolkit\" >nul
    echo [OK]   Installed: vx_vm.lib
    set /a MOVED+=1
)

echo.
if %MOVED% gtr 0 (
    echo [OK]   Installation complete! %MOVED% artifact(s^) moved to toolkit\
) else (
    echo [ERROR] No artifacts were found to install.
    goto :error_exit
)

if %FAILED% gtr 0 (
    echo [WARN] %FAILED% binary(ies^) were not found in the build output.
)

:: ── Step 6: Post-install summary ───────────────────────────────────────────
echo.
echo ══════════════════════════════════════════════════════════
echo   VX Toolkit has been installed successfully!
echo ══════════════════════════════════════════════════════════
echo.
echo [INFO] Installed binaries are located in: .\toolkit\
echo [INFO] You can add them to your PATH with:
echo.
echo     set "PATH=%%PATH%%;%CD%\toolkit"
echo.
echo [INFO] Available commands:
echo     vxcompiler  - VX language compiler
echo     vxlinker    - VX linker
echo     vx_runtime  - VX runtime (VM)
echo     vpm         - VX package manager
echo     vx-lsp      - VX language server
echo     vxdbg       - VX debugger
echo.
echo [INFO] Platform: Windows (%ARCH_NAME%^)
echo.
goto :end

:: ── Error exit ─────────────────────────────────────────────────────────────
:error_exit
echo.
echo [ERROR] Installation failed.
echo [ERROR] Please review the error messages above and try again.
exit /b 1

:end
endlocal
exit /b 0
