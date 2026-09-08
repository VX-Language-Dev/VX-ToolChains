package typeir

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"io"
	"strings"
)

// SerializeTypeModule serializes a TypeModule to binary format.
func SerializeTypeModule(module *TypeModule) ([]byte, error) {
	var buf bytes.Buffer

	writeU32 := func(v uint32) {
		_ = binary.Write(&buf, binary.BigEndian, v)
	}
	writeStr := func(s string) {
		writeU32(uint32(len(s)))
		_, _ = buf.WriteString(s)
	}

	writeU32(uint32(len(module.Functions)))
	writeU32(uint32(len(module.StructTypes)))

	for _, st := range module.StructTypes {
		writeStr(st.Name)
		writeU32(uint32(len(st.Fields)))
		for _, f := range st.Fields {
			writeStr(f.Name)
			if err := serializeType(&buf, f.FieldType); err != nil {
				return nil, fmt.Errorf("field %s type: %w", f.Name, err)
			}
		}
	}

	for _, fn := range module.Functions {
		writeStr(fn.Name)
		writeU32(fn.ID)
		writeU32(fn.ParamCount)

		hasRet := byte(0)
		if fn.HasReturn {
			hasRet = 1
		}
		buf.WriteByte(hasRet)

		if fn.HasReturn {
			if err := serializeType(&buf, fn.ReturnType); err != nil {
				return nil, fmt.Errorf("func %s return type: %w", fn.Name, err)
			}
		} else {
			buf.WriteByte(0) // Void
		}

		for _, p := range fn.Params {
			writeStr(p.Name)
			if err := serializeType(&buf, p.Type); err != nil {
				return nil, fmt.Errorf("func %s param %s type: %w", fn.Name, p.Name, err)
			}
		}

		writeU32(fn.VarCount)

		writeU32(uint32(len(fn.LocalTypes)))
		for vid, ty := range fn.LocalTypes {
			writeU32(vid)
			if err := serializeType(&buf, ty); err != nil {
				return nil, fmt.Errorf("func %s local type: %w", fn.Name, err)
			}
		}

		body := fn.Body
		if len(body) == 0 && len(fn.Instructions) > 0 {
			body = fn.Instructions
		}
		writeU32(uint32(len(body)))
		for _, inst := range body {
			if err := serializeInstruction(&buf, inst); err != nil {
				return nil, fmt.Errorf("func %s inst: %w", fn.Name, err)
			}
		}
	}

	// Linkage table
	writeU32(uint32(len(module.Linkage)))
	for funcID, entry := range module.Linkage {
		writeU32(funcID)
		if entry.IsExternal {
			buf.WriteByte(1)
			writeStr(entry.Name)
		} else {
			buf.WriteByte(0)
		}
	}

	// Globals section (appended at the end so older readers that stop at
	// the linkage table remain compatible).
	writeU32(uint32(len(module.Globals)))
	for _, g := range module.Globals {
		writeStr(g.Name)
		if err := serializeType(&buf, g.Type); err != nil {
			return nil, fmt.Errorf("global %s type: %w", g.Name, err)
		}
	}

	return buf.Bytes(), nil
}

func serializeType(w io.Writer, t Typ) error {
	var writeU32 = func(v uint32) {
		_ = binary.Write(w, binary.BigEndian, v)
	}
	var writeStr = func(s string) {
		writeU32(uint32(len(s)))
		_, _ = io.WriteString(w, s)
	}

	switch x := t.(type) {
	case baseTyp:
		return binary.Write(w, binary.BigEndian, byte(x))
	case StructTyp:
		_, _ = w.Write([]byte{5}) // struct tag
		writeStr(x.Name)
		writeU32(uint32(len(x.Fields)))
		for _, f := range x.Fields {
			writeStr(f.Name)
			if err := serializeType(w, f.FieldType); err != nil {
				return err
			}
		}
		return nil
	case ArrayTyp:
		_, _ = w.Write([]byte{6}) // array tag
		if x.Elem == nil {
			return serializeType(w, Unknown)
		}
		return serializeType(w, x.Elem)
	case MapTyp:
		_, _ = w.Write([]byte{7}) // map tag
		if x.Key == nil {
			if err := serializeType(w, Unknown); err != nil {
				return err
			}
		} else if err := serializeType(w, x.Key); err != nil {
			return err
		}
		if x.Value == nil {
			return serializeType(w, Unknown)
		}
		return serializeType(w, x.Value)
	case PointerTyp:
		_, _ = w.Write([]byte{9}) // pointer tag
		if x.To == nil {
			return serializeType(w, Unknown)
		}
		return serializeType(w, x.To)
	case GenericTyp:
		_, _ = w.Write([]byte{10}) // generic tag
		writeStr(x.Name)
		writeU32(uint32(len(x.Args)))
		for _, a := range x.Args {
			if err := serializeType(w, a); err != nil {
				return err
			}
		}
		return nil
	case FuncTyp:
		_, _ = w.Write([]byte{8}) // func tag
		writeU32(uint32(len(x.Params)))
		for _, p := range x.Params {
			if err := serializeType(w, p); err != nil {
				return err
			}
		}
		if x.Return == nil {
			return serializeType(w, Void)
		}
		return serializeType(w, x.Return)
	default:
		_, _ = w.Write([]byte{255}) // unknown
		return nil
	}
}

