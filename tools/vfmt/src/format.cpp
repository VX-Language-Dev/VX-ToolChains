// format.cpp — vfmt 格式化核心实现。
//
// 策略：基于 Lexer 的 token 流重建缩进结构与空格布局；注释由独立的源码行
// 扫描器提取并按原位插回。
//
// 相比「AST → 源码」重建，token 路线保留了 AST 会丢失的信息：
//   - var / mut / const 关键字原文（AST 的 VarDecl 只区分 is_mut，const 与 var 不可分）
//   - import ... as 别名（AST 的 Import 节点不含 alias）
//   - 字符串原始转义形式（AST 的 StrLit 只存解码后的值）
// 因此本格式化器不会因 AST 字段缺失而改写源码语义。
//
// 格式化结果再经 Lexer + Parser 复解析，与原 AST 结构指纹比对，
// 确保结构等价后才输出。
#include "format.hpp"

#include "verify.hpp"

#include "vx/compiler/diag.hpp"
#include "vx/compiler/lexer.hpp"
#include "vx/compiler/parser.hpp"
#include "vx/util/arena.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vfmt {
namespace {

using vx::compiler::DiagBag;
using vx::compiler::Program;
using vx::compiler::Tok;
using vx::compiler::Token;

// ===== 源码行扫描 =====

// 源码的物理行信息。Lexer's token 流不含注释与空行（空行与整行注释在
// 词法阶段被跳过），故需独立扫描源码补齐。
struct SrcLine {
  bool blank = false;            // 纯空白行
  bool has_code = false;         // 含代码
  bool has_comment = false;      // 含注释
  bool comment_own_line = false; // 注释独占一行（之前仅有空白）
  std::string comment;           // 注释文本（含 '#'，已去行尾空白）
};

// 逐行扫描源码：识别空白行、代码行与注释。
// 字符串跳过规则与 Lexer 一致（字符串可跨行，转义序列以 '\' 引导）。
std::vector<SrcLine> scan_lines(std::string_view src) {
  std::vector<SrcLine> lines;
  bool in_string = false;
  char quote = '\0';

  std::size_t pos = 0;
  while (true) {
    std::size_t line_end = src.find('\n', pos);
    const bool last = (line_end == std::string_view::npos);
    if (last) {
      line_end = src.size();
    }
    std::string_view raw = src.substr(pos, line_end - pos);
    if (!raw.empty() && raw.back() == '\r') {
      raw.remove_suffix(1);
    }

    SrcLine sl;
    if (in_string) {
      // 字符串延续行：整行属于字符串内容，不参与注释/空行判定。
      std::size_t j = 0;
      while (j < raw.size()) {
        if (raw[j] == '\\') {
          j += 2;
          continue;
        }
        if (raw[j] == quote) {
          in_string = false;
          ++j;
          break;
        }
        ++j;
      }
      sl.has_code = true;
      lines.push_back(std::move(sl));
    } else {
      std::size_t j = 0;
      while (j < raw.size() && (raw[j] == ' ' || raw[j] == '\t')) {
        ++j;
      }
      bool saw_code = false;
      while (j < raw.size()) {
        const char c = raw[j];
        if (c == '"' || c == '\'') {
          const char q = c;
          ++j;
          bool closed = false;
          while (j < raw.size()) {
            if (raw[j] == '\\') {
              j += 2;
              continue;
            }
            if (raw[j] == q) {
              ++j;
              closed = true;
              break;
            }
            ++j;
          }
          if (!closed) {
            in_string = true;
            break;
          }
          saw_code = true;
          continue;
        }
        if (c == '#') {
          sl.has_comment = true;
          sl.comment_own_line = !saw_code;
          std::string_view t = raw.substr(j);
          while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) {
            t.remove_suffix(1);
          }
          sl.comment.assign(t);
          break;
        }
        if (c == ' ' || c == '\t') {
          ++j;
          continue;
        }
        saw_code = true;
        ++j;
      }
      sl.has_code = saw_code;
      sl.blank = !saw_code && !sl.has_comment;
      lines.push_back(std::move(sl));
    }

    if (last) {
      break;
    }
    pos = line_end + 1;
  }
  return lines;
}

// ===== token 渲染 =====

