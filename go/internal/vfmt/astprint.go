package vfmt

import (
	"fmt"
	"strings"

	"github.com/vx-lang/vxc/internal/ast"
)

// Printer converts an AST back into VX source code.
type Printer struct {
	indent    int
	indentStr string
	sb        strings.Builder
}

// NewPrinter creates a new Printer.
func NewPrinter() *Printer {
	return &Printer{
		indent:    0,
		indentStr: "    ", // 4 spaces
	}
}

// Print generates VX source code from the AST.
func (p *Printer) Print(program []ast.Expr) string {
	p.sb.Reset()
	for i, stmt := range program {
		if i > 0 {
			p.sb.WriteString("\n")
		}
		p.printStmt(stmt, true)
	}
	return p.sb.String()
}

// ==================== Statements ====================

func (p *Printer) printStmt(s ast.Expr, standalone bool) {
	// Only add indentation for standalone statements (not inside blocks that are already indented)
	if standalone {
		p.sb.WriteString(strings.Repeat(p.indentStr, p.indent))
	}

	switch x := s.(type) {
	case *ast.ExprStmt:
		if _, ok := x.Expr.(*ast.NilLiteral); ok {
			// Empty/blank line
			if standalone {
				p.sb.WriteString("\n")
			}
			return
		}
		p.printExpr(x.Expr)
		if standalone {
			p.sb.WriteString("\n")
		}

	case *ast.VarDecl:
		if x.IsMut {
			p.sb.WriteString("var ")
		} else {
			p.sb.WriteString("var ")
		}
		p.sb.WriteString(x.Name)
		if x.Type != nil {
			p.sb.WriteString(": ")
			p.printTypeExpr(*x.Type)
		}
		p.sb.WriteString(" = ")
		p.printExpr(x.Init)
		if standalone {
			p.sb.WriteString("\n")
		}

	case *ast.Assign:
		p.printExpr(x.Target)
		p.sb.WriteString(" ")
		p.sb.WriteString(x.Op)
		p.sb.WriteString(" ")
		p.printExpr(x.Value)
		if standalone {
			p.sb.WriteString("\n")
		}

	case *ast.ReturnStmt:
		if x.Value != nil {
			p.sb.WriteString("return ")
			p.printExpr(x.Value)
		} else {
			p.sb.WriteString("return")
		}
		if standalone {
			p.sb.WriteString("\n")
		}

	case *ast.BreakStmt:
		p.sb.WriteString("break")
		if x.Label != "" {
			p.sb.WriteString(" ")
			p.sb.WriteString(x.Label)
		}
		if standalone {
			p.sb.WriteString("\n")
		}

	case *ast.ContinueStmt:
		p.sb.WriteString("continue")
		if x.Label != "" {
			p.sb.WriteString(" ")
			p.sb.WriteString(x.Label)
		}
		if standalone {
			p.sb.WriteString("\n")
		}

	case *ast.ImportStmt:
		p.sb.WriteString("import ")
		p.sb.WriteString(x.Path)
		if x.Alias != "" {
			p.sb.WriteString(" as ")
			p.sb.WriteString(x.Alias)
		}
		if standalone {
			p.sb.WriteString("\n")
		}

	case *ast.IfStmt:
		p.printIfStmt(x)

	case *ast.MatchStmt:
		p.sb.WriteString("match ")
		p.printExpr(x.Target)
		p.sb.WriteString(":\n")
		p.indent++
		for _, arm := range x.Arms {
			p.printMatchArm(arm)
		}
		p.indent--

	case *ast.WhileStmt:
		p.sb.WriteString("while ")
		p.printExpr(x.Condition)
		p.sb.WriteString(":\n")
		p.indent++
		for _, stmt := range x.Body {
			p.printStmt(stmt, true)
		}
		p.indent--

	case *ast.ForStmt:
		p.sb.WriteString("for ")
		p.sb.WriteString(x.VarName)
		p.sb.WriteString(" in ")
		p.printExpr(x.Iterable)
		p.sb.WriteString(":\n")
		p.indent++
		for _, stmt := range x.Body {
			p.printStmt(stmt, true)
		}
		p.indent--

	case *ast.LoopStmt:
		p.sb.WriteString("loop")
		if x.Label != "" {
			p.sb.WriteString(" ")
			p.sb.WriteString(x.Label)
		}
		p.sb.WriteString(":\n")
		p.indent++
		for _, stmt := range x.Body {
			p.printStmt(stmt, true)
		}
		p.indent--

	case *ast.FuncDecl:
		p.printFuncDecl(x)

	case *ast.ExternDecl:
		p.sb.WriteString("extern func ")
		p.sb.WriteString(x.Name)
		p.sb.WriteString("(")
		p.printParams(x.Params)
		p.sb.WriteString(")")
		if x.ReturnType != nil {
			p.sb.WriteString(" -> ")
			p.printTypeExpr(*x.ReturnType)
		}
		if standalone {
			p.sb.WriteString("\n")
		}

	case *ast.StructDecl:
		p.sb.WriteString("struct ")
		p.sb.WriteString(x.Name)
		if len(x.GenericParams) > 0 {
			p.sb.WriteString("[")
			for i, g := range x.GenericParams {
				if i > 0 {
					p.sb.WriteString(", ")
				}
				p.sb.WriteString(g)
			}
			p.sb.WriteString("]")
		}
		if len(x.Fields) > 0 || len(x.Methods) > 0 {
			p.sb.WriteString(":\n")
			p.indent++
			for _, f := range x.Fields {
				p.sb.WriteString(strings.Repeat(p.indentStr, p.indent))
				p.sb.WriteString(f.Name)
				p.sb.WriteString(": ")
				p.printTypeExpr(f.FieldType)
				p.sb.WriteString("\n")
			}
			if len(x.Fields) > 0 && len(x.Methods) > 0 {
				p.sb.WriteString("\n")
			}
			for _, m := range x.Methods {
				p.printStmt(m, true)
			}
			p.indent--
		}

	case *ast.ClassDecl:
		p.sb.WriteString("class ")
		p.sb.WriteString(x.Name)
		if len(x.GenericParams) > 0 {
			p.sb.WriteString("[")
			for i, g := range x.GenericParams {
				if i > 0 {
					p.sb.WriteString(", ")
				}
				p.sb.WriteString(g)
			}
			p.sb.WriteString("]")
		}
		p.sb.WriteString(":\n")
		p.indent++
		for _, f := range x.Fields {
			p.sb.WriteString(strings.Repeat(p.indentStr, p.indent))
			p.sb.WriteString(f.Name)
			p.sb.WriteString(": ")
			p.printTypeExpr(f.FieldType)
			p.sb.WriteString("\n")
		}
		if len(x.Fields) > 0 && len(x.Methods) > 0 {
			p.sb.WriteString("\n")
		}
		for _, m := range x.Methods {
			p.printStmt(m, true)
		}
		p.indent--

	case *ast.EnumDecl:
		p.sb.WriteString("enum ")
		p.sb.WriteString(x.Name)
		p.sb.WriteString(":\n")
		p.indent++
		for _, v := range x.Variants {
			p.sb.WriteString(strings.Repeat(p.indentStr, p.indent))
			p.sb.WriteString(v.Name)
			if v.Value != nil {
				p.sb.WriteString(" = ")
				p.sb.WriteString(fmt.Sprintf("%d", *v.Value))
			}
			p.sb.WriteString("\n")
		}
		p.indent--

	case *ast.UnionDecl:
		p.sb.WriteString("union ")
		p.sb.WriteString(x.Name)
		if len(x.Fields) > 0 {
			p.sb.WriteString(":\n")
			p.indent++
			for _, f := range x.Fields {
				p.sb.WriteString(strings.Repeat(p.indentStr, p.indent))
				p.sb.WriteString(f.Name)
				p.sb.WriteString(": ")
				p.printTypeExpr(f.FieldType)
				p.sb.WriteString("\n")
			}
			p.indent--
		}

	case *ast.MacroDef:
		p.sb.WriteString("macro ")
		p.sb.WriteString(x.Name)
		p.sb.WriteString("(")
		for i, param := range x.Params {
			if i > 0 {
				p.sb.WriteString(", ")
			}
			p.sb.WriteString(param)
		}
		p.sb.WriteString("):\n")
		p.indent++
		for _, stmt := range x.Body {
			p.printStmt(stmt, true)
		}
		p.indent--

	case *ast.MacroCall:
		p.sb.WriteString("#")
		p.sb.WriteString(x.Name)
		p.sb.WriteString("(")
		for i, arg := range x.Args {
			if i > 0 {
				p.sb.WriteString(", ")
			}
			p.printExpr(arg)
		}
		p.sb.WriteString(")")
		if standalone {
			p.sb.WriteString("\n")
		}

	case *ast.NewExpr:
		p.sb.WriteString("new ")
		p.sb.WriteString(x.TypeName)
		p.sb.WriteString("(")
		for i, arg := range x.Positional {
			if i > 0 {
				p.sb.WriteString(", ")
			}
			p.printExpr(arg)
		}
		p.sb.WriteString(")")
		if standalone {
			p.sb.WriteString("\n")
		}

	default:
		p.printExpr(s)
		if standalone {
			p.sb.WriteString("\n")
		}
	}
}

