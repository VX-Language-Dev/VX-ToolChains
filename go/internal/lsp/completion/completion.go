package completion

import (
	"fmt"
	"strings"

	"github.com/vx-lang/vxc/internal/ast"
)

type SymbolInfoKind int

const (
	SymbolFunction SymbolInfoKind = iota
	SymbolVariable
	SymbolStruct
	SymbolClass
	SymbolEnum
	SymbolUnion
	SymbolTypeAlias
	SymbolLoopVar
	SymbolParam
)

type SymbolInfo struct {
	Name   string
	Kind   SymbolInfoKind
	Detail *string
	Doc    *string
}

func CollectSymbols(astNodes []ast.Expr) ([]SymbolInfo, error) {
	var symbolsList []SymbolInfo

	for _, node := range astNodes {
		collectFromStmt(node, &symbolsList)
	}

	return symbolsList, nil
}

func collectFromStmt(node ast.Expr, symbols *[]SymbolInfo) {
	switch n := node.(type) {
	case *ast.FuncDecl:
		var detailBuf strings.Builder
		detailBuf.WriteString("func ")
		detailBuf.WriteString(n.Name)
		detailBuf.WriteString("(")
		for i, p := range n.Params {
			if i > 0 {
				detailBuf.WriteString(", ")
			}
			detailBuf.WriteString(p.Name)
			detailBuf.WriteString(": ")
			detailBuf.WriteString(p.ParamType.Name)
		}
		detailBuf.WriteString(")")
		if n.ReturnType != nil {
			detailBuf.WriteString(" -> ")
			detailBuf.WriteString(n.ReturnType.Name)
		}
		detail := detailBuf.String()

		*symbols = append(*symbols, SymbolInfo{
			Name:   n.Name,
			Kind:   SymbolFunction,
			Detail: &detail,
			Doc:    nil,
		})
		for _, bodyNode := range n.Body {
			collectFromStmt(bodyNode, symbols)
		}

	case *ast.VarDecl:
		var detailBuf strings.Builder
		if n.IsMut {
			detailBuf.WriteString("mut ")
		} else {
			detailBuf.WriteString("var ")
		}
		detailBuf.WriteString(n.Name)
		if n.Type != nil {
			detailBuf.WriteString(": ")
			detailBuf.WriteString(n.Type.Name)
		}
		detail := detailBuf.String()

		*symbols = append(*symbols, SymbolInfo{
			Name:   n.Name,
			Kind:   SymbolVariable,
			Detail: &detail,
			Doc:    nil,
		})

	case *ast.StructDecl:
		var detailBuf strings.Builder
		detailBuf.WriteString("struct ")
		detailBuf.WriteString(n.Name)
		if len(n.GenericParams) > 0 {
			detailBuf.WriteString("<")
			for i, gp := range n.GenericParams {
				if i > 0 {
					detailBuf.WriteString(", ")
				}
				detailBuf.WriteString(gp)
			}
			detailBuf.WriteString(">")
		}
		detail := detailBuf.String()

		*symbols = append(*symbols, SymbolInfo{
			Name:   n.Name,
			Kind:   SymbolStruct,
			Detail: &detail,
			Doc:    nil,
		})

	case *ast.ClassDecl:
		detail := "class"
		*symbols = append(*symbols, SymbolInfo{
			Name:   n.Name,
			Kind:   SymbolClass,
			Detail: &detail,
			Doc:    nil,
		})

	case *ast.EnumDecl:
		detail := "enum"
		*symbols = append(*symbols, SymbolInfo{
			Name:   n.Name,
			Kind:   SymbolEnum,
			Detail: &detail,
			Doc:    nil,
		})

	case *ast.UnionDecl:
		detail := "union"
		*symbols = append(*symbols, SymbolInfo{
			Name:   n.Name,
			Kind:   SymbolUnion,
			Detail: &detail,
			Doc:    nil,
		})

	case *ast.ForStmt:
		detail := "loop variable"
		*symbols = append(*symbols, SymbolInfo{
			Name:   n.VarName,
			Kind:   SymbolLoopVar,
			Detail: &detail,
			Doc:    nil,
		})
		for _, bodyNode := range n.Body {
			collectFromStmt(bodyNode, symbols)
		}

	case *ast.IfStmt:
		for _, bodyNode := range n.ThenBranch {
			collectFromStmt(bodyNode, symbols)
		}
		for _, elif := range n.ElifBranches {
			for _, bodyNode := range elif.Body {
				collectFromStmt(bodyNode, symbols)
			}
		}
		for _, bodyNode := range n.ElseBranch {
			collectFromStmt(bodyNode, symbols)
		}

	case *ast.WhileStmt:
		for _, bodyNode := range n.Body {
			collectFromStmt(bodyNode, symbols)
		}

	case *ast.MatchStmt:
		for _, arm := range n.Arms {
			for _, bodyNode := range arm.Body {
				collectFromStmt(bodyNode, symbols)
			}
		}

	case *ast.LoopStmt:
		for _, bodyNode := range n.Body {
			collectFromStmt(bodyNode, symbols)
		}
	}
}

func FormatFuncSignature(name string, params []struct{ Name, Type string }, retType *string) string {
	var result strings.Builder
	result.WriteString("func ")
	result.WriteString(name)
	result.WriteString("(")

	for i, param := range params {
		if i > 0 {
			result.WriteString(", ")
		}
		result.WriteString(param.Name)
		result.WriteString(": ")
		result.WriteString(param.Type)
	}

	result.WriteString(")")

	if retType != nil {
		result.WriteString(" -> ")
		result.WriteString(*retType)
	}

	return result.String()
}

func FormatStructInfo(sd *ast.StructDecl) string {
	var buf strings.Builder
	buf.WriteString("struct ")
	buf.WriteString(sd.Name)
	if len(sd.GenericParams) > 0 {
		buf.WriteString("<")
		for i, gp := range sd.GenericParams {
			if i > 0 {
				buf.WriteString(", ")
			}
			buf.WriteString(gp)
		}
		buf.WriteString(">")
	}
	buf.WriteString(" {\n")
	for _, f := range sd.Fields {
		buf.WriteString("    ")
		buf.WriteString(f.Name)
		buf.WriteString(": ")
		buf.WriteString(f.FieldType.Name)
		buf.WriteString("\n")
	}
	buf.WriteString("}")
	return buf.String()
}

func FormatClassInfo(cd *ast.ClassDecl) string {
	var buf strings.Builder
	buf.WriteString("class ")
	buf.WriteString(cd.Name)
	buf.WriteString(" {\n")
	for _, f := range cd.Fields {
		buf.WriteString("    ")
		buf.WriteString(f.Visibility)
		buf.WriteString(" ")
		buf.WriteString(f.Name)
		buf.WriteString(": ")
		buf.WriteString(f.FieldType.Name)
		buf.WriteString("\n")
	}
	buf.WriteString("}")
	return buf.String()
}

func FormatEnumInfo(ed *ast.EnumDecl) string {
	var buf strings.Builder
	buf.WriteString("enum ")
	buf.WriteString(ed.Name)
	buf.WriteString(" {\n")
	for _, v := range ed.Variants {
		buf.WriteString("    ")
		buf.WriteString(v.Name)
		buf.WriteString(" = ")
		if v.Value != nil {
			buf.WriteString(fmt.Sprintf("%d", *v.Value))
		}
		buf.WriteString("\n")
	}
	buf.WriteString("}")
	return buf.String()
}