package typeir

import (
	"encoding/binary"
	"fmt"
	"strconv"
	"strings"
)

// LinkageEntry represents a linkage table entry.
type LinkageEntry struct {
	IsExternal bool
	Name       string
}

// FunctionIR represents a deserialized function.
type FunctionIR struct {
	ID           uint32
	Name         string
	ParamCount   uint32
	HasReturn    bool
	ReturnType   Typ
	Params       []ParamIR
	VarCount     uint32
	LocalTypes   map[uint32]Typ
	Body         []TypedInstruction
	Instructions []TypedInstruction // alias for Body (used by compiler/typeirgen)
	Locals       map[string]Typ     // local variables by name (used by typeirgen)
}

// ParamIR represents a function parameter.
type ParamIR struct {
	Name string
	Type Typ
}

// StructTypeDef represents a struct type definition.
type StructTypeDef struct {
	Name   string
	Fields []StructField
}

func readU32Be(data []byte, pos *int) (uint32, error) {
	if *pos+4 > len(data) {
		return 0, fmt.Errorf("readU32Be: unexpected end of data at pos %d", *pos)
	}
	v := binary.BigEndian.Uint32(data[*pos : *pos+4])
	*pos += 4
	return v, nil
}

func readStr(data []byte, pos *int) (string, error) {
	length, err := readU32Be(data, pos)
	if err != nil {
		return "", err
	}
	if *pos+int(length) > len(data) {
		return "", fmt.Errorf("readStr: string length %d exceeds data at pos %d", length, *pos)
	}
	s := string(data[*pos : *pos+int(length)])
	*pos += int(length)
	return s, nil
}