func opcodeToTag(op Opcode) (byte, string, error) {
	switch op {
	case ConstInt:
		return 0, "i", nil
	case ConstFloat:
		return 1, "f", nil
	case ConstBool:
		return 2, "b", nil
	case ConstString:
		return 3, "s", nil
	case ConstNil:
		return 4, "", nil
	case LoadVar:
		return 5, "", nil
	case StoreVar:
		return 6, "", nil
	case I32Add:
		return 10, "", nil
	case I32Sub:
		return 11, "", nil
	case I32Mul:
		return 12, "", nil
	case I32Div:
		return 13, "", nil
	case I32Mod:
		return 14, "", nil
	case I64Add:
		return 19, "", nil
	case I64Sub:
		return 45, "", nil
	case I64Mul:
		return 46, "", nil
	case I64Div:
		return 47, "", nil
	case F64Add:
		return 15, "", nil
	case F64Sub:
		return 16, "", nil
	case F64Mul:
		return 17, "", nil
	case F64Div:
		return 18, "", nil
	case I32Eq:
		return 20, "", nil
	case I32Neq:
		return 21, "", nil
	case I32Lt:
		return 22, "", nil
	case I32Gt:
		return 23, "", nil
	case I32Le:
		return 24, "", nil
	case I32Ge:
		return 25, "", nil
	case F64Eq:
		return 26, "", nil
	case F64Ne:
		return 27, "", nil
	case F64Lt:
		return 28, "", nil
	case F64Gt:
		return 29, "", nil
	case F64Le:
		return 30, "", nil
	case F64Ge:
		return 31, "", nil
	case I32Neg:
		return 32, "", nil
	case F64Neg:
		return 33, "", nil
	case BoolNot:
		return 34, "", nil
	case I32And:
		return 35, "", nil
	case I32Or:
		return 36, "", nil
	case Jump:
		return 40, "", nil
	case JumpIfFalse:
		return 41, "", nil
	case JumpIfTrue:
		return 42, "", nil
	case Call:
		return 50, "", nil
	case Return:
		return 52, "", nil
	case ReturnVoid:
		return 53, "", nil
	case StructNew:
		return 60, "", nil
	case FieldLoad:
		return 61, "", nil
	case FieldStore:
		return 62, "", nil
	case ArrayNew:
		return 63, "", nil
	case IndexGet:
		return 64, "", nil
	case IndexSet:
		return 65, "", nil
	case MapNew:
		return 66, "", nil
	case Newz:
		return 70, "", nil
	case Free:
		return 71, "", nil
	case OwnershipMove:
		return 72, "", nil
	case BorrowCheck:
		return 73, "", nil
	case Deref:
		return 74, "", nil
	case AliveCheck:
		return 75, "", nil
	case BoolAnd:
		return 37, "", nil
	case BoolOr:
		return 38, "", nil
	case StrEq:
		return 43, "", nil
	case NeqInt:
		return 44, "", nil
	case Dup:
		return 80, "", nil
	case Pop:
		return 81, "", nil
	case AddressOf:
		return 82, "", nil
	case PropertyGet:
		return 83, "", nil
	case PointerMember:
		return 84, "", nil
	case ScopeDrop:
		return 85, "", nil
	case SysArgv:
		return 86, "", nil
	case System:
		return 87, "", nil
	case FileRead:
		return 88, "", nil
	case FileWrite:
		return 89, "", nil
	case FileExists:
		return 90, "", nil
	case StrConcat:
		return 94, "", nil
	case ArrayConcat:
		return 95, "", nil
	case Noop:
		return 91, "", nil
	case Halt:
		return 93, "", nil
	default:
		return 0, "", fmt.Errorf("unknown opcode %s for serialization", op)
	}
}

