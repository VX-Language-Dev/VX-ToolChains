package lexer

import (
	"fmt"
	"os"
	"testing"

	"github.com/vx-lang/vxc/internal/token"
)

func TestTraceTokens(t *testing.T) {
	content := readTestFile(t, "/Users/jiulong2023/Documents/vx-toolkit/std/tests/run_all.vx")
	l := NewLexer(content)
	for i := 0; i < 30; i++ {
		tok := l.NextToken()
		fmt.Printf("token %d: Kind=%d, Value=%q, Line=%d, Col=%d\n",
			i, tok.Kind, tok.Value, tok.Line, tok.Col)
		if tok.Kind == token.EOF {
			break
		}
	}
}

func readTestFile(t *testing.T, path string) string {
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("读取文件失败: %v", err)
	}
	return string(data)
}