// DeserializeTypeModule deserializes a TypeIR binary module.
func DeserializeTypeModule(data []byte) (*TypeModule, error) {
	pos := 0
	if len(data) < 8 {
		return nil, fmt.Errorf("invalid TypeIR data: too short")
	}

	numFuncs, err := readU32Be(data, &pos)
	if err != nil {
		return nil, err
	}
	numLayouts, err := readU32Be(data, &pos)
	if err != nil {
		return nil, err
	}

	module := &TypeModule{
		Linkage: make(map[uint32]LinkageEntry),
	}

	for i := uint32(0); i < numLayouts; i++ {
		name, err := readStr(data, &pos)
		if err != nil {
			return nil, fmt.Errorf("struct name: %w", err)
		}
		numFields, err := readU32Be(data, &pos)
		if err != nil {
			return nil, err
		}
		fields := make([]StructField, numFields)
		for fi := uint32(0); fi < numFields; fi++ {
			fname, err := readStr(data, &pos)
			if err != nil {
				return nil, fmt.Errorf("struct field name: %w", err)
			}
			ftype, err := deserializeType(data, &pos)
			if err != nil {
				return nil, fmt.Errorf("struct field type: %w", err)
			}
			fields[fi] = StructField{Name: fname, FieldType: ftype}
		}
		module.StructTypes = append(module.StructTypes, StructTypeDef{Name: name, Fields: fields})
	}

	for i := uint32(0); i < numFuncs; i++ {
		name, err := readStr(data, &pos)
		if err != nil {
			return nil, fmt.Errorf("func name: %w", err)
		}
		id, err := readU32Be(data, &pos)
		if err != nil {
			return nil, err
		}
		paramCount, err := readU32Be(data, &pos)
		if err != nil {
			return nil, err
		}
		if pos >= len(data) {
			return nil, fmt.Errorf("unexpected end reading has_return")
		}
		hasReturn := data[pos] != 0
		pos++

		returnType, err := deserializeType(data, &pos)
		if err != nil {
			return nil, fmt.Errorf("return type: %w", err)
		}

		fn := FunctionIR{
			ID:         id,
			Name:       name,
			ParamCount: paramCount,
			HasReturn:  hasReturn,
			ReturnType: returnType,
			LocalTypes: make(map[uint32]Typ),
		}

		for pi := uint32(0); pi < paramCount; pi++ {
			pname, err := readStr(data, &pos)
			if err != nil {
				return nil, fmt.Errorf("param name: %w", err)
			}
			ptype, err := deserializeType(data, &pos)
			if err != nil {
				return nil, fmt.Errorf("param type: %w", err)
			}
			fn.Params = append(fn.Params, ParamIR{Name: pname, Type: ptype})
		}

		varCount, err := readU32Be(data, &pos)
		if err != nil {
			return nil, err
		}
		fn.VarCount = varCount

		numLocalTypes, err := readU32Be(data, &pos)
		if err != nil {
			return nil, err
		}
		for lti := uint32(0); lti < numLocalTypes; lti++ {
			vid, err := readU32Be(data, &pos)
			if err != nil {
				return nil, err
			}
			vty, err := deserializeType(data, &pos)
			if err != nil {
				return nil, fmt.Errorf("local type: %w", err)
			}
			fn.LocalTypes[vid] = vty
		}

		numInsts, err := readU32Be(data, &pos)
		if err != nil {
			return nil, err
		}
		fn.Body = make([]TypedInstruction, numInsts)
		for ii := uint32(0); ii < numInsts; ii++ {
			inst, err := deserializeInstruction(data, &pos)
			if err != nil {
				return nil, fmt.Errorf("instruction %d: %w", ii, err)
			}
			fn.Body[ii] = inst
		}

		module.Functions = append(module.Functions, fn)
	}

	// Linkage table (optional for backward compatibility)
	if pos < len(data) {
		numLinkages, err := readU32Be(data, &pos)
		if err == nil {
			for li := uint32(0); li < numLinkages; li++ {
				funcID, err := readU32Be(data, &pos)
				if err != nil {
					break
				}
				if pos >= len(data) {
					break
				}
				tag := data[pos]
				pos++
				if tag == 0 {
					module.Linkage[funcID] = LinkageEntry{IsExternal: false}
				} else if tag == 1 {
					extName, err := readStr(data, &pos)
					if err != nil {
						break
					}
					module.Linkage[funcID] = LinkageEntry{IsExternal: true, Name: extName}
				}
			}
		}
	}

	// Globals section (appended at the end by newer writers; absent in
	// older .vxobj files).
	if pos < len(data) {
		numGlobals, err := readU32Be(data, &pos)
		if err == nil {
			for gi := uint32(0); gi < numGlobals; gi++ {
				gname, err := readStr(data, &pos)
				if err != nil {
					break
				}
				gtype, err := deserializeType(data, &pos)
				if err != nil {
					break
				}
				module.Globals = append(module.Globals, GlobalIR{Name: gname, Type: gtype})
			}
		}
	}

	return module, nil
}

func deserializeType(data []byte, pos *int) (Typ, error) {
	if *pos >= len(data) {
		return nil, fmt.Errorf("unexpected end reading type tag")
	}
	tag := data[*pos]
	*pos++

	switch tag {
	case 0:
		return Void, nil
	case 1:
		return Int, nil
	case 2:
		return Float, nil
	case 3:
		return Bool, nil
	case 4:
		return String, nil
	case 5:
		name, err := readStr(data, pos)
		if err != nil {
			return nil, err
		}
		numFields, err := readU32Be(data, pos)
		if err != nil {
			return nil, err
		}
		fields := make([]StructField, numFields)
		for fi := uint32(0); fi < numFields; fi++ {
			fname, err := readStr(data, pos)
			if err != nil {
				return nil, err
			}
			ftype, err := deserializeType(data, pos)
			if err != nil {
				return nil, err
			}
			fields[fi] = StructField{Name: fname, FieldType: ftype}
		}
		return StructTyp{Name: name, Fields: fields}, nil
	case 6:
		inner, err := deserializeType(data, pos)
		if err != nil {
			return nil, err
		}
		return ArrayTyp{Elem: inner}, nil
	case 7:
		key, err := deserializeType(data, pos)
		if err != nil {
			return nil, err
		}
		value, err := deserializeType(data, pos)
		if err != nil {
			return nil, err
		}
		return MapTyp{Key: key, Value: value}, nil
	case 8:
		numParams, err := readU32Be(data, pos)
		if err != nil {
			return nil, err
		}
		params := make([]Typ, numParams)
		for pi := uint32(0); pi < numParams; pi++ {
			params[pi], err = deserializeType(data, pos)
			if err != nil {
				return nil, err
			}
		}
		retType, err := deserializeType(data, pos)
		if err != nil {
			return nil, err
		}
		return FuncTyp{Params: params, Return: retType}, nil
	case 9:
		inner, err := deserializeType(data, pos)
		if err != nil {
			return nil, err
		}
		return PointerTyp{To: inner}, nil
	case 10:
		name, err := readStr(data, pos)
		if err != nil {
			return nil, err
		}
		numArgs, err := readU32Be(data, pos)
		if err != nil {
			return nil, err
		}
		args := make([]Typ, numArgs)
		for ai := uint32(0); ai < numArgs; ai++ {
			args[ai], err = deserializeType(data, pos)
			if err != nil {
				return nil, err
			}
		}
		return GenericTyp{Name: name, Args: args}, nil
	case 11:
		return Unknown, nil
	case 255:
		return Unknown, nil
	default:
		return nil, fmt.Errorf("unknown type tag: %d", tag)
	}
}