// 字符串重新编码：统一使用双引号并转义。
// 注意 Lexer 不解码 "\r"（未知转义保留原字符），故真实 CR 只能原样输出。
std::string quote_string(std::string_view v) {
  std::string out = "\"";
  for (const char c : v) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += c;
        break;
    }
  }
  out += '"';
  return out;
}

std::string token_text(const Token& t) {
  switch (t.kind) {
    case Tok::Ident:
    case Tok::Int:
    case Tok::Float:
    case Tok::Kw:
      return std::string(t.text);
    case Tok::Str:
      return quote_string(t.text);
    case Tok::And:
      return "and";
    case Tok::Or:
      return "or";
    case Tok::Not:
      return "!";
    case Tok::Plus:
      return "+";
    case Tok::Minus:
      return "-";
    case Tok::Star:
      return "*";
    case Tok::Slash:
      return "/";
    case Tok::Percent:
      return "%";
    case Tok::Power:
      return "^";
    case Tok::Eq:
      return "==";
    case Tok::Ne:
      return "!=";
    case Tok::Lt:
      return "<";
    case Tok::Gt:
      return ">";
    case Tok::Le:
      return "<=";
    case Tok::Ge:
      return ">=";
    case Tok::Assign:
      return "=";
    case Tok::PlusEq:
      return "+=";
    case Tok::MinusEq:
      return "-=";
    case Tok::StarEq:
      return "*=";
    case Tok::SlashEq:
      return "/=";
    case Tok::Arrow:
      return "->";
    case Tok::LParen:
      return "(";
    case Tok::RParen:
      return ")";
    case Tok::LBracket:
      return "[";
    case Tok::RBracket:
      return "]";
    case Tok::LBrace:
      return "{";
    case Tok::RBrace:
      return "}";
    case Tok::Colon:
      return ":";
    case Tok::Comma:
      return ",";
    case Tok::Dot:
      return ".";
    case Tok::Semi:
      return ";";
    case Tok::At:
      return "@";
    default:
      return {};
  }
}

// token 是否「结束一个值」：用于区分一元 / 二元运算符。
bool ends_value(const Token* t) {
  switch (t->kind) {
    case Tok::Ident:
    case Tok::Int:
    case Tok::Float:
    case Tok::Str:
    case Tok::RParen:
    case Tok::RBracket:
    case Tok::RBrace:
      return true;
    default:
      return false;
  }
}

bool is_operator_token(const Token* t) {
  switch (t->kind) {
    case Tok::Plus:
    case Tok::Minus:
    case Tok::Star:
    case Tok::Slash:
    case Tok::Percent:
    case Tok::Power:
    case Tok::Eq:
    case Tok::Ne:
    case Tok::Lt:
    case Tok::Gt:
    case Tok::Le:
    case Tok::Ge:
    case Tok::Assign:
    case Tok::PlusEq:
    case Tok::MinusEq:
    case Tok::StarEq:
    case Tok::SlashEq:
    case Tok::Arrow:
    case Tok::And:
    case Tok::Or:
    case Tok::Not:
      return true;
    default:
      return false;
  }
}

// 是否前缀（一元）运算符：'-' / '+' / '!'。
// '-' 与 '+' 依前一个 token 判定：前一个结束一个值则为中缀（二元）。
bool is_prefix_op(const std::vector<const Token*>& toks, std::size_t j) {
  const Token* t = toks[j];
  if (t->kind == Tok::Not) {
    return true;
  }
  if (t->kind != Tok::Minus && t->kind != Tok::Plus) {
    return false;
  }
  if (j == 0) {
    return true;
  }
  return !ends_value(toks[j - 1]);
}

// token 是否可以作为「被调用者 / 下标宿主」的结尾。
// 关键字也可作函数名或方法名（如 func new()），故与 ends_value 分开判定。
bool is_callee_end(const Token* t) {
  if (t->kind == Tok::Kw) {
    return true;
  }
  return ends_value(t);
}

// 控制流关键字：其后接 '(' 时保留一个空格（if (x):），区别于函数调用 f(x)。
bool is_control_kw(const Token* t) {
  if (t->kind != Tok::Kw) {
    return false;
  }
  const std::string_view w = t->text;
  return w == "if" || w == "elif" || w == "else" || w == "while" || w == "for" || w == "return" || w == "not" ||
         w == "and" || w == "or" || w == "in";
}

// 当前是否处于 @imp / @cppimp 的库引用括号内（路径紧凑书写，无空格）。
bool in_import_path(const std::vector<bool>& import_stack) {
  return !import_stack.empty() && import_stack.back();
}

