package translator

import (
	"fmt"
	"testing"
	"github.com/vx-lang/vxc/internal/typeir"
)

func TestDebugIR(t *testing.T) {
	module := &typeir.TypeModule{
		Name: "test",
		Functions: []typeir.FunctionIR{
			{
				ID:         0,
				Name:       "main",
				HasReturn:  true,
				ReturnType: typeir.Int,
				Body: []typeir.TypedInstruction{
					{Op: typeir.ConstInt, Args: []string{"42"}},
					{Op: typeir.Return, Args: []string{"int"}},
				},
			},
		},
		StructTypes: []typeir.StructTypeDef{},
		Linkage:     make(map[uint32]typeir.LinkageEntry),
	}
	
	target := HostTarget()
	trans := NewTranslator(target)
	
	ir, err := trans.GenerateLLVMIR(module)
	if err != nil {
		t.Fatalf("GenerateLLVMIR failed: %v", err)
	}
	fmt.Println("=== GENERATED IR ===")
	fmt.Println(ir)
	fmt.Println("=== END IR ===")
}
