package state

import (
	"github.com/vx-lang/vxc/internal/ast"
	"github.com/vx-lang/vxc/internal/lsp/types"
	"github.com/vx-lang/vxc/internal/token"
)

type DocumentState struct {
	Source      string
	Tokens      []token.Token
	AstNodes    []ast.Expr
	Diagnostics []types.Diagnostic
}

func NewDocumentState() *DocumentState {
	return &DocumentState{
		Source:      "",
		Tokens:      nil,
		AstNodes:    nil,
		Diagnostics: nil,
	}
}

type BackendState struct {
	Documents map[string]*DocumentState
}

func NewBackendState() BackendState {
	return BackendState{
		Documents: make(map[string]*DocumentState),
	}
}

func (bs *BackendState) GetDocument(uri string) *DocumentState {
	return bs.Documents[uri]
}

func (bs *BackendState) PutDocument(uri string, state *DocumentState) {
	bs.Documents[uri] = state
}