// 判断两个相邻 token 之间是否插入空格。
bool need_space(const std::vector<const Token*>& toks, std::size_t i, const std::vector<bool>& import_stack) {
  const Token* p = toks[i - 1];
  const Token* c = toks[i];

  // @imp(路径) / @cppimp(路径)：路径内紧凑书写
  if (in_import_path(import_stack)) {
    if (c->kind == Tok::Slash || c->kind == Tok::Dot || c->kind == Tok::Minus) {
      return false;
    }
    if (p->kind == Tok::Slash || p->kind == Tok::Dot) {
      return false;
    }
  }

  // 右闭合符、点、逗号、冒号、分号：紧贴前一个 token
  switch (c->kind) {
    case Tok::RParen:
    case Tok::RBracket:
    case Tok::RBrace:
    case Tok::Dot:
    case Tok::Comma:
    case Tok::Colon:
    case Tok::Semi:
      return false;
    default:
      break;
  }

  // 左括号：函数名 / 下标紧贴（被调用者），运算符、赋值与控制流关键字后留空格
  switch (c->kind) {
    case Tok::LParen:
    case Tok::LBracket:
      return !is_callee_end(p) || is_control_kw(p);
    case Tok::LBrace:
      // 字典字面量的 '{'：紧贴左括号 / 中括号，其余留一个空格
      return p->kind != Tok::LParen && p->kind != Tok::LBracket && p->kind != Tok::LBrace;
    default:
      break;
  }

  // 左括号、点之后：紧贴
  switch (p->kind) {
    case Tok::LParen:
    case Tok::LBracket:
    case Tok::LBrace:
    case Tok::Dot:
      return false;
    default:
      break;
  }

  // 逗号、冒号、分号之后：一个空格
  if (p->kind == Tok::Comma || p->kind == Tok::Colon || p->kind == Tok::Semi) {
    return true;
  }

  // 前缀运算符：与操作数紧贴；与另一个运算符相邻时留空格避免粘连
  if (is_prefix_op(toks, i)) {
    return is_operator_token(p);
  }
  // 前缀运算符与其操作数之间：紧贴
  if (is_prefix_op(toks, i - 1)) {
    return false;
  }

  // @imp / @cppimp：'@' 与注解名紧贴
  if (p->kind == Tok::At) {
    return false;
  }

  // 其余情形：一律以一个空格分隔
  return true;
}

// 渲染器：维护跨物理行的括号栈与注解状态。
// 括号内的换行不产生 token 边界，故状态需在物理行分组之间延续。
class Renderer {
public:
  // 渲染一个物理行分组：token 间插入规范化空格。
  std::string render(const std::vector<const Token*>& toks) {
    std::string out;
    for (std::size_t i = 0; i < toks.size(); ++i) {
      const Token& t = *toks[i];
      if (i > 0 && need_space(toks, i, import_stack_)) {
        out += ' ';
      }
      out += token_text(t);

      switch (t.kind) {
        case Tok::At:
          at_pending_ = true;
          break;
        case Tok::Ident:
          break; // 注解名（@imp 的 imp），保持 at_pending
        case Tok::LParen:
          import_stack_.push_back(at_pending_);
          at_pending_ = false;
          break;
        case Tok::LBracket:
        case Tok::LBrace:
          import_stack_.push_back(false);
          at_pending_ = false;
          break;
        case Tok::RParen:
        case Tok::RBracket:
        case Tok::RBrace:
          if (!import_stack_.empty()) {
            import_stack_.pop_back();
          }
          at_pending_ = false;
          break;
        default:
          at_pending_ = false;
          break;
      }
    }
    return out;
  }

private:
  std::vector<bool> import_stack_;
  bool at_pending_ = false;
};

// ===== 逻辑行切分 =====

// 物理行分组：同一物理行上的 token。
// 括号内的换行不产生 Nl，故一条语句可能跨多个分组；保留分组即保留原有的
// 多行字面量结构（与 gofmt 一致），只规范化每组的缩进与空格。
struct Group {
  std::vector<const Token*> toks;
  int first_line = 0;
  int last_line = 0;
};

// 一条语句：由若干物理行分组构成。
struct Stmt {
  std::vector<Group> groups;
  int level = 0; // 缩进级别（由 Indent / Dedent 推得）
};

