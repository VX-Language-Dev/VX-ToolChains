"""vx_toolkit —— VX 工具链的 uv 安装包装。

本包不含任何编译逻辑：它只把 `uv tool install` 生成的命令转发给
随 wheel 分发的原生二进制（vxc / vlnk / vmake / vxls / vx-tidy / vx），
或转发给 `VX_TOOLKIT_BIN` / PATH 中已安装的同一套工具。
"""

__version__ = "1.15.0"

__all__ = ["__version__"]
