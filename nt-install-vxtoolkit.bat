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

if not exist "src-zig\build.zig" (
    echo [ERROR] src-zig\build.zig not found after locating/cloning.
    echo [ERROR] Something went wrong. Please clone the repo manually.
    goto :error_exit
)

if not exist "src-zig\src\" (
    echo [ERROR] 'src-zig\src\' directory not found. This does not look like the project root.
    goto :error_exit
)

echo [OK]   Project root verified.

:: ── Step 3: Check & install Zig ───────────────────────────────────────────
echo [INFO] Checking for Zig installation...

where zig >nul 2>&1
if %errorlevel% equ 0 (
    for /f "tokens=*" %%v in ('zig version') do set ZIG_VER=%%v
    echo [OK]   Zig is already installed: !ZIG_VER!
    goto :build_step
)

echo [WARN] Zig is not installed.

:: Check for curl
where curl >nul 2>&1
if errorlevel 1 (
    echo [ERROR] curl is required to install Zig but was not found.
    echo [ERROR] Please install Zig manually from: https://ziglang.org/download/
    goto :error_exit
)

echo [INFO] Downloading Zig 0.13+ for Windows...

if "%ARCH_NAME%"=="AMD64" (
    set "ZIG_URL=https://ziglang.org/download/0.13.0/zig-windows-x86_64-0.13.0.zip"
) else if "%ARCH_NAME%"=="ARM64" (
    echo [ERROR] Automatic Zig installation is not supported for Windows ARM64.
    echo [ERROR] Please install Zig manually from: https://ziglang.org/download/
    goto :error_exit
) else (
    echo [ERROR] Unsupported architecture: %ARCH_NAME%
    goto :error_exit
)

curl -fSL "%ZIG_URL%" -o zig-install.zip
if errorlevel 1 (
    echo [ERROR] Failed to download Zig.
    goto :error_exit
)

echo [INFO] Extracting Zig...
tar -xf zig-install.zip
if errorlevel 1 (
    echo [ERROR] Failed to extract Zig archive.
    del zig-install.zip >nul 2>&1
    goto :error_exit
)

:: Find the extracted directory
for /d %%d in (zig-*) do set "ZIG_DIR=%%d"

:: Add to PATH for this session
set "PATH=%CD%\%ZIG_DIR%;%PATH%"

where zig >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Zig installation appeared to fail.
    goto :error_exit
)

for /f "tokens=*" %%v in ('zig version') do set ZIG_VER=%%v
echo [OK]   Zig installed successfully: !ZIG_VER!
echo [INFO] Note: You may need to add %ZIG_DIR% to your system PATH permanently.

:: ── Step 4: Build the project ──────────────────────────────────────────────
:build_step
echo [INFO] Building VX Toolkit in release mode...
echo [INFO] This may take several minutes on first build.

cd src-zig
zig build -Doptimize=ReleaseSafe
if errorlevel 1 (
    echo [ERROR] Build failed. Please check the compiler errors above.
    cd ..
    goto :error_exit
)
cd ..

echo [OK]   Build completed successfully.

:: ── Step 5: Move artifacts to toolkit\ ─────────────────────────────────────
echo [INFO] Installing artifacts to toolkit\...

if not exist "toolkit\" mkdir toolkit

set /a MOVED=0
set /a FAILED=0

:: List of expected binaries (Zig build outputs)
for %%b in (vxc.exe vlnk.exe vpm.exe) do (
    if exist "src-zig\zig-out\bin\%%b" (
        move /y "src-zig\zig-out\bin\%%b" "toolkit\" >nul
        echo [OK]   Installed: %%b
        set /a MOVED+=1
    ) else (
        echo [WARN] Binary not found: %%b ^(skipped^)
        set /a FAILED+=1
    )
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
echo     vxc         - VX language compiler (Zig)
echo     vlnk        - VX native linker (Zig)
echo     vpm         - VX package manager
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