// 逻辑行是否以块构造关键字开头（if / func / class ...）。
// 此类行的 ';' 分隔语句属于块的同行体，不可拆行（否则会脱离所属块）。
bool starts_block_head(const std::vector<const Token*>& toks) {
  if (toks.empty() || toks[0]->kind != Tok::Kw) {
    return false;
  }
  const std::string_view w = toks[0]->text;
  return w == "if" || w == "elif" || w == "else" || w == "while" || w == "for" || w == "func" || w == "class" ||
         w == "struct";
}

// 按 token 的行号分组：同一物理行的 token 归为一组。
std::vector<Group> group_by_line(const std::vector<const Token*>& toks) {
  std::vector<Group> groups;
  Group cur;
  for (const Token* t : toks) {
    if (!cur.toks.empty() && cur.toks.back()->line != t->line) {
      groups.push_back(std::move(cur));
      cur = Group{};
    }
    if (cur.toks.empty()) {
      cur.first_line = t->line;
    }
    cur.last_line = t->line;
    cur.toks.push_back(t);
  }
  if (!cur.toks.empty()) {
    groups.push_back(std::move(cur));
  }
  return groups;
}

// 按 Nl 切分原始逻辑行，再按 ';' 细分（同块内的多语句），最后按物理行分组。
std::vector<Stmt> split_stmts(std::span<const Token> toks) {
  std::vector<Stmt> stmts;
  std::vector<const Token*> raw;
  int level = 0;

  auto emit = [&](std::vector<const Token*> toks_in) {
    if (toks_in.empty()) {
      return;
    }
    Stmt s;
    s.groups = group_by_line(toks_in);
    s.level = level;
    stmts.push_back(std::move(s));
  };

  auto flush_raw = [&] {
    if (raw.empty()) {
      return;
    }
    if (starts_block_head(raw)) {
      emit(raw);
    } else {
      // 按 ';' 切分为多条语句；各语句同属一块，级别一致。
      std::vector<const Token*> cur;
      for (const Token* t : raw) {
        if (t->kind == Tok::Semi) {
          emit(cur);
          cur.clear();
          continue;
        }
        cur.push_back(t);
      }
      emit(cur);
    }
    raw.clear();
  };

  for (const Token& t : toks) {
    switch (t.kind) {
      case Tok::Indent:
        ++level;
        continue;
      case Tok::Dedent:
        --level;
        continue;
      case Tok::Nl:
        flush_raw();
        continue;
      case Tok::Eof:
        flush_raw();
        return stmts;
      default:
        raw.push_back(&t);
        continue;
    }
  }
  flush_raw();
  return stmts;
}

// ===== 输出组装 =====

std::string indent_str(int level, int width) {
  if (level <= 0 || width <= 0) {
    return {};
  }
  return std::string(static_cast<std::size_t>(level) * static_cast<std::size_t>(width), ' ');
}

// 输出待定空行：块内最多 1 个，顶层最多 2 个。
void flush_blank(std::string& out, int count, int level) {
  const int max_blank = (level > 0) ? 1 : 2;
  const int n = std::min(count, max_blank);
  for (int i = 0; i < n; ++i) {
    out += '\n';
  }
}

// 查找物理行区间内的行尾注释（独占一行的注释不在此列）。
const std::string* find_trailing_comment(const std::vector<SrcLine>& src, const Group& g) {
  for (int ln = g.first_line; ln <= g.last_line; ++ln) {
    if (ln < 1 || std::cmp_greater(ln, src.size())) {
      continue;
    }
    const SrcLine& sl = src[static_cast<std::size_t>(ln - 1)];
    if (sl.has_comment && !sl.comment_own_line) {
      return &sl.comment;
    }
  }
  return nullptr;
}

// 分组的缩进级别：语句首组取语句级别；括号内续行缩进一级，
// 右闭合符所在行与前缀对齐（与 gofmt 的复合字面量排版一致）。
int group_level(const Group& g, bool is_first, int stmt_level) {
  if (is_first) {
    return stmt_level;
  }
  const Token* t = g.toks.front();
  if (t->kind == Tok::RParen || t->kind == Tok::RBracket || t->kind == Tok::RBrace) {
    return stmt_level;
  }
  return stmt_level + 1;
}

