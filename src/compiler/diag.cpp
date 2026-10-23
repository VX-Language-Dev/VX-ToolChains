// diag.cpp — Compiler 层统一诊断基础设施实现。
#include "vx/compiler/diag.hpp"

#include "vx/util/str.hpp"

#include <algorithm>

namespace vx::compiler {

std::string_view severity_name(Severity s) {
  switch (s) {
    case Severity::Info:    return "info";
    case Severity::Warning: return "warning";
    case Severity::Error:   return "error";
  }
  return "error";
}

std::uint8_t severity_rank(Severity s) {
  return static_cast<std::uint8_t>(s);
}

Diagnostic make_diag(std::string_view code, Severity sev, std::string message,
                     std::string file, int line, int col) {
  Diagnostic d;
  d.code = std::string(code);
  d.severity = sev;
  d.message = std::move(message);
  d.file = std::move(file);
  d.line = line;
  d.col = col;
  return d;
}

Diagnostic make_diag_span(std::string_view code, Severity sev,
                          std::string message, std::string file, int line,
                          int col, int end_line, int end_col) {
  Diagnostic d = make_diag(code, sev, std::move(message), std::move(file), line,
                           col);
  d.end_line = end_line;
  d.end_col = end_col;
  return d;
}

void DiagBag::add(Diagnostic d) { diags_.push_back(std::move(d)); }

void DiagBag::add_all(const DiagBag &other) {
  diags_.insert(diags_.end(), other.diags_.begin(), other.diags_.end());
}

bool DiagBag::has_error() const {
  return std::any_of(diags_.begin(), diags_.end(), [](const Diagnostic &d) {
    return d.severity == Severity::Error;
  });
}

void DiagBag::sort() {
  std::stable_sort(diags_.begin(), diags_.end(),
                   [](const Diagnostic &a, const Diagnostic &b) {
                     if (a.file != b.file) return a.file < b.file;
                     if (a.line != b.line) return a.line < b.line;
                     if (a.col != b.col) return a.col < b.col;
                     return severity_rank(a.severity) > severity_rank(b.severity);
                   });
}

namespace {

constexpr std::string_view kReset = "\x1b[0m";
constexpr std::string_view kRed = "\x1b[31m";
constexpr std::string_view kYellow = "\x1b[33m";
constexpr std::string_view kCyan = "\x1b[36m";
constexpr std::string_view kBlue = "\x1b[34m";
constexpr std::string_view kBold = "\x1b[1m";

std::string_view severity_color(Severity s) {
  switch (s) {
    case Severity::Info:    return kCyan;
    case Severity::Warning: return kYellow;
    case Severity::Error:   return kRed;
  }
  return kRed;
}

// color=true 时以 color_code 包裹 text
std::string paint(std::string_view text, std::string_view color_code, bool color) {
  std::string s;
  if (color) {
    s += color_code;
    s += text;
    s += kReset;
  } else {
    s += text;
  }
  return s;
}

// 数字的十进制宽度（line=0 时至少为 1）
std::size_t num_width(int n) {
  std::size_t w = 1;
  for (int v = n; v >= 10; v /= 10) ++w;
  return w;
}

} // namespace

// source_of(file) 返回指定文件的源码文本，文件不存在时返回空视图。
std::string render_diagnostic(
    const Diagnostic &d,
    std::function<std::string_view(std::string_view file)> source_of,
    bool color) {
  std::string out;

  // 首行：<severity>[<code>]: <message>
  out += paint(severity_name(d.severity), severity_color(d.severity), color);
  if (!d.code.empty()) {
    out += '[';
    out += paint(d.code, kBold, color);
    out += ']';
  }
  out += ": ";
  out += d.message;
  out += '\n';

  // 位置行：  --> file:line[:col]
  if (!d.file.empty() && d.line > 0) {
    out += "  ";
    out += paint("-->", kBlue, color);
    out += ' ';
    out += d.file;
    out += ':';
    out += std::to_string(d.line);
    if (d.col > 0) {
      out += ':';
      out += std::to_string(d.col);
    }
    out += '\n';
  }

  // 源码摘录段
  std::string_view src_line;
  bool has_line = false;
  std::string_view source = source_of(d.file);
  if (!source.empty() && d.line > 0) {
    const auto lines = vx::util::split(source, '\n');
    if (static_cast<std::size_t>(d.line) <= lines.size()) {
      src_line = lines[d.line - 1];
      has_line = true;
    }
  }

  const std::string line_no = std::to_string(d.line);
  const std::string gutter(num_width(d.line), ' ');
  if (has_line) {
    out += gutter;
    out += " |\n";
    out += line_no;
    out += " | ";
    out += src_line;
    out += '\n';
    if (d.col > 0) {
      // 插入符定位：列按显示列计（与 d.col 约定一致）。
      // P1-6：起始缩进与插入符一律不越出本行行尾（col 越界时退化到行尾）。
      const std::size_t line_len = src_line.size();
      const std::size_t lead =
          std::min(static_cast<std::size_t>(d.col - 1), line_len);

      std::size_t caret = 1;
      if (d.end_line == d.line && d.end_col > d.col) {
        // 同行 span：高亮 [col, end_col)
        caret = static_cast<std::size_t>(d.end_col - d.col);
      } else if (d.end_line > d.line && line_len > lead) {
        // P1-5：跨行 span 退化——高亮到起始行的行尾
        caret = line_len - lead;
      }
      // 限定在本行可见宽度内
      const std::size_t room = line_len > lead ? line_len - lead : 0;
      if (room > 0 && caret > room) {
        caret = room;
      }

      out += gutter;
      out += " | ";
      out += std::string(lead, ' ');
      out += paint(std::string(caret, '^'), severity_color(d.severity), color);
      out += '\n';
    }
  }

  if (!d.note.empty()) {
    out += gutter;
    out += " = note: ";
    out += d.note;
    out += '\n';
  }

  return out;
}

// 单文件渲染：所有诊断共用同一份 source 文本。
std::string render_all(const DiagBag &bag, std::string_view source, bool color) {
  auto source_of = [source](std::string_view) { return source; };
  return render_all(bag, source_of, color);
}

// 向后兼容：单文件 render_diagnostic（source 视图）。
std::string render_diagnostic(const Diagnostic &d, std::string_view source,
                              bool color) {
  auto source_of = [source](std::string_view) { return source; };
  return render_diagnostic(d, source_of, color);
}

// 多文件渲染：按诊断的 file 字段分别取源码渲染。
std::string render_all(
    const DiagBag &bag,
    std::function<std::string_view(std::string_view file)> source_of,
    bool color) {
  DiagBag sorted = bag; // 副本排序，不改动调用方
  sorted.sort();

  std::string out;
  std::size_t errors = 0;
  std::size_t warnings = 0;
  for (const Diagnostic &d : sorted.items()) {
    if (d.severity == Severity::Error) {
      ++errors;
    } else if (d.severity == Severity::Warning) {
      ++warnings;
    }
    out += render_diagnostic(d, source_of, color);
  }

  if (errors > 0) {
    out += paint("error", kRed, color);
    out += ": 因 " + std::to_string(errors) + " 个错误中止编译";
    if (warnings > 0) {
      out += "（另有 " + std::to_string(warnings) + " 个警告）";
    }
    out += '\n';
  } else if (warnings > 0) {
    out += paint("warning", kYellow, color);
    out += ": " + std::to_string(warnings) + " 个警告\n";
  }
  return out;
}

} // namespace vx::compiler
