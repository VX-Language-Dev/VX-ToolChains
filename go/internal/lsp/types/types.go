package types

type Position struct {
	Line      uint32 `json:"line"`
	Character uint32 `json:"character"`
}

type Range struct {
	Start Position `json:"start"`
	End   Position `json:"end"`
}

type TextDocumentIdentifier struct {
	URI string `json:"uri"`
}

type TextDocumentItem struct {
	URI        string `json:"uri"`
	LanguageID string `json:"languageId"`
	Version    int64  `json:"version"`
	Text       string `json:"text"`
}

type VersionedTextDocumentIdentifier struct {
	URI     string `json:"uri"`
	Version *int64 `json:"version"`
}

type TextDocumentContentChangeEvent struct {
	Range *Range `json:"range"`
	Text  string `json:"text"`
}

type Location struct {
	URI   string `json:"uri"`
	Range Range  `json:"range"`
}

type Diagnostic struct {
	Range    Range    `json:"range"`
	Severity Severity `json:"severity"`
	Code     *string  `json:"code"`
	Source   string   `json:"source"`
	Message  string   `json:"message"`
}

type Severity uint8

const (
	SeverityError       Severity = 1
	SeverityWarning     Severity = 2
	SeverityInformation Severity = 3
	SeverityHint        Severity = 4
)

type CompletionItem struct {
	Label         string             `json:"label"`
	Kind          CompletionItemKind `json:"kind"`
	Detail        *string            `json:"detail"`
	Documentation *string            `json:"documentation"`
}

type CompletionItemKind uint8

const (
	CompletionItemText          CompletionItemKind = 1
	CompletionItemMethod        CompletionItemKind = 2
	CompletionItemFunction      CompletionItemKind = 3
	CompletionItemConstructor   CompletionItemKind = 4
	CompletionItemField         CompletionItemKind = 5
	CompletionItemVariable      CompletionItemKind = 6
	CompletionItemClass         CompletionItemKind = 7
	CompletionItemInterface     CompletionItemKind = 8
	CompletionItemModule        CompletionItemKind = 9
	CompletionItemProperty      CompletionItemKind = 10
	CompletionItemUnit          CompletionItemKind = 11
	CompletionItemValue         CompletionItemKind = 12
	CompletionItemEnum          CompletionItemKind = 13
	CompletionItemKeyword       CompletionItemKind = 14
	CompletionItemSnippet       CompletionItemKind = 15
	CompletionItemColor         CompletionItemKind = 16
	CompletionItemFile          CompletionItemKind = 17
	CompletionItemReference     CompletionItemKind = 18
	CompletionItemFolder        CompletionItemKind = 19
	CompletionItemEnumMember    CompletionItemKind = 20
	CompletionItemConstant      CompletionItemKind = 21
	CompletionItemStruct        CompletionItemKind = 22
	CompletionItemEvent         CompletionItemKind = 23
	CompletionItemOperator      CompletionItemKind = 24
	CompletionItemTypeParameter CompletionItemKind = 25
)

type SymbolKind uint16

const (
	SymbolFile          SymbolKind = 1
	SymbolModule        SymbolKind = 2
	SymbolNamespace     SymbolKind = 3
	SymbolPackage       SymbolKind = 4
	SymbolClass         SymbolKind = 5
	SymbolMethod        SymbolKind = 6
	SymbolProperty      SymbolKind = 7
	SymbolField         SymbolKind = 8
	SymbolConstructor   SymbolKind = 9
	SymbolEnum          SymbolKind = 10
	SymbolInterface     SymbolKind = 11
	SymbolFunction      SymbolKind = 12
	SymbolVariable      SymbolKind = 13
	SymbolConstant      SymbolKind = 14
	SymbolString        SymbolKind = 15
	SymbolNumber        SymbolKind = 16
	SymbolBoolean       SymbolKind = 17
	SymbolArray         SymbolKind = 18
	SymbolObject        SymbolKind = 19
	SymbolKey           SymbolKind = 20
	SymbolNull          SymbolKind = 21
	SymbolEnumMember    SymbolKind = 22
	SymbolStruct        SymbolKind = 23
	SymbolEvent         SymbolKind = 24
	SymbolOperator      SymbolKind = 25
	SymbolTypeParameter SymbolKind = 26
)

type DocumentSymbol struct {
	Name           string           `json:"name"`
	Detail         *string          `json:"detail"`
	Kind           SymbolKind       `json:"kind"`
	Range          Range            `json:"range"`
	SelectionRange Range            `json:"selectionRange"`
	Children       []DocumentSymbol `json:"children"`
}

type Hover struct {
	Contents HoverContents `json:"contents"`
	Range    *Range        `json:"range"`
}