func (p *Printer) printIfStmt(x *ast.IfStmt) {
	p.sb.WriteString("if ")
	p.printExpr(x.Condition)
	p.sb.WriteString(":\n")
	p.indent++
	for _, stmt := range x.ThenBranch {
		p.printStmt(stmt, true)
	}
	p.indent--

	for _, elif := range x.ElifBranches {
		p.sb.WriteString(strings.Repeat(p.indentStr, p.indent))
		p.sb.WriteString("elif ")
		p.printExpr(elif.Condition)
		p.sb.WriteString(":\n")
		p.indent++
		for _, stmt := range elif.Body {
			p.printStmt(stmt, true)
		}
		p.indent--
	}

	if len(x.ElseBranch) > 0 {
		p.sb.WriteString(strings.Repeat(p.indentStr, p.indent))
		p.sb.WriteString("else:\n")
		p.indent++
		for _, stmt := range x.ElseBranch {
			p.printStmt(stmt, true)
		}
		p.indent--
	}
}

func (p *Printer) printMatchArm(arm ast.MatchArm) {
	p.sb.WriteString(strings.Repeat(p.indentStr, p.indent))
	p.printExpr(arm.Pattern)
	p.sb.WriteString(":\n")
	p.indent++
	for _, stmt := range arm.Body {
		p.printStmt(stmt, true)
	}
	p.indent--
}