func deserializeInstruction(data []byte, pos *int) (TypedInstruction, error) {
	if *pos >= len(data) {
		return TypedInstruction{}, fmt.Errorf("unexpected end reading instruction tag")
	}
	tag := data[*pos]
	*pos++

	// Tags that have no payload
	// Note: serialization always writes a 4-byte payload length,
	// so we must skip it even for no-payload instructions.
	switch tag {
	case 4:
		*pos += 4 // skip empty payload length
		return TypedInstruction{Op: ConstNil}, nil
	case 70:
		// Newz now carries struct name in payload.
		pl, err := readStr(data, pos)
		if err != nil {
			return TypedInstruction{Op: Newz, Args: nil}, nil
		}
		return TypedInstruction{Op: Newz, Args: []string{pl}}, nil
	case 80:
		*pos += 4 // skip empty payload length
		return TypedInstruction{Op: Dup}, nil
	case 81:
		*pos += 4 // skip empty payload length
		return TypedInstruction{Op: Pop}, nil
	case 82:
		*pos += 4 // skip empty payload length
		return TypedInstruction{Op: AddressOf}, nil
	case 85:
		*pos += 4 // skip empty payload length
		return TypedInstruction{Op: ScopeDrop}, nil
	case 93:
		*pos += 4 // skip empty payload length
		return TypedInstruction{Op: Halt}, nil
	}

	payload, err := readStr(data, pos)
	if err != nil {
		return TypedInstruction{}, fmt.Errorf("reading payload for tag %d: %w", tag, err)
	}

	switch tag {
	case 0:
		raw := strings.TrimPrefix(payload, "i")
		return TypedInstruction{Op: ConstInt, Args: []string{raw}}, nil
	case 1:
		raw := strings.TrimPrefix(payload, "f")
		return TypedInstruction{Op: ConstFloat, Args: []string{raw}}, nil
	case 2:
		raw := strings.TrimPrefix(payload, "b")
		return TypedInstruction{Op: ConstBool, Args: []string{raw}}, nil
	case 3:
		raw := strings.TrimPrefix(payload, "s")
		return TypedInstruction{Op: ConstString, Args: []string{raw}}, nil
	case 5:
		return TypedInstruction{Op: LoadVar, Args: []string{payload}}, nil
	case 6:
		return TypedInstruction{Op: StoreVar, Args: []string{payload}}, nil
	case 10:
		return TypedInstruction{Op: I32Add, Args: strings.Split(payload, ",")}, nil
	case 11:
		return TypedInstruction{Op: I32Sub, Args: strings.Split(payload, ",")}, nil
	case 12:
		return TypedInstruction{Op: I32Mul, Args: strings.Split(payload, ",")}, nil
	case 13:
		return TypedInstruction{Op: I32Div, Args: strings.Split(payload, ",")}, nil
	case 14:
		return TypedInstruction{Op: I32Mod, Args: strings.Split(payload, ",")}, nil
	case 15:
		return TypedInstruction{Op: F64Add, Args: strings.Split(payload, ",")}, nil
	case 16:
		return TypedInstruction{Op: F64Sub, Args: strings.Split(payload, ",")}, nil
	case 17:
		return TypedInstruction{Op: F64Mul, Args: strings.Split(payload, ",")}, nil
	case 18:
		return TypedInstruction{Op: F64Div, Args: strings.Split(payload, ",")}, nil
	case 19:
		return TypedInstruction{Op: I64Add, Args: strings.Split(payload, ",")}, nil
	case 20:
		return TypedInstruction{Op: I32Eq, Args: strings.Split(payload, ",")}, nil
	case 21:
		return TypedInstruction{Op: I32Neq, Args: strings.Split(payload, ",")}, nil
	case 22:
		return TypedInstruction{Op: I32Lt, Args: strings.Split(payload, ",")}, nil
	case 23:
		return TypedInstruction{Op: I32Gt, Args: strings.Split(payload, ",")}, nil
	case 24:
		return TypedInstruction{Op: I32Le, Args: strings.Split(payload, ",")}, nil
	case 25:
		return TypedInstruction{Op: I32Ge, Args: strings.Split(payload, ",")}, nil
	case 26:
		return TypedInstruction{Op: F64Eq, Args: strings.Split(payload, ",")}, nil
	case 27:
		return TypedInstruction{Op: F64Ne, Args: strings.Split(payload, ",")}, nil
	case 28:
		return TypedInstruction{Op: F64Lt, Args: strings.Split(payload, ",")}, nil
	case 29:
		return TypedInstruction{Op: F64Gt, Args: strings.Split(payload, ",")}, nil
	case 30:
		return TypedInstruction{Op: F64Le, Args: strings.Split(payload, ",")}, nil
	case 31:
		return TypedInstruction{Op: F64Ge, Args: strings.Split(payload, ",")}, nil
	case 32:
		return TypedInstruction{Op: I32Neg, Args: []string{payload}}, nil
	case 33:
		return TypedInstruction{Op: F64Neg, Args: []string{payload}}, nil
	case 34:
		return TypedInstruction{Op: BoolNot, Args: []string{payload}}, nil
	case 35:
		return TypedInstruction{Op: I32And, Args: strings.Split(payload, ",")}, nil
	case 36:
		return TypedInstruction{Op: I32Or, Args: strings.Split(payload, ",")}, nil
	case 37:
		return TypedInstruction{Op: BoolAnd, Args: strings.Split(payload, ",")}, nil
	case 38:
		return TypedInstruction{Op: BoolOr, Args: strings.Split(payload, ",")}, nil
	case 40:
		return TypedInstruction{Op: Jump, Args: []string{payload}}, nil
	case 41:
		return TypedInstruction{Op: JumpIfFalse, Args: strings.Split(payload, ",")}, nil
	case 42:
		return TypedInstruction{Op: JumpIfTrue, Args: strings.Split(payload, ",")}, nil
	case 43:
		return TypedInstruction{Op: StrEq, Args: strings.Split(payload, ",")}, nil
	case 44:
		return TypedInstruction{Op: NeqInt, Args: strings.Split(payload, ",")}, nil
	case 45:
		return TypedInstruction{Op: I64Sub, Args: strings.Split(payload, ",")}, nil
	case 46:
		return TypedInstruction{Op: I64Mul, Args: strings.Split(payload, ",")}, nil
	case 47:
		return TypedInstruction{Op: I64Div, Args: strings.Split(payload, ",")}, nil
	case 50:
		return deserializeCallInstr(payload), nil
	case 51:
		return deserializeCallIndirectInstr(payload), nil
	case 52:
		return TypedInstruction{Op: Return, Args: []string{payload}}, nil
	case 53:
		return TypedInstruction{Op: ReturnVoid}, nil
	case 60:
		return deserializeMakeStructInstr(payload), nil
	case 61:
		return TypedInstruction{Op: FieldLoad, Args: strings.Split(payload, ",")}, nil
	case 62:
		return TypedInstruction{Op: FieldStore, Args: strings.Split(payload, ",")}, nil
	case 63:
		return deserializeMakeArrayInstr(payload), nil
	case 64:
		return TypedInstruction{Op: IndexGet, Args: strings.Split(payload, ",")}, nil
	case 65:
		return TypedInstruction{Op: IndexSet, Args: strings.Split(payload, ",")}, nil
	case 66:
		return deserializeMakeMapInstr(payload), nil
	case 71:
		return TypedInstruction{Op: Free, Args: []string{payload}}, nil
	case 72:
		return TypedInstruction{Op: OwnershipMove, Args: []string{payload}}, nil
	case 73:
		return TypedInstruction{Op: BorrowCheck, Args: []string{payload}}, nil
	case 74:
		return TypedInstruction{Op: Deref, Args: []string{payload}}, nil
	case 75:
		return TypedInstruction{Op: AliveCheck, Args: []string{payload}}, nil
	case 83:
		return TypedInstruction{Op: PropertyGet, Args: []string{payload}}, nil
	case 84:
		return TypedInstruction{Op: PointerMember, Args: []string{payload}}, nil
	case 86:
		return TypedInstruction{Op: SysArgv, Args: []string{payload}}, nil
	case 87:
		return TypedInstruction{Op: System, Args: []string{payload}}, nil
	case 88:
		return TypedInstruction{Op: FileRead, Args: []string{payload}}, nil
	case 89:
		return TypedInstruction{Op: FileWrite, Args: []string{payload}}, nil
	case 90:
		return TypedInstruction{Op: FileExists, Args: []string{payload}}, nil
	case 91:
		return TypedInstruction{Op: Noop, Args: []string{payload}}, nil
	case 92:
		return TypedInstruction{Op: Import, Args: []string{payload}}, nil
	case 94:
		return TypedInstruction{Op: StrConcat, Args: []string{payload}}, nil
	case 95:
		return TypedInstruction{Op: ArrayConcat, Args: []string{payload}}, nil
	default:
		return TypedInstruction{}, fmt.Errorf("unknown instruction tag: %d", tag)
	}
}

