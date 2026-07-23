package definition

import (
	"strings"

	"github.com/vx-lang/vxc/internal/ast"
	"github.com/vx-lang/vxc/internal/lsp/types"
)

func GotoDefinition(astNodes []ast.Expr, source string, position types.Position, uri string) (*types.GotoDefinitionResponse, error) {
	line := int(position.Line) + 1
	col := int(position.Character) + 1

	name := identifierNameAt(source, line, col)
	if name == "" {
		return nil, nil
	}

	if enclosing := findEnclosingFunction(astNodes, line); enclosing != nil {
		if loc := findLocalDefinition(enclosing, name, line); loc != nil {
			return locationToResponse(uri, *loc), nil
		}
	}

	var defs [][2]int
	collectDefinitions(astNodes, name, &defs)

	if len(defs) > 0 {
		return locationToResponse(uri, defs[0]), nil
	}

	return nil, nil
}

func identifierNameAt(source string, line, col int) string {
	lines := strings.Split(source, "\n")
	currentLine := 0

	for _, l := range lines {
		currentLine++
		if currentLine == line {
			if col <= len(l) {
				start := col
				for start > 0 && isIdentChar(l[start-1]) {
					start--
				}
				end := col
				for end < len(l) && isIdentChar(l[end]) {
					end++
				}

				if start < end {
					return l[start:end]
				}
			}
			break
		}
	}
	return ""
}

func isIdentChar(c byte) bool {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		(c >= '0' && c <= '9') || c == '_'
}

func findEnclosingFunction(astNodes []ast.Expr, targetLine int) ast.Expr {
	for _, node := range astNodes {
		if found := findEnclosingInNode(node, targetLine); found != nil {
			return found
		}
	}
	return nil
}

func findEnclosingInNode(node ast.Expr, targetLine int) ast.Expr {
	switch n := node.(type) {
	case *ast.FuncDecl:
		p := n.Pos()
		if p.Line <= targetLine {
			for _, bodyNode := range n.Body {
				if found := findEnclosingInNode(bodyNode, targetLine); found != nil {
					return found
				}
			}
			return n
		}

	case *ast.StructDecl:
		for _, method := range n.Methods {
			if found := findEnclosingInNode(method, targetLine); found != nil {
				return found
			}
		}

	case *ast.ClassDecl:
		for _, method := range n.Methods {
			if found := findEnclosingInNode(method, targetLine); found != nil {
				return found
			}
		}

	case *ast.IfStmt:
		for _, bodyNode := range n.ThenBranch {
			if found := findEnclosingInNode(bodyNode, targetLine); found != nil {
				return found
			}
		}
		for _, elif := range n.ElifBranches {
			for _, bodyNode := range elif.Body {
				if found := findEnclosingInNode(bodyNode, targetLine); found != nil {
					return found
				}
			}
		}
		for _, bodyNode := range n.ElseBranch {
			if found := findEnclosingInNode(bodyNode, targetLine); found != nil {
				return found
			}
		}

	case *ast.WhileStmt:
		for _, bodyNode := range n.Body {
			if found := findEnclosingInNode(bodyNode, targetLine); found != nil {
				return found
			}
		}

	case *ast.ForStmt:
		for _, bodyNode := range n.Body {
			if found := findEnclosingInNode(bodyNode, targetLine); found != nil {
				return found
			}
		}

	case *ast.LoopStmt:
		for _, bodyNode := range n.Body {
			if found := findEnclosingInNode(bodyNode, targetLine); found != nil {
				return found
			}
		}

	case *ast.MatchStmt:
		for _, arm := range n.Arms {
			for _, bodyNode := range arm.Body {
				if found := findEnclosingInNode(bodyNode, targetLine); found != nil {
					return found
				}
			}
		}
	}
	return nil
}

