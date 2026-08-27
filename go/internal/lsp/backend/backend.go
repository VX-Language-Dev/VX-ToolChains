package backend

import (
	"github.com/vx-lang/vxc/internal/lsp/completion"
	"github.com/vx-lang/vxc/internal/lsp/definition"
	"github.com/vx-lang/vxc/internal/lsp/diagnostics"
	"github.com/vx-lang/vxc/internal/lsp/hover"
	"github.com/vx-lang/vxc/internal/lsp/inlay_hints"
	"github.com/vx-lang/vxc/internal/lsp/state"
	"github.com/vx-lang/vxc/internal/lsp/symbols"
	"github.com/vx-lang/vxc/internal/lsp/types"
)

type VxLspBackend struct {
	backendState state.BackendState
}

func NewVxLspBackend() *VxLspBackend {
	return &VxLspBackend{
		backendState: state.NewBackendState(),
	}
}

func (b *VxLspBackend) AnalyzeAndPublish(uri, source string) ([]types.Diagnostic, error) {
	result, err := diagnostics.RunDiagnostics(uri, source, true)
	if err != nil {
		return nil, err
	}

	docState := state.NewDocumentState()
	docState.Source = source
	docState.Tokens = result.Tokens
	docState.AstNodes = result.AstNodes
	docState.Diagnostics = result.Diagnostics
	b.backendState.PutDocument(uri, docState)

	return result.Diagnostics, nil
}

func (b *VxLspBackend) HandleCompletion(params types.CompletionParams) (types.CompletionResponse, error) {
	uri := params.TextDocument.URI

	doc := b.backendState.GetDocument(uri)
	if doc == nil {
		return types.CompletionResponse{Items: []types.CompletionItem{}}, nil
	}

	symbolsList, err := completion.CollectSymbols(doc.AstNodes)
	if err != nil {
		return types.CompletionResponse{}, err
	}

	items := make([]types.CompletionItem, 0, len(symbolsList)+30)

	for _, sym := range symbolsList {
		var kind types.CompletionItemKind
		switch sym.Kind {
		case completion.SymbolFunction:
			kind = types.CompletionItemFunction
		case completion.SymbolVariable:
			kind = types.CompletionItemVariable
		case completion.SymbolStruct:
			kind = types.CompletionItemStruct
		case completion.SymbolClass:
			kind = types.CompletionItemClass
		case completion.SymbolEnum:
			kind = types.CompletionItemEnum
		case completion.SymbolUnion:
			kind = types.CompletionItemStruct
		case completion.SymbolTypeAlias:
			kind = types.CompletionItemTypeParameter
		case completion.SymbolLoopVar:
			kind = types.CompletionItemVariable
		case completion.SymbolParam:
			kind = types.CompletionItemVariable
		default:
			kind = types.CompletionItemText
		}

		items = append(items, types.CompletionItem{
			Label:         sym.Name,
			Kind:          kind,
			Detail:        sym.Detail,
			Documentation: nil,
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
		items = append(items, types.CompletionItem{
			Label:         kw,
			Kind:          types.CompletionItemKeyword,
			Detail:        nil,
			Documentation: nil,
		})
	}

	return types.CompletionResponse{Items: items}, nil
}

func (b *VxLspBackend) HandleHover(params types.HoverParams) (*types.Hover, error) {
	uri := params.TextDocument.URI

	doc := b.backendState.GetDocument(uri)
	if doc == nil {
		return nil, nil
	}

	return hover.Hover(doc.AstNodes, doc.Tokens, doc.Source, params.Position)
}

func (b *VxLspBackend) HandleGotoDefinition(params types.GotoDefinitionParams) (*types.GotoDefinitionResponse, error) {
	uri := params.TextDocument.URI

	doc := b.backendState.GetDocument(uri)
	if doc == nil {
		return nil, nil
	}

	return definition.GotoDefinition(doc.AstNodes, doc.Source, params.Position, uri)
}

func (b *VxLspBackend) HandleDocumentSymbols(params types.DocumentSymbolParams) ([]types.DocumentSymbol, error) {
	uri := params.TextDocument.URI

	doc := b.backendState.GetDocument(uri)
	if doc == nil {
		return []types.DocumentSymbol{}, nil
	}

	return symbols.DocumentSymbols(doc.AstNodes)
}

func (b *VxLspBackend) HandleInlayHints(params types.InlayHintParams) ([]types.InlayHint, error) {
	uri := params.TextDocument.URI

	doc := b.backendState.GetDocument(uri)
	if doc == nil {
		return []types.InlayHint{}, nil
	}

	return inlay_hints.CollectInlayHints(doc.AstNodes)
}

func (b *VxLspBackend) GetDiagnostics(uri string) ([]types.Diagnostic, error) {
	doc := b.backendState.GetDocument(uri)
	if doc == nil {
		return []types.Diagnostic{}, nil
	}

	return doc.Diagnostics, nil
}
