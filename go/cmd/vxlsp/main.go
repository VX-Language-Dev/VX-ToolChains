package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"strconv"
	"strings"

	"github.com/vx-lang/vxc/internal/lsp"
)

func main() {
	backend := lsp.NewVxLspBackend()

	scanner := bufio.NewScanner(os.Stdin)
	defer func() {
		if err := scanner.Err(); err != nil {
			// 处理扫描错误
		}
	}()
	reader := bufio.NewReader(os.Stdin)
	writer := bufio.NewWriter(os.Stdout)

	for {
		var contentLength int

		for {
			scanner.Scan()
			line := scanner.Text()

			if line == "" {
				break
			}

			if strings.HasPrefix(line, "Content-Length: ") {
				lenStr := line[16:]
				var err error
				contentLength, err = strconv.Atoi(lenStr)
				if err != nil {
					contentLength = 0
				}
			}
		}

		if contentLength <= 0 {
			continue
		}

		body := make([]byte, contentLength)
		_, err := io.ReadFull(reader, body)
		if err != nil {
			continue
		}

		response := processRequest(backend, body)
		if response != nil {
			fmt.Fprintf(writer, "Content-Length: %d\r\n\r\n", len(response))
			writer.Write(response)
			writer.Flush()
		}
	}
}

func processRequest(backend *lsp.VxLspBackend, body []byte) []byte {
	var req map[string]interface{}
	if err := json.Unmarshal(body, &req); err != nil {
		return nil
	}

	method, ok := req["method"].(string)
	if !ok {
		return nil
	}

	id := req["id"]
	params := req["params"]

	switch method {
	case "initialize":
		return handleInitialize(id)
	case "initialized":
		return nil
	case "shutdown":
		return handleResponse(id, nil)
	case "exit":
		return nil
	case "textDocument/didOpen":
		handleDidOpen(backend, params)
		return nil
	case "textDocument/didChange":
		handleDidChange(backend, params)
		return nil
	case "textDocument/completion":
		result := handleCompletion(backend, params)
		return handleResponse(id, result)
	case "textDocument/hover":
		result := handleHover(backend, params)
		return handleResponse(id, result)
	case "textDocument/definition":
		result := handleDefinition(backend, params)
		return handleResponse(id, result)
	case "textDocument/documentSymbol":
		result := handleDocumentSymbol(backend, params)
		return handleResponse(id, result)
	case "textDocument/inlayHint":
		result := handleInlayHint(backend, params)
		return handleResponse(id, result)
	case "textDocument/publishDiagnostics":
		return nil
	default:
		if id != nil {
			return handleResponse(id, nil)
		}
		return nil
	}
}

func handleInitialize(id interface{}) []byte {
	result := map[string]interface{}{
		"capabilities": map[string]interface{}{
			"textDocumentSync":       1,
			"completionProvider":     true,
			"hoverProvider":          true,
			"definitionProvider":     true,
			"documentSymbolProvider": true,
			"inlayHintProvider":      true,
		},
	}
	return handleResponse(id, result)
}

func handleResponse(id interface{}, result interface{}) []byte {
	resp := map[string]interface{}{}
	if id != nil {
		resp["id"] = id
	}
	if result != nil {
		resp["result"] = result
	} else {
		resp["result"] = nil
	}

	data, _ := json.Marshal(resp)
	return data
}

func handleDidOpen(backend *lsp.VxLspBackend, params interface{}) {
	p, ok := params.(map[string]interface{})
	if !ok {
		return
	}

	td, ok := p["textDocument"].(map[string]interface{})
	if !ok {
		return
	}

	uri, _ := td["uri"].(string)
	text, _ := td["text"].(string)

	backend.AnalyzeAndPublish(uri, text)
}

func handleDidChange(backend *lsp.VxLspBackend, params interface{}) {
	p, ok := params.(map[string]interface{})
	if !ok {
		return
	}

	td, ok := p["textDocument"].(map[string]interface{})
	if !ok {
		return
	}

	uri, _ := td["uri"].(string)

	changes, ok := p["contentChanges"].([]interface{})
	if !ok || len(changes) == 0 {
		return
	}

	lastChange, ok := changes[len(changes)-1].(map[string]interface{})
	if !ok {
		return
	}

	text, _ := lastChange["text"].(string)

	backend.AnalyzeAndPublish(uri, text)
}