func serializeInstruction(w io.Writer, inst TypedInstruction) error {
	tag, prefix, err := opcodeToTag(inst.Op)
	if err != nil {
		return err
	}

	w.Write([]byte{tag}) //nolint:errcheck

	var payload string
	switch inst.Op {
	case ConstInt:
		if len(inst.Args) > 0 {
			payload = prefix + inst.Args[0]
		}
	case ConstFloat:
		if len(inst.Args) > 0 {
			payload = prefix + inst.Args[0]
		}
	case ConstBool:
		if len(inst.Args) > 0 {
			payload = prefix + inst.Args[0]
		}
	case ConstString:
		if len(inst.Args) > 0 {
			payload = prefix + inst.Args[0]
		}
	case LoadVar, StoreVar, Jump, Free, OwnershipMove, BorrowCheck, Deref, AliveCheck, I32Neg, F64Neg, BoolNot,
		Noop, Import, PropertyGet, PointerMember:
		if len(inst.Args) > 0 {
			payload = inst.Args[0]
		}
	case I32Add, I32Sub, I32Mul, I32Div, I32Mod, I64Add, I64Sub, I64Mul, I64Div, F64Add, F64Sub, F64Mul, F64Div,
		I32Eq, I32Neq, I32Lt, I32Gt, I32Le, I32Ge, F64Eq, F64Ne, F64Lt, F64Gt, F64Le, F64Ge,
		I32And, I32Or, BoolAnd, BoolOr, StrEq, NeqInt, JumpIfFalse, JumpIfTrue:
		payload = strings.Join(inst.Args, ",")
	case Return:
		if len(inst.Args) > 0 && inst.Args[0] != "" {
			payload = inst.Args[0]
		}
	case ReturnVoid:
		payload = ""
	case Call:
		// Two accepted arg formats (must round-trip identically):
		//   vxc form:     [";callee", paramType1, ...]
		//   vxbootstrap:  ["funcID", ";callee", paramType1, ...]
		// We normalize to vxc form for storage so the deserializer can
		// unambiguously split callee from paramTypes.
		if len(inst.Args) == 0 {
			break
		}
		// Find the callee marker (first element starting with ';').
		callee := ""
		calleeIdx := -1
		for i, a := range inst.Args {
			if strings.HasPrefix(a, ";") {
				callee = a[1:]
				calleeIdx = i
				break
			}
		}
		// Param types follow the callee marker.
		var paramTypes []string
		if calleeIdx >= 0 && calleeIdx+1 < len(inst.Args) {
			paramTypes = inst.Args[calleeIdx+1:]
		}
		// Wire format: f;<callee>,<paramType1>,<paramType2>,...
		// (funcID intentionally omitted — the vxc form has no funcID,
		// and LLVM IR resolves calls by callee name.)
		payload = "f;" + callee
		if len(paramTypes) > 0 {
			payload += "," + strings.Join(paramTypes, ",")
		}
	case StructNew:
		payload = "s" + strings.Join(inst.Args, ",")
	case FieldLoad, FieldStore, IndexGet, IndexSet:
		payload = strings.Join(inst.Args, ",")
	case ArrayNew:
		payload = strings.Join(inst.Args, ",")
	case MapNew:
		payload = strings.Join(inst.Args, ",")
	case Newz:
		payload = strings.Join(inst.Args, ",")
	case Dup, Pop, ConstNil, AddressOf, ScopeDrop, Halt:
		payload = ""
	default:
		payload = strings.Join(inst.Args, ",")
	}

	if err := binary.Write(w, binary.BigEndian, uint32(len(payload))); err != nil {
		return err
	}
	if payload != "" {
		if _, err := io.WriteString(w, payload); err != nil {
			return err
		}
	}
	return nil
}