func findLocalDefinition(funcNode ast.Expr, name string, targetLine int) *[2]int {
	switch n := funcNode.(type) {
	case *ast.FuncDecl:
		for _, p := range n.Params {
			if p.Name == name {
				pp := n.Pos()
				return &[2]int{pp.Line, pp.Col}
			}
		}
		for _, bodyNode := range n.Body {
			if loc := searchVarDecl(bodyNode, name, targetLine); loc != nil {
				return loc
			}
		}
	}
	return nil
}

func searchVarDecl(node ast.Expr, name string, targetLine int) *[2]int {
	switch n := node.(type) {
	case *ast.VarDecl:
		if n.Name == name {
			p := n.Pos()
			if p.Line <= targetLine {
				return &[2]int{p.Line, p.Col}
			}
		}

	case *ast.ForStmt:
		if n.VarName == name {
			p := n.Pos()
			if p.Line <= targetLine {
				return &[2]int{p.Line, p.Col}
			}
		}
		for _, bodyNode := range n.Body {
			if loc := searchVarDecl(bodyNode, name, targetLine); loc != nil {
				return loc
			}
		}

	case *ast.IfStmt:
		for _, bodyNode := range n.ThenBranch {
			if loc := searchVarDecl(bodyNode, name, targetLine); loc != nil {
				return loc
			}
		}
		for _, elif := range n.ElifBranches {
			for _, bodyNode := range elif.Body {
				if loc := searchVarDecl(bodyNode, name, targetLine); loc != nil {
					return loc
				}
			}
		}
		for _, bodyNode := range n.ElseBranch {
			if loc := searchVarDecl(bodyNode, name, targetLine); loc != nil {
				return loc
			}
		}

	case *ast.WhileStmt:
		for _, bodyNode := range n.Body {
			if loc := searchVarDecl(bodyNode, name, targetLine); loc != nil {
				return loc
			}
		}

	case *ast.MatchStmt:
		for _, arm := range n.Arms {
			for _, bodyNode := range arm.Body {
				if loc := searchVarDecl(bodyNode, name, targetLine); loc != nil {
					return loc
				}
			}
		}

	case *ast.LoopStmt:
		for _, bodyNode := range n.Body {
			if loc := searchVarDecl(bodyNode, name, targetLine); loc != nil {
				return loc
			}
		}
	}
	return nil
}

func collectDefinitions(astNodes []ast.Expr, name string, defs *[][2]int) {
	for _, node := range astNodes {
		switch n := node.(type) {
		case *ast.FuncDecl:
			if n.Name == name {
				p := n.Pos()
				*defs = append(*defs, [2]int{p.Line, p.Col})
			}

		case *ast.StructDecl:
			if n.Name == name {
				p := n.Pos()
				*defs = append(*defs, [2]int{p.Line, p.Col})
			}

		case *ast.ClassDecl:
			if n.Name == name {
				p := n.Pos()
				*defs = append(*defs, [2]int{p.Line, p.Col})
			}

		case *ast.EnumDecl:
			if n.Name == name {
				p := n.Pos()
				*defs = append(*defs, [2]int{p.Line, p.Col})
			}

		case *ast.UnionDecl:
			if n.Name == name {
				p := n.Pos()
				*defs = append(*defs, [2]int{p.Line, p.Col})
			}

		case *ast.VarDecl:
			if n.Name == name {
				p := n.Pos()
				*defs = append(*defs, [2]int{p.Line, p.Col})
			}
		}
	}
}

func locationToResponse(uri string, loc [2]int) *types.GotoDefinitionResponse {
	line := uint32(loc[0] - 1)
	if loc[0] == 0 {
		line = 0
	}
	col := uint32(loc[1] - 1)
	if loc[1] == 0 {
		col = 0
	}
	return &types.GotoDefinitionResponse{
		URI: uri,
		Range: types.Range{
			Start: types.Position{Line: line, Character: col},
			End:   types.Position{Line: line, Character: col + uint32(1)},
		},
	}
}
