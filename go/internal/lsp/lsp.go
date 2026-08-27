package lsp

import (
	"github.com/vx-lang/vxc/internal/ast"
	"github.com/vx-lang/vxc/internal/lsp/completion"
	"github.com/vx-lang/vxc/internal/lsp/definition"
	"github.com/vx-lang/vxc/internal/lsp/diagnostics"
	"github.com/vx-lang/vxc/internal/lsp/hover"
	"github.com/vx-lang/vxc/internal/lsp/inlay_hints"
	"github.com/vx-lang/vxc/internal/lsp/symbols"
	"github.com/vx-lang/vxc/internal/lsp/types"
	"github.com/vx-lang/vxc/internal/token"
)

// Diagnostic LSP 诊断信息
type Diagnostic struct {
	Range    Range
	Severity int
	Message  string
	Source   string
	Code     string
}

// Range LSP 范围
type Range struct {
	Start Position
	End   Position
}

// Position LSP 位置
type Position struct {
	Line      uint32
	Character uint32
}

// CompletionItem 补全项
type CompletionItem struct {
	Label  string
	Kind   int
	Detail string
}

// Hover 悬停信息
type Hover struct {
	Contents string
}

// DocumentSymbol 文档符号
type DocumentSymbol struct {
	Name  string
	Kind  int
	Range Range
}

// InlayHint 内嵌提示
type InlayHint struct {
	Position Position
	Label    string
}

// DocumentState 文档状态
type DocumentState struct {
	Source   string
	Tokens   []token.Token
	AstNodes []ast.Expr
}

// VxLspBackend LSP 后端
type VxLspBackend struct {
	documents map[string]*DocumentState
}

// NewVxLspBackend 创建新的 LSP 后端
func NewVxLspBackend() *VxLspBackend {
	return &VxLspBackend{
		documents: make(map[string]*DocumentState),
	}
}

// AnalyzeAndPublish 分析并发布诊断
func (b *VxLspBackend) AnalyzeAndPublish(uri, text string) []Diagnostic {
	result, err := diagnostics.RunDiagnostics(uri, text, true)
	if err != nil {
		return nil
	}

	b.documents[uri] = &DocumentState{
		Source:   text,
		Tokens:   result.Tokens,
		AstNodes: result.AstNodes,
	}

	lspDiags := make([]Diagnostic, 0, len(result.Diagnostics))
	for _, diag := range result.Diagnostics {
		lspDiags = append(lspDiags, Diagnostic{
			Range: Range{
				Start: Position{
					Line:      diag.Range.Start.Line,
					Character: diag.Range.Start.Character,
				},
				End: Position{
					Line:      diag.Range.End.Line,
					Character: diag.Range.End.Character,
				},
			},
			Severity: int(diag.Severity),
			Message:  diag.Message,
			Source:   diag.Source,
		})
	}

	return lspDiags
}

// HandleCompletion 处理补全请求
func (b *VxLspBackend) HandleCompletion(uri string, pos Position) []CompletionItem {
	doc := b.documents[uri]
	if doc == nil {
		return nil
	}

	symbolsList, err := completion.CollectSymbols(doc.AstNodes)
	if err != nil {
		return nil
	}

	items := make([]CompletionItem, 0, len(symbolsList))
	for _, sym := range symbolsList {
		var kind int
		switch sym.Kind {
		case completion.SymbolFunction:
			kind = int(types.CompletionItemFunction)
		case completion.SymbolVariable:
			kind = int(types.CompletionItemVariable)
		case completion.SymbolStruct:
			kind = int(types.CompletionItemStruct)
		case completion.SymbolClass:
			kind = int(types.CompletionItemClass)
		case completion.SymbolEnum:
			kind = int(types.CompletionItemEnum)
		case completion.SymbolUnion:
			kind = int(types.CompletionItemStruct)
		case completion.SymbolLoopVar:
			kind = int(types.CompletionItemVariable)
		default:
			kind = int(types.CompletionItemText)
		}

		detail := ""
		if sym.Detail != nil {
			detail = *sym.Detail
		}

		items = append(items, CompletionItem{
			Label:  sym.Name,
			Kind:   kind,
			Detail: detail,
		})
	}

	keywords := []string{
		"if", "elif", "else", "for", "while", "break", "continue",
		"func", "return", "import", "as", "struct", "class", "enum",
		"union", "new", "move", "int", "float", "double", "bool",
		"void", "true", "false", "nil", "loop", "match", "extern",
		"macro", "mut",
	}

	for _, kw := range keywords {
		items = append(items, CompletionItem{
			Label:  kw,
			Kind:   int(types.CompletionItemKeyword),
			Detail: "",
		})
	}

	return items
}

// HandleHover 处理悬停请求
func (b *VxLspBackend) HandleHover(uri string, pos Position) *Hover {
	doc := b.documents[uri]
	if doc == nil {
		return nil
	}

	result, err := hover.Hover(doc.AstNodes, doc.Tokens, doc.Source, types.Position{
		Line:      pos.Line,
		Character: pos.Character,
	})
	if err != nil || result == nil {
		return nil
	}

	return &Hover{
		Contents: result.Contents.Scalar.Value,
	}
}

// HandleGotoDefinition 处理跳转定义请求
func (b *VxLspBackend) HandleGotoDefinition(uri string, pos Position) *DocumentSymbol {
	doc := b.documents[uri]
	if doc == nil {
		return nil
	}

	result, err := definition.GotoDefinition(doc.AstNodes, doc.Source, types.Position{
		Line:      pos.Line,
		Character: pos.Character,
	}, uri)
	if err != nil || result == nil {
		return nil
	}

	return &DocumentSymbol{
		Name: "",
		Kind: 0,
		Range: Range{
			Start: Position{
				Line:      result.Range.Start.Line,
				Character: result.Range.Start.Character,
			},
			End: Position{
				Line:      result.Range.End.Line,
				Character: result.Range.End.Character,
			},
		},
	}
}

// HandleDocumentSymbols 处理文档符号请求
func (b *VxLspBackend) HandleDocumentSymbols(uri string) []DocumentSymbol {
	doc := b.documents[uri]
	if doc == nil {
		return nil
	}

	syms, err := symbols.DocumentSymbols(doc.AstNodes)
	if err != nil {
		return nil
	}

	result := make([]DocumentSymbol, 0, len(syms))
	for _, sym := range syms {
		result = append(result, DocumentSymbol{
			Name: sym.Name,
			Kind: int(sym.Kind),
			Range: Range{
				Start: Position{
					Line:      sym.Range.Start.Line,
					Character: sym.Range.Start.Character,
				},
				End: Position{
					Line:      sym.Range.End.Line,
					Character: sym.Range.End.Character,
				},
			},
		})
	}

	return result
}

// HandleInlayHints 处理内嵌提示请求
func (b *VxLspBackend) HandleInlayHints(uri string, start, end Position) []InlayHint {
	doc := b.documents[uri]
	if doc == nil {
		return nil
	}

	hints, err := inlay_hints.CollectInlayHints(doc.AstNodes)
	if err != nil {
		return nil
	}

	result := make([]InlayHint, 0, len(hints))
	for _, hint := range hints {
		label := ""
		if hint.Label.String != nil {
			label = *hint.Label.String
		}
		result = append(result, InlayHint{
			Position: Position{
				Line:      hint.Position.Line,
				Character: hint.Position.Character,
			},
			Label: label,
		})
	}

	return result
}
