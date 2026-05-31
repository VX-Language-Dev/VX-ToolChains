#!/usr/bin/env python3
"""
VX 浏览器检测模块 browser_detect v1.0
检测操作系统默认浏览器，格式化输出供 VX 外部调用。
支持 Linux / macOS / Windows 三大平台。

输出格式（JSON，一行一条记录便于 VX 逐行解析）：
    {"os": "<操作系统>", "browser": "<浏览器名>", "path": "<浏览器路径>", "status": "ok"}
或错误：
    {"error": "<错误信息>", "status": "error"}
"""

import json
import os
import platform
import shutil
import subprocess
import sys


def detect_linux_browser() -> dict:
    """在 Linux 上检测默认浏览器"""
    result = {"os": "Linux", "browser": "unknown", "path": ""}

    # 方法 1: xdg-settings get default-web-browser
    try:
        proc = subprocess.run(
            ["xdg-settings", "get", "default-web-browser"],
            capture_output=True, text=True, timeout=5
        )
        if proc.returncode == 0 and proc.stdout.strip():
            desktop_file = proc.stdout.strip()
            result["browser"] = desktop_file.replace(".desktop", "")
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass

    # 方法 2: xdg-mime query default x-scheme-handler/https
    if result["browser"] == "unknown":
        try:
            proc = subprocess.run(
                ["xdg-mime", "query", "default", "x-scheme-handler/https"],
                capture_output=True, text=True, timeout=5
            )
            if proc.returncode == 0 and proc.stdout.strip():
                result["browser"] = proc.stdout.strip().replace(".desktop", "")
        except (FileNotFoundError, subprocess.TimeoutExpired):
            pass

    # 方法 3: 检查常见浏览器是否在 PATH 中
    if result["browser"] == "unknown":
        common_browsers = [
            ("google-chrome-stable", "Google Chrome"),
            ("google-chrome", "Google Chrome"),
            ("chromium-browser", "Chromium"),
            ("chromium", "Chromium"),
            ("firefox", "Firefox"),
            ("firefox-esr", "Firefox ESR"),
            ("microsoft-edge-stable", "Microsoft Edge"),
            ("microsoft-edge", "Microsoft Edge"),
            ("opera", "Opera"),
            ("brave-browser", "Brave"),
            ("brave", "Brave"),
            ("vivaldi", "Vivaldi"),
        ]
        for bin_name, display_name in common_browsers:
            path = shutil.which(bin_name)
            if path:
                result["browser"] = display_name
                result["path"] = path
                break

    # 补充浏览器可执行文件路径
    if not result["path"] and result["browser"] != "unknown":
        browser_bin_map = {
            "Google Chrome": "google-chrome",
            "Chromium": "chromium-browser",
            "Firefox": "firefox",
            "Microsoft Edge": "microsoft-edge",
            "Opera": "opera",
            "Brave": "brave-browser",
            "Vivaldi": "vivaldi",
        }
        bin_name = browser_bin_map.get(result["browser"], "")
        if bin_name:
            result["path"] = shutil.which(bin_name) or ""

    return result


def detect_macos_browser() -> dict:
    """在 macOS 上检测默认浏览器"""
    result = {"os": "macOS", "browser": "unknown", "path": ""}

    try:
        # 读取 LaunchServices 默认 HTTP handler
        proc = subprocess.run(
            [
                "defaults", "read",
                "com.apple.LaunchServices/com.apple.launchservices.secure",
                "LSHandlers"
            ],
            capture_output=True, text=True, timeout=5
        )
        # macOS defaults read 输出是旧式 plist 格式，不好解析
        # 改用更可靠的方法
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass

    # 方法 2: 使用 Python 的 webbrowser 模块推断
    try:
        import webbrowser
        browser_name = webbrowser.get().name if webbrowser.get() else ""
        # 常见映射
        name_map = {
            "safari": "Safari",
            "chrome": "Google Chrome",
            "firefox": "Firefox",
            "edge": "Microsoft Edge",
            "opera": "Opera",
        }
        result["browser"] = name_map.get(browser_name, browser_name or "unknown")
    except Exception:
        pass

    # 方法 3: 按优先级检查常见浏览器
    if result["browser"] == "unknown":
        browser_list = [
            ("/Applications/Safari.app", "Safari"),
            ("/Applications/Google Chrome.app", "Google Chrome"),
            ("/Applications/Firefox.app", "Firefox"),
            ("/Applications/Microsoft Edge.app", "Microsoft Edge"),
            ("/Applications/Opera.app", "Opera"),
            ("/Applications/Brave Browser.app", "Brave"),
        ]
        for app_path, name in browser_list:
            if os.path.isdir(app_path):
                result["browser"] = name
                result["path"] = app_path
                break

    return result


def detect_windows_browser() -> dict:
    """在 Windows 上检测默认浏览器"""
    result = {"os": "Windows", "browser": "unknown", "path": ""}

    try:
        import winreg

        # 方法 1: 读取 HTTP 协议关联
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                            r"Software\Microsoft\Windows\Shell\Associations\UrlAssociations\http\UserChoice") as key:
            browser_id, _ = winreg.QueryValueEx(key, "ProgId")
    except Exception:
        browser_id = ""

    # 方法 2: 查询系统注册表
    if not browser_id:
        try:
            proc = subprocess.run(
                ["reg", "query",
                 r"HKEY_CURRENT_USER\Software\Microsoft\Windows\Shell\Associations\UrlAssociations\http\UserChoice",
                 "/v", "ProgId"],
                capture_output=True, text=True, timeout=5
            )
            for line in proc.stdout.splitlines():
                if "ProgId" in line:
                    browser_id = line.strip().split()[-1]
                    break
        except (FileNotFoundError, subprocess.TimeoutExpired):
            pass

    # 浏览器 ProgId 映射
    browser_map = {
        "ChromeHTML": "Google Chrome",
        "FirefoxURL": "Firefox",
        "MSEdgeHTM": "Microsoft Edge",
        "OperaStable": "Opera",
        "BraveHTML": "Brave",
        "VivaldiHTM": "Vivaldi",
        "IE.HTTP": "Internet Explorer",
    }
    result["browser"] = browser_map.get(browser_id, browser_id or "unknown")

    # 查找浏览器路径
    browser_paths = {
        "Google Chrome": r"C:\Program Files\Google\Chrome\Application\chrome.exe",
        "Firefox": r"C:\Program Files\Mozilla Firefox\firefox.exe",
        "Microsoft Edge": r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    }
    default_path = browser_paths.get(result["browser"], "")
    if default_path and os.path.isfile(default_path):
        result["path"] = default_path

    return result


def main():
    """主函数：检测默认浏览器并输出格式化结果"""
    try:
        system = platform.system()

        if system == "Linux":
            info = detect_linux_browser()
        elif system == "Darwin":
            info = detect_macos_browser()
        elif system == "Windows":
            info = detect_windows_browser()
        else:
            info = {
                "os": system,
                "browser": "unknown",
                "path": "",
            }

        # 补充 Python 版本信息
        info["python_version"] = platform.python_version()
        info["status"] = "ok" if info["browser"] != "unknown" else "partial"

        # 格式化输出：每行一条 JSON 记录
        print(json.dumps(info, ensure_ascii=False))
        sys.exit(0)

    except Exception as e:
        error_info = {
            "error": str(e),
            "status": "error",
            "os": platform.system(),
        }
        print(json.dumps(error_info, ensure_ascii=False), file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
