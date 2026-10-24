// process.cpp — fork/execvp 子进程 + 临时日志文件捕获输出。
#include "process.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace vmake {
namespace {

namespace fs = std::filesystem;

bool needs_quote(const std::string &s) {
  if (s.empty())
    return true;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '"' || c == '\'' ||
        c == '\\' || c == '$' || c == '`' || c == '&' || c == '|' || c == ';' ||
        c == '<' || c == '>') {
      return true;
    }
  }
  return false;
}

std::string quote_arg(const std::string &s) {
  if (!needs_quote(s))
    return s;
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}

} // namespace

std::string format_command(const std::string &program,
                           const std::vector<std::string> &args) {
  std::string cmd = quote_arg(program);
  for (const std::string &a : args)
    cmd += " " + quote_arg(a);
  return cmd;
}

ProcessResult
run_process(const std::string &program, const std::vector<std::string> &args,
            const std::vector<std::pair<std::string, std::string>> &env) {
  ProcessResult r;

  // 临时日志文件：stdout/stderr 共用，避免管道死锁并保留输出顺序
  std::string tmpl = (fs::temp_directory_path() / "vmake_log_XXXXXX").string();
  std::vector<char> tmpl_buf(tmpl.begin(), tmpl.end());
  tmpl_buf.push_back('\0');
  const int fd = ::mkstemp(tmpl_buf.data());
  if (fd < 0) {
    r.error = "无法创建临时日志文件";
    return r;
  }
  const std::string log_path(tmpl_buf.data());

  std::vector<std::string> cmd;
  cmd.reserve(args.size() + 1);
  cmd.push_back(program);
  cmd.insert(cmd.end(), args.begin(), args.end());
  std::vector<char *> argv;
  argv.reserve(cmd.size() + 1);
  for (std::string &s : cmd)
    argv.push_back(s.data());
  argv.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(fd);
    ::unlink(log_path.c_str());
    r.error = "fork 失败";
    return r;
  }
  if (pid == 0) {
    ::dup2(fd, STDOUT_FILENO);
    ::dup2(fd, STDERR_FILENO);
    ::close(fd);
    for (const auto &[key, value] : env) {
      ::setenv(key.c_str(), value.c_str(), 1);
    }
    ::execvp(cmd[0].c_str(), argv.data());
    ::_exit(127); // exec 失败
  }

  ::close(fd);
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    // 被信号中断则重试
  }

  {
    std::ifstream in(log_path, std::ios::binary);
    if (in) {
      r.output.assign((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
    }
  }
  ::unlink(log_path.c_str());

  if (WIFEXITED(status)) {
    r.exit_code = WEXITSTATUS(status);
    r.ok = r.exit_code == 0;
    if (r.exit_code == 127 && r.output.empty()) {
      r.error = "无法执行 " + program;
    }
  } else {
    r.exit_code = -1;
    r.error = program + " 被信号终止";
  }
  return r;
}

} // namespace vmake