std::string assemble(const std::vector<Stmt>& stmts, const std::vector<SrcLine>& src, int indent_width) {
  std::string out;
  int prev_end = 0;      // 已处理到的源码行号
  int pending_blank = 0; // 待输出的空行数
  bool first = true;

  for (const Stmt& s : stmts) {
    Renderer renderer; // 括号栈状态跨分组延续
    for (std::size_t gi = 0; gi < s.groups.size(); ++gi) {
      const Group& g = s.groups[gi];
      const int lvl = group_level(g, gi == 0, s.level);

      // 本组之前：空行与独立注释行
      for (int ln = prev_end + 1; ln < g.first_line && std::cmp_less_equal(ln, src.size()); ++ln) {
        const SrcLine& sl = src[static_cast<std::size_t>(ln - 1)];
        if (sl.has_comment && sl.comment_own_line) {
          if (!first) {
            flush_blank(out, pending_blank, s.level);
          }
          pending_blank = 0;
          out += indent_str(lvl, indent_width);
          out += sl.comment;
          out += '\n';
          first = false;
        } else if (sl.blank) {
          ++pending_blank;
        }
      }
      if (!first) {
        flush_blank(out, pending_blank, s.level);
      }
      pending_blank = 0;
      first = false;

      out += indent_str(lvl, indent_width);
      out += renderer.render(g.toks);
      if (const std::string* tc = find_trailing_comment(src, g)) {
        out += "  ";
        out += *tc;
      }
      out += '\n';
      prev_end = g.last_line;
    }
  }

  // 文件末尾：剩余的独立注释行
  const int tail_level = stmts.empty() ? 0 : stmts.back().level;
  for (int ln = prev_end + 1; std::cmp_less_equal(ln, src.size()); ++ln) {
    const SrcLine& sl = src[static_cast<std::size_t>(ln - 1)];
    if (sl.has_comment && sl.comment_own_line) {
      out += indent_str(tail_level, indent_width);
      out += sl.comment;
      out += '\n';
    }
  }
  return out;
}

} // namespace

std::expected<FormatResult, std::string> format(std::string_view src, const Options& opts) {
  if (opts.indent_width <= 0) {
    return std::unexpected("缩进宽度必须为正整数");
  }

  vx::util::Arena arena;
  DiagBag bag;
  const std::span<const Token> toks = vx::compiler::tokenize(src, "<vfmt>", arena, bag);
  if (bag.has_error()) {
    return std::unexpected(vx::compiler::render_all(bag, src, false));
  }
  const std::optional<Program> prog = vx::compiler::parse(toks, "<vfmt>", arena, bag);
  if (!prog || bag.has_error()) {
    return std::unexpected(vx::compiler::render_all(bag, src, false));
  }
  const std::string before = ast_fingerprint(prog->stmts);

  const std::vector<SrcLine> src_lines = scan_lines(src);
  const std::vector<Stmt> stmts = split_stmts(toks);
  std::string out = assemble(stmts, src_lines, opts.indent_width);

  // 自检：格式化结果须能重新解析，且 AST 结构与原源码一致。
  if (opts.verify) {
    vx::util::Arena arena2;
    DiagBag bag2;
    const std::span<const Token> toks2 = vx::compiler::tokenize(out, "<vfmt>", arena2, bag2);
    if (bag2.has_error()) {
      return std::unexpected("格式化结果无法词法分析（vfmt 内部错误）");
    }
    const std::optional<Program> prog2 = vx::compiler::parse(toks2, "<vfmt>", arena2, bag2);
    if (!prog2 || bag2.has_error()) {
      return std::unexpected("格式化结果无法解析（vfmt 内部错误）");
    }
    if (ast_fingerprint(prog2->stmts) != before) {
      return std::unexpected("格式化结果与原源码结构不一致（vfmt 内部错误）");
    }
  }

  FormatResult res;
  res.text = std::move(out);
  // 行宽检查
  int line_no = 0;
  std::size_t pos = 0;
  while (pos <= res.text.size()) {
    const std::size_t nl = res.text.find('\n', pos);
    const std::size_t end = (nl == std::string::npos) ? res.text.size() : nl;
    ++line_no;
    if (std::cmp_greater(end - pos, opts.line_length)) {
      res.long_lines.push_back(line_no);
    }
    if (nl == std::string::npos) {
      break;
    }
    pos = nl + 1;
  }
  return res;
}

} // namespace vfmt