func (p *Printer) printFuncDecl(x *ast.FuncDecl) {
	p.sb.WriteString("func ")
	p.sb.WriteString(x.Name)
	if len(x.GenericParams) > 0 {
		p.sb.WriteString("[")
		for i, g := range x.GenericParams {
			if i > 0 {
				p.sb.WriteString(", ")
			}
			p.sb.WriteString(g)
		}
		p.sb.WriteString("]")
	}
	p.sb.WriteString("(")
	p.printParams(x.Params)
	p.sb.WriteString(")")
	if x.ReturnType != nil {
		p.sb.WriteString(" -> ")
		p.printTypeExpr(*x.ReturnType)
	}
	p.sb.WriteString(":\n")
	p.indent++
	for _, stmt := range x.Body {
		p.printStmt(stmt, true)
	}
	p.indent--
}

// ==================== Expressions ====================

func (p *Printer) printExpr(e ast.Expr) {
	switch x := e.(type) {
	case *ast.IntLiteral:
		p.sb.WriteString(fmt.Sprintf("%d", x.Value))

	case *ast.FloatLiteral:
		p.sb.WriteString(fmt.Sprintf("%g", x.Value))

	case *ast.StringLiteral:
		p.sb.WriteString("\"")
		p.sb.WriteString(escapeString(x.Value))
		p.sb.WriteString("\"")

	case *ast.BoolLiteral:
		if x.Value {
			p.sb.WriteString("true")
		} else {
			p.sb.WriteString("false")
		}

	case *ast.NilLiteral:
		p.sb.WriteString("nil")

	case *ast.Identifier:
		p.sb.WriteString(x.Name)

	case *ast.ArrayLiteral:
		p.sb.WriteString("[")
		for i, elem := range x.Elements {
			if i > 0 {
				p.sb.WriteString(", ")
			}
			p.printExpr(elem)
		}
		p.sb.WriteString("]")

	case *ast.MapLiteral:
		p.sb.WriteString("{")
		for i, pair := range x.Pairs {
			if i > 0 {
				p.sb.WriteString(", ")
			}
			p.printExpr(pair.Key)
			p.sb.WriteString(": ")
			p.printExpr(pair.Value)
		}
		p.sb.WriteString("}")

	case *ast.BinaryOp:
		p.sb.WriteString("(")
		p.printExpr(x.Left)
		p.sb.WriteString(" ")
		p.sb.WriteString(x.Op)
		p.sb.WriteString(" ")
		p.printExpr(x.Right)
		p.sb.WriteString(")")

	case *ast.UnaryOp:
		p.sb.WriteString(x.Op)
		p.printExpr(x.Expr)

	case *ast.AddressOf:
		p.sb.WriteString("&")
		if x.IsMut {
			p.sb.WriteString("mut ")
		}
		p.printExpr(x.Expr)

	case *ast.Deref:
		p.sb.WriteString("*")
		p.printExpr(x.Expr)

	case *ast.PointerMember:
		p.printExpr(x.Expr)
		p.sb.WriteString("->")
		p.sb.WriteString(x.Member)

	case *ast.IndexAccess:
		p.printExpr(x.Target)
		p.sb.WriteString("[")
		p.printExpr(x.Index)
		p.sb.WriteString("]")

	case *ast.PropertyAccess:
		p.printExpr(x.Target)
		p.sb.WriteString(".")
		p.sb.WriteString(x.Prop)

	case *ast.CallExpr:
		p.printExpr(x.Callee)
		p.sb.WriteString("(")
		for i, arg := range x.Args {
			if i > 0 {
				p.sb.WriteString(", ")
			}
			p.printExpr(arg)
		}
		p.sb.WriteString(")")

	case *ast.MoveExpr:
		p.sb.WriteString("move ")
		p.printExpr(x.Expr)

	case *ast.NewExpr:
		p.sb.WriteString("new ")
		p.sb.WriteString(x.TypeName)
		p.sb.WriteString("(")
		for i, arg := range x.Positional {
			if i > 0 {
				p.sb.WriteString(", ")
			}
			p.printExpr(arg)
		}
		p.sb.WriteString(")")

	case *ast.ExprStmt:
		p.printExpr(x.Expr)

	case *ast.MacroCall:
		p.sb.WriteString("#")
		p.sb.WriteString(x.Name)
		p.sb.WriteString("(")
		for i, arg := range x.Args {
			if i > 0 {
				p.sb.WriteString(", ")
			}
			p.printExpr(arg)
		}
		p.sb.WriteString(")")

	default:
		p.sb.WriteString(fmt.Sprintf("/* unknown AST node: %T */", e))
	}
}

// ==================== Type Expressions ====================

func (p *Printer) printTypeExpr(t ast.TypeExpr) {
	p.sb.WriteString(t.Name)
	if len(t.GenericArgs) > 0 {
		p.sb.WriteString("<")
		for i, a := range t.GenericArgs {
			if i > 0 {
				p.sb.WriteString(", ")
			}
			p.printTypeExpr(a)
		}
		p.sb.WriteString(">")
	}
	for i := 0; i < t.Pointer; i++ {
		p.sb.WriteString("*")
	}
}

// ==================== Parameters ====================

func (p *Printer) printParams(params []ast.ParamDef) {
	for i, param := range params {
		if i > 0 {
			p.sb.WriteString(", ")
		}
		p.sb.WriteString(param.Name)
		p.sb.WriteString(": ")
		p.printTypeExpr(param.ParamType)
	}
}

// ==================== Helpers ====================

func escapeString(s string) string {
	s = strings.ReplaceAll(s, "\\", "\\\\")
	s = strings.ReplaceAll(s, "\"", "\\\"")
	s = strings.ReplaceAll(s, "\n", "\\n")
	s = strings.ReplaceAll(s, "\t", "\\t")
	s = strings.ReplaceAll(s, "\r", "\\r")
	return s
}
