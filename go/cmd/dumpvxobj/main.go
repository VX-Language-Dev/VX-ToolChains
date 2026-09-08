package main

import (
	"fmt"
	"os"

	"github.com/vx-lang/vxc/internal/bytecode"
	"github.com/vx-lang/vxc/internal/typeir"
)

func main() {
	data, err := os.ReadFile(os.Args[1])
	if err != nil {
		panic(err)
	}
	container, err := bytecode.ParseVxobjV4(data)
	if err != nil {
		panic(err)
	}
	payload := container.GetSection("TypeIR")
	if payload == nil {
		fmt.Println("no TypeIR section")
		return
	}
	m, err := typeir.DeserializeTypeModule(payload)
	if err != nil {
		panic(err)
	}
	fmt.Printf("functions=%d globals=%d structs=%d\n", len(m.Functions), len(m.Globals), len(m.StructTypes))
	for _, g := range m.Globals {
		fmt.Printf("  global %s type=%s\n", g.Name, typeir.TypeName(g.Type))
	}
	if len(os.Args) > 2 {
		for _, fn := range m.Functions {
			if fn.Name == os.Args[2] {
				fmt.Printf("=== %s ===\n", fn.Name)
				body := fn.Body
				if len(body) == 0 {
					body = fn.Instructions
				}
				for i, inst := range body {
					fmt.Printf("%4d: %v args=%v\n", i, inst.Op, inst.Args)
				}
			}
		}
	}
}