type HoverContents struct {
	Scalar *MarkupContent  `json:"-"`
	Array  []MarkupContent `json:"-"`
}

func (h HoverContents) MarshalJSON() ([]byte, error) {
	if h.Scalar != nil {
		return []byte(`{"kind":"markdown","value":"` + escapeJSON(h.Scalar.Value) + `"}`), nil
	}
	return []byte("[]"), nil
}

func escapeJSON(s string) string {
	result := make([]byte, 0, len(s))
	for _, c := range s {
		switch c {
		case '"':
			result = append(result, '\\', '"')
		case '\\':
			result = append(result, '\\', '\\')
		case '\n':
			result = append(result, '\\', 'n')
		case '\r':
			result = append(result, '\\', 'r')
		case '\t':
			result = append(result, '\\', 't')
		default:
			result = append(result, string(c)...)
		}
	}
	return string(result)
}

type MarkupContent struct {
	Kind  MarkupKind `json:"kind"`
	Value string     `json:"value"`
}

type MarkupKind string

const (
	MarkupKindPlainText MarkupKind = "plaintext"
	MarkupKindMarkdown  MarkupKind = "markdown"
)

type InlayHint struct {
	Position     Position       `json:"position"`
	Label        InlayHintLabel `json:"label"`
	Kind         *InlayHintKind `json:"kind"`
	Tooltip      *string        `json:"tooltip"`
	PaddingLeft  bool           `json:"paddingLeft"`
	PaddingRight bool           `json:"paddingRight"`
}

type InlayHintLabel struct {
	String *string              `json:"-"`
	Parts  []InlayHintLabelPart `json:"-"`
}

func (i InlayHintLabel) MarshalJSON() ([]byte, error) {
	if i.String != nil {
		return []byte(`"` + escapeJSON(*i.String) + `"`), nil
	}
	return []byte("[]"), nil
}

type InlayHintLabelPart struct {
	Value    string    `json:"value"`
	Tooltip  *string   `json:"tooltip"`
	Location *Location `json:"location"`
	Command  *Command  `json:"command"`
}

type InlayHintKind uint8

const (
	InlayHintKindType      InlayHintKind = 1
	InlayHintKindParameter InlayHintKind = 2
)

type Command struct {
	Title     string   `json:"title"`
	Command   string   `json:"command"`
	Arguments []string `json:"arguments"`
}

type GotoDefinitionResponse struct {
	URI   string `json:"uri"`
	Range Range  `json:"range"`
}

type CompletionResponse struct {
	Items []CompletionItem `json:"items"`
}

type DocumentSymbolParams struct {
	TextDocument TextDocumentIdentifier `json:"textDocument"`
}

type HoverParams struct {
	TextDocument TextDocumentIdentifier `json:"textDocument"`
	Position     Position               `json:"position"`
}

type CompletionParams struct {
	TextDocument TextDocumentIdentifier `json:"textDocument"`
	Position     Position               `json:"position"`
}

type GotoDefinitionParams struct {
	TextDocument TextDocumentIdentifier `json:"textDocument"`
	Position     Position               `json:"position"`
}

type InlayHintParams struct {
	TextDocument TextDocumentIdentifier `json:"textDocument"`
	Range        Range                  `json:"range"`
}

type InitializeParams struct {
	ProcessId    *int64             `json:"processId"`
	RootUri      *string            `json:"rootUri"`
	Capabilities ClientCapabilities `json:"capabilities"`
}

type ClientCapabilities struct {
	TextDocument *TextDocumentClientCapabilities `json:"textDocument"`
}

type TextDocumentClientCapabilities struct {
	Completion *struct {
		CompletionItem *struct {
			SnippetSupport bool `json:"snippetSupport"`
		} `json:"completionItem"`
	} `json:"completion"`
	Hover *struct {
		ContentFormat []string `json:"contentFormat"`
	} `json:"hover"`
	PublishDiagnostics *struct{} `json:"publishDiagnostics"`
	InlayHint          *struct{} `json:"inlayHint"`
}

type InitializeResult struct {
	Capabilities ServerCapabilities `json:"capabilities"`
}

type ServerCapabilities struct {
	TextDocumentSync       uint8 `json:"textDocumentSync"`
	CompletionProvider     bool  `json:"completionProvider"`
	HoverProvider          bool  `json:"hoverProvider"`
	DefinitionProvider     bool  `json:"definitionProvider"`
	DocumentSymbolProvider bool  `json:"documentSymbolProvider"`
	InlayHintProvider      bool  `json:"inlayHintProvider"`
}

type PublishDiagnosticsParams struct {
	URI         string       `json:"uri"`
	Diagnostics []Diagnostic `json:"diagnostics"`
}
