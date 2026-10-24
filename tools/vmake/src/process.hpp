// process.hpp — 后台子进程执行（fork/execvp）并捕获合并输出。
// VMake 自身不编译任何代码，只通过该模块调用 vxc / vlnk。
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace vmake {

struct ProcessResult {
  bool ok = false;
  int exit_code = -1;
  std::string output; // stdout + stderr（按写入顺序合并）
  std::string error;  // fork/exec 失败等启动错误
};

// 运行 program args...，环境变量以 env 覆盖（继承其余变量）。
ProcessResult
run_process(const std::string &program, const std::vector<std::string> &args,
            const std::vector<std::pair<std::string, std::string>> &env = {});

// 供 verbose 输出的命令行显示（对含特殊字符的参数加单引号）
std::string format_command(const std::string &program,
                           const std::vector<std::string> &args);

} // namespace vmake
