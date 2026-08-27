package inlay_hints

import (
	"github.com/vx-lang/vxc/internal/ast"
	"github.com/vx-lang/vxc/internal/lsp/types"
)

func CollectInlayHints(astNodes []ast.Expr) ([]types.InlayHint, error) {
	var hints []types.InlayHint

	for _, node := range astNodes {
		collectFromStmt(node, &hints)
	}

	return hints, nil
}

func collectFromStmt(node ast.Expr, hints *[]types.InlayHint) {
	switch n := node.(type) {
	case *ast.VarDecl:
		if n.Type == nil {
			if inferredType := tryInferTypeFromExpr(n.Init); inferredType != "" {
				p := n.Pos()
				nameLen := len(n.Name)
				*hints = append(*hints, types.InlayHint{
					Position: types.Position{
						Line:      uint32(p.Line - 1),
						Character: uint32(p.Col - 1 + nameLen + 2),
					},
					Label:        types.InlayHintLabel{String: &inferredType},
					Kind:         func() *types.InlayHintKind { k := types.InlayHintKindType; return &k }(),
					Tooltip:      nil,
					PaddingLeft:  true,
					PaddingRight: false,
				})
			}
		}

	case *ast.FuncDecl:
		if n.ReturnType == nil {
			if len(n.Body) > 0 {
				if inferredType := tryInferTypeFromExpr(n.Body[len(n.Body)-1]); inferredType != "" {
					p := n.Pos()
					nameLen := len(n.Name)
					*hints = append(*hints, types.InlayHint{
						Position: types.Position{
							Line:      uint32(p.Line - 1),
							Character: uint32(p.Col - 1 + nameLen),
						},
						Label:        types.InlayHintLabel{String: &inferredType},
						Kind:         func() *types.InlayHintKind { k := types.InlayHintKindType; return &k }(),
						Tooltip:      nil,
						PaddingLeft:  true,
						PaddingRight: false,
					})
				}
			}
		}
		for _, bodyNode := range n.Body {
			collectFromStmt(bodyNode, hints)
		}

	case *ast.ForStmt:
		for _, bodyNode := range n.Body {
			collectFromStmt(bodyNode, hints)
		}

	case *ast.WhileStmt:
		for _, bodyNode := range n.Body {
			collectFromStmt(bodyNode, hints)
		}

	case *ast.IfStmt:
		for _, bodyNode := range n.ThenBranch {
			collectFromStmt(bodyNode, hints)
		}
		for _, elif := range n.ElifBranches {
			for _, bodyNode := range elif.Body {
				collectFromStmt(bodyNode, hints)
			}
		}
		for _, bodyNode := range n.ElseBranch {
			collectFromStmt(bodyNode, hints)
		}

	case *ast.MatchStmt:
		for _, arm := range n.Arms {
			for _, bodyNode := range arm.Body {
				collectFromStmt(bodyNode, hints)
			}
		}

	case *ast.LoopStmt:
		for _, bodyNode := range n.Body {
			collectFromStmt(bodyNode, hints)
		}
	}
}

func tryInferTypeFromExpr(expr ast.Expr) string {
	switch expr.(type) {
	case *ast.IntLiteral:
		return "int"
	case *ast.FloatLiteral:
		return "float"
	case *ast.StringLiteral:
		return "string"
	case *ast.BoolLiteral:
		return "bool"
	case *ast.NilLiteral:
		return "nil"
	case *ast.ArrayLiteral:
		return "array"
	case *ast.MapLiteral:
		return "map"
	default:
		return ""
	}
}