func deserializeCallInstr(payload string) TypedInstruction {
	// The wire format is: f;<callee>,<paramType1>,<paramType2>,...
	// The ';' immediately follows the 'f' prefix so the callee is the
	// token between the first ';' and the first ',' (or end of string).
	if !strings.HasPrefix(payload, "f;") {
		// Malformed: missing callee marker.
		return TypedInstruction{Op: Call, Args: nil}
	}
	after := strings.TrimPrefix(payload, "f;")
	commaIdx := strings.Index(after, ",")
	var callee, rest string
	if commaIdx >= 0 {
		callee = after[:commaIdx]
		rest = after[commaIdx+1:]
	} else {
		callee = after
		rest = ""
	}
	// Deserialized args follow the canonical form: [";callee", paramType1, ...]
	var args []string
	args = append(args, ";"+callee)
	if rest != "" {
		args = append(args, strings.Split(rest, ",")...)
	}
	return TypedInstruction{Op: Call, Args: args}
}

func deserializeCallIndirectInstr(payload string) TypedInstruction {
	payload = strings.TrimPrefix(payload, "vi")
	parts := strings.Split(payload, ",")
	return TypedInstruction{Op: Call, Args: parts}
}

func deserializeMakeStructInstr(payload string) TypedInstruction {
	payload = strings.TrimPrefix(payload, "s")
	parts := strings.Split(payload, ",")
	return TypedInstruction{Op: StructNew, Args: parts}
}

func deserializeMakeArrayInstr(payload string) TypedInstruction {
	parts := []string{}
	if payload != "" {
		parts = strings.Split(payload, ",")
	}
	return TypedInstruction{Op: ArrayNew, Args: parts}
}

func deserializeMakeMapInstr(payload string) TypedInstruction {
	parts := []string{}
	if payload != "" {
		parts = strings.Split(payload, ",")
	}
	return TypedInstruction{Op: MapNew, Args: parts}
}

// TypeName returns the string representation of a type.
func TypeName(t Typ) string {
	return TypToString(t)
}

// ParseUint32 is a helper for parsing uint32 from string.
func ParseUint32(s string) uint32 {
	v, _ := strconv.ParseUint(s, 10, 32) //nolint:errcheck
	return uint32(v)
}
