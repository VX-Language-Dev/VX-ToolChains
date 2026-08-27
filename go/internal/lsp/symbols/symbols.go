package symbols

import (
	"github.com/vx-lang/vxc/internal/ast"
	"github.com/vx-lang/vxc/internal/lsp/types"
)

func DocumentSymbols(astNodes []ast.Expr) ([]types.DocumentSymbol, error) {
	var symbols []types.DocumentSymbol

	for _, node := range astNodes {
		if sym := buildDocumentSymbol(node); sym != nil {
			symbols = append(symbols, *sym)
		}
	}

	return symbols, nil
}

func buildDocumentSymbol(node ast.Expr) *types.DocumentSymbol {
	switch n := node.(type) {
	case *ast.FuncDecl:
		var children []types.DocumentSymbol
		for _, bodyNode := range n.Body {
			if child := buildDocumentSymbol(bodyNode); child != nil {
				children = append(children, *child)
			}
		}

		p := n.Pos()
		nameLen := len(n.Name)
		rng := types.Range{
			Start: types.Position{Line: uint32(p.Line - 1), Character: uint32(p.Col - 1)},
			End:   types.Position{Line: uint32(p.Line - 1), Character: uint32(p.Col - 1 + nameLen)},
		}

		var detail *string
		if n.ReturnType != nil {
			dt := n.ReturnType.Name
			detail = &dt
		}

		return &types.DocumentSymbol{
			Name:           n.Name,
			Detail:         detail,
			Kind:           types.SymbolFunction,
			Range:          rng,
			SelectionRange: rng,
			Children:       children,
		}

	case *ast.StructDecl:
		var children []types.DocumentSymbol
		for _, f := range n.Fields {
			p := n.Pos()
			fieldRange := types.Range{
				Start: types.Position{Line: uint32(p.Line - 1), Character: uint32(p.Col - 1)},
				End:   types.Position{Line: uint32(p.Line - 1), Character: uint32(p.Col - 1 + len(f.Name))},
			}
			fieldType := f.FieldType.Name
			children = append(children, types.DocumentSymbol{
				Name:           f.Name,
				Detail:         &fieldType,
				Kind:           types.SymbolField,
				Range:          fieldRange,
				SelectionRange: fieldRange,
				Children:       nil,
			})
		}

		p := n.Pos()
		nameLen := len(n.Name)
		rng := types.Range{
			Start: types.Position{Line: uint32(p.Line - 1), Character: uint32(p.Col - 1)},
			End:   types.Position{Line: uint32(p.Line - 1), Character: uint32(p.Col - 1 + nameLen)},
		}

		return &types.DocumentSymbol{
			Name:           n.Name,
			Detail:         nil,
			Kind:           types.SymbolStruct,
			Range:          rng,
			SelectionRange: rng,
			Children:       children,
		}

	case *ast.ClassDecl:
		var children []types.DocumentSymbol
		for _, f := range n.Fields {
			p := n.Pos()
			fieldRange := types.Range{
				Start: types.Position{Line: uint32(p.Line - 1), Character: uint32(p.Col - 1)},
				End:   types.Position{Line: uint32(p.Line - 1), Character: uint32(p.Col - 1 + len(f.Name))},
			}
			fieldType := f.FieldType.Name
			children = append(children, types.DocumentSymbol{
				Name:           f.Name,
				Detail:         &fieldType,
				Kind:           types.SymbolField,
				Range:          fieldRange,
				SelectionRange: fieldRange,
				Children:       nil,
			})
		}

		p := n.Pos()
		nameLen := len(n.Name)
		rng := types.Range{
			Start: types.Position{Line: uint32(p.Line - 1), Character: uint32(p.Col - 1)},
			End:   types.Position{Line: uint32(p.Line - 1), Character: uint32(p.Col - 1 + nameLen)},
		}

		return &types.DocumentSymbol{
			Name:           n.Name,
			Detail:         nil,
			Kind:           types.SymbolClass,
			Range:          rng,
			SelectionRange: rng,
			Children:       children,
		}

	case *ast.EnumDecl:
		var children []types.DocumentSymbol
		for _, v := range n.Variants {
			p := n.Pos()
			varRange := types.Range{
				Start: types.Position{Line: uint32(p.Line - 1), Character: uint32(p.Col - 1)},
				End:   types.Position{Line: uint32(p.Line - 1), Character: uint32(p.Col - 1 + len(v.Name))},
			}
			children = append(children, types.DocumentSymbol{
				Name:           v.Name,
				Detail:         nil,
				Kind:           types.SymbolEnumMember,
				Range:          varRange,
				SelectionRange: varRange,
				Children:       nil,
			})
		}

		p := n.Pos()
		nameLen := len(n.Name)
		rng := types.Range{
			Start: types.Position{Line: uint32(p.Line - 1), Character: uint32(p.Col - 1)},
			End:   types.Position{Line: uint32(p.Line - 1), Character: uint32(p.Col - 1 + nameLen)},
		}

		return &types.DocumentSymbol{
			Name:           n.Name,
			Detail:         nil,
			Kind:           types.SymbolEnum,
			Range:          rng,
			SelectionRange: rng,
			Children:       children,
		}

	case *ast.VarDecl:
		p := n.Pos()
		nameLen := len(n.Name)
		rng := types.Range{
			Start: types.Position{Line: uint32(p.Line - 1), Character: uint32(p.Col - 1)},
			End:   types.Position{Line: uint32(p.Line - 1), Character: uint32(p.Col - 1 + nameLen)},
		}

		return &types.DocumentSymbol{
			Name:           n.Name,
			Detail:         nil,
			Kind:           types.SymbolVariable,
			Range:          rng,
			SelectionRange: rng,
			Children:       nil,
		}

	case *ast.ForStmt:
		var children []types.DocumentSymbol
		for _, bodyNode := range n.Body {
			if child := buildDocumentSymbol(bodyNode); child != nil {
				children = append(children, *child)
			}
		}

		p := n.Pos()
		nameLen := len(n.VarName)
		rng := types.Range{
			Start: types.Position{Line: uint32(p.Line - 1), Character: uint32(p.Col - 1)},
			End:   types.Position{Line: uint32(p.Line - 1), Character: uint32(p.Col - 1 + nameLen)},
		}

		return &types.DocumentSymbol{
			Name:           n.VarName,
			Detail:         nil,
			Kind:           types.SymbolVariable,
			Range:          rng,
			SelectionRange: rng,
			Children:       children,
		}
	}
	return nil
}
