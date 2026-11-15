// parser.cpp — VX 语法分析器实现（主入口；实现拆分于 parser_*.hpp）
#include "vx/compiler/parser.hpp"

#include "parser_decl.hpp"
#include "parser_expr.hpp"
#include "parser_stmt.hpp"

namespace vx::compiler {

std::string_view node_kind_name(NodeKind kind) {
  switch (kind) {
    case NodeKind::IntLit:
      return "IntLit";
    case NodeKind::FloatLit:
      return "FloatLit";
    case NodeKind::StrLit:
      return "StrLit";
    case NodeKind::BoolLit:
      return "BoolLit";
    case NodeKind::NilLit:
      return "NilLit";
    case NodeKind::Ident:
      return "Ident";
    case NodeKind::ListLit:
      return "ListLit";
    case NodeKind::DictLit:
      return "DictLit";
    case NodeKind::Index:
      return "Index";
    case NodeKind::Property:
      return "Property";
    case NodeKind::Call:
      return "Call";
    case NodeKind::New:
      return "New";
    case NodeKind::Unary:
      return "Unary";
    case NodeKind::Binary:
      return "Binary";
    case NodeKind::Deref:
      return "Deref";
    case NodeKind::FuncRef:
      return "FuncRef";
    case NodeKind::ExprStmt:
      return "ExprStmt";
    case NodeKind::VarDecl:
      return "VarDecl";
    case NodeKind::Assign:
      return "Assign";
    case NodeKind::FuncDecl:
      return "FuncDecl";
    case NodeKind::ExternDecl:
      return "ExternDecl";
    case NodeKind::Import:
      return "Import";
    case NodeKind::Export:
      return "Export";
    case NodeKind::Return:
      return "Return";
    case NodeKind::Break:
      return "Break";
    case NodeKind::Continue:
      return "Continue";
    case NodeKind::If:
      return "If";
    case NodeKind::While:
      return "While";
    case NodeKind::For:
      return "For";
    case NodeKind::ClassDecl:
      return "ClassDecl";
    case NodeKind::StructDecl:
      return "StructDecl";
    case NodeKind::EnumDecl:
      return "EnumDecl";
    case NodeKind::ImpDecl:
      return "ImpDecl";
    case NodeKind::CppImpDecl:
      return "CppImpDecl";
  }
  return "?";
}

std::optional<Program> parse(std::span<const Token> tokens, std::string_view filename, vx::util::Arena& arena,
                             DiagBag& bag) {
  if (tokens.empty() || tokens.back().kind != Tok::Eof) {
    bag.add(make_diag(diag_code::parse_invalid_token_stream, Severity::Error, "token 流必须以 Eof 结尾",
                      std::string(filename), 0, 0));
    return std::nullopt;
  }
  Parser p(tokens, filename, arena, bag);
  return p.run();
}

} // namespace vx::compiler
