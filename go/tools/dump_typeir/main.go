package main

import (
	"fmt"
	"os"

	"github.com/vx-lang/vxc/internal/bytecode"
	"github.com/vx-lang/vxc/internal/typeir"
)

func main() {
	if len(os.Args) < 2 {
		fmt.Println("Usage: dump_typeir <file.vxobj>")
		os.Exit(1)
	}
	data, err := os.ReadFile(os.Args[1])
	if err != nil {
		panic(err)
	}
	container, err := bytecode.ParseVxobjV4(data)
	if err != nil {
		panic(err)
	}
	section := container.GetSection(bytecode.SECTION_TYPE_IR)
	if section == nil {
		panic("no TypeIR section")
	}
	module, err := typeir.DeserializeTypeModule(section)
	if err != nil {
		panic(err)
	}
	fmt.Printf("Module: %s\n", module.Name)
	fmt.Printf("Functions: %d\n", len(module.Functions))
	fmt.Printf("Linkage entries: %d\n", len(module.Linkage))
	fmt.Println()

	for _, fn := range module.Functions {
		linkage, hasLinkage := module.Linkage[fn.ID]
		ext := ""
		if hasLinkage && linkage.IsExternal {
			ext = fmt.Sprintf(" [EXTERN: %s]", linkage.Name)
		}
		fmt.Printf("  ID=%d name=%s hasReturn=%v retType=%v params=%d body=%d%s\n",
			fn.ID, fn.Name, fn.HasReturn, fn.ReturnType, fn.ParamCount, len(fn.Body), ext)
	}
	fmt.Println()

	for id, entry := range module.Linkage {
		fmt.Printf("  Linkage[%d]: external=%v name=%q\n", id, entry.IsExternal, entry.Name)
	}
}