func handleCompletion(backend *lsp.VxLspBackend, params interface{}) []lsp.CompletionItem {
	p, ok := params.(map[string]interface{})
	if !ok {
		return nil
	}

	td, ok := p["textDocument"].(map[string]interface{})
	if !ok {
		return nil
	}

	uri, _ := td["uri"].(string)

	pos, ok := p["position"].(map[string]interface{})
	if !ok {
		return nil
	}

	line := uint32(0)
	if l, ok := pos["line"].(float64); ok {
		line = uint32(l)
	}

	character := uint32(0)
	if c, ok := pos["character"].(float64); ok {
		character = uint32(c)
	}

	return backend.HandleCompletion(uri, lsp.Position{Line: line, Character: character})
}

func handleHover(backend *lsp.VxLspBackend, params interface{}) *lsp.Hover {
	p, ok := params.(map[string]interface{})
	if !ok {
		return nil
	}

	td, ok := p["textDocument"].(map[string]interface{})
	if !ok {
		return nil
	}

	uri, _ := td["uri"].(string)

	pos, ok := p["position"].(map[string]interface{})
	if !ok {
		return nil
	}

	line := uint32(0)
	if l, ok := pos["line"].(float64); ok {
		line = uint32(l)
	}

	character := uint32(0)
	if c, ok := pos["character"].(float64); ok {
		character = uint32(c)
	}

	return backend.HandleHover(uri, lsp.Position{Line: line, Character: character})
}

func handleDefinition(backend *lsp.VxLspBackend, params interface{}) *lsp.DocumentSymbol {
	p, ok := params.(map[string]interface{})
	if !ok {
		return nil
	}

	td, ok := p["textDocument"].(map[string]interface{})
	if !ok {
		return nil
	}

	uri, _ := td["uri"].(string)

	pos, ok := p["position"].(map[string]interface{})
	if !ok {
		return nil
	}

	line := uint32(0)
	if l, ok := pos["line"].(float64); ok {
		line = uint32(l)
	}

	character := uint32(0)
	if c, ok := pos["character"].(float64); ok {
		character = uint32(c)
	}

	return backend.HandleGotoDefinition(uri, lsp.Position{Line: line, Character: character})
}

func handleDocumentSymbol(backend *lsp.VxLspBackend, params interface{}) []lsp.DocumentSymbol {
	p, ok := params.(map[string]interface{})
	if !ok {
		return nil
	}

	td, ok := p["textDocument"].(map[string]interface{})
	if !ok {
		return nil
	}

	uri, _ := td["uri"].(string)

	return backend.HandleDocumentSymbols(uri)
}

func handleInlayHint(backend *lsp.VxLspBackend, params interface{}) []lsp.InlayHint {
	p, ok := params.(map[string]interface{})
	if !ok {
		return nil
	}

	td, ok := p["textDocument"].(map[string]interface{})
	if !ok {
		return nil
	}

	uri, _ := td["uri"].(string)

	rg, ok := p["range"].(map[string]interface{})
	if !ok {
		return nil
	}

	start, ok := rg["start"].(map[string]interface{})
	if !ok {
		return nil
	}

	end, ok := rg["end"].(map[string]interface{})
	if !ok {
		return nil
	}

	startLine := uint32(0)
	if l, ok := start["line"].(float64); ok {
		startLine = uint32(l)
	}

	startChar := uint32(0)
	if c, ok := start["character"].(float64); ok {
		startChar = uint32(c)
	}

	endLine := uint32(0)
	if l, ok := end["line"].(float64); ok {
		endLine = uint32(l)
	}

	endChar := uint32(0)
	if c, ok := end["character"].(float64); ok {
		endChar = uint32(c)
	}

	return backend.HandleInlayHints(uri, lsp.Position{Line: startLine, Character: startChar}, lsp.Position{Line: endLine, Character: endChar})
}
