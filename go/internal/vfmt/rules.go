package vfmt

// RuleSet defines common API migration rule collections.
// Rules operate at the AST level without type resolution, so they match
// by name only (not by type). For stricter type-based matching, combine
// this tool with semantic analysis or use targeted rules per file.
//
// Rule Kinds:
//   - "rename_func":       Rename a function call `oldName(args)` -> `newName(args)` or `newModule.newName(args)`
//   - "rename_method":     Rename a method call `obj.oldMethod(args)` -> `obj.newMethod(args)`
//   - "migrate_call":      Full call migration with module moves and arg transformation
//   - "migrate_property":  Rename a property access `obj.oldProp` -> `obj.newProp`
//   - "migrate_import":    Change import paths
//   - "migrate_type":      Rename type references

// CommonMigrationRules returns common migration rules for new API migration.
func CommonMigrationRules() []Rule {
	return []Rule{
		// ========== I/O API Migration (module-level) ==========
		{
			Description: "io.out() -> sys.print()",
			Kind:        "rename_func",
			OldName:     "out",
			NewName:     "print",
			NewModule:   "sys",
		},
		{
			Description: "io.println() -> sys.println()",
			Kind:        "rename_func",
			OldName:     "println",
			NewName:     "println",
			NewModule:   "sys",
		},
		{
			Description: "io.readln() -> sys.readln()",
			Kind:        "rename_func",
			OldName:     "readln",
			NewName:     "readln",
			NewModule:   "sys",
		},
		{
			Description: "io.input() -> sys.input()",
			Kind:        "rename_func",
			OldName:     "input",
			NewName:     "input",
			NewModule:   "sys",
		},

		// ========== Method renames (without type filtering) ==========
		{
			Description: "push() -> append()",
			Kind:        "rename_method",
			OldName:     "push",
			NewName:     "append",
		},
		{
			Description: "pop() -> remove_last()",
			Kind:        "rename_method",
			OldName:     "pop",
			NewName:     "remove_last",
		},
		{
			Description: "to_array() -> to_vec()",
			Kind:        "rename_method",
			OldName:     "to_array",
			NewName:     "to_vec",
		},

		// ========== Option API Migration ==========
		{
			Description: "some() -> Some()",
			Kind:        "rename_method",
			OldName:     "some",
			NewName:     "Some",
		},
		{
			Description: "none() -> None()",
			Kind:        "rename_method",
			OldName:     "none",
			NewName:     "None",
		},
		{
			Description: "is_some() -> isSome()",
			Kind:        "rename_method",
			OldName:     "is_some",
			NewName:     "isSome",
		},
		{
			Description: "is_none() -> isNone()",
			Kind:        "rename_method",
			OldName:     "is_none",
			NewName:     "isNone",
		},
		{
			Description: "unwrap_or() -> unwrapOr()",
			Kind:        "rename_method",
			OldName:     "unwrap_or",
			NewName:     "unwrapOr",
		},
		{
			Description: "if_some() -> ifSome()",
			Kind:        "rename_method",
			OldName:     "if_some",
			NewName:     "ifSome",
		},
		{
			Description: "to_string() -> toString()",
			Kind:        "rename_method",
			OldName:     "to_string",
			NewName:     "toString",
		},

		// ========== General naming convention migration ==========
		{
			Description: "get_value -> getValue",
			Kind:        "rename_method",
			OldName:     "get_value",
			NewName:     "getValue",
		},
		{
			Description: "set_value -> setValue",
			Kind:        "rename_method",
			OldName:     "set_value",
			NewName:     "setValue",
		},
		{
			Description: "from_string -> fromString",
			Kind:        "rename_method",
			OldName:     "from_string",
			NewName:     "fromString",
		},
		{
			Description: "from_int -> fromInt",
			Kind:        "rename_method",
			OldName:     "from_int",
			NewName:     "fromInt",
		},

		// ========== Import Path Migration ==========
		{
			Description: "old_module -> new_module",
			Kind:        "migrate_import",
			OldName:     "old_module",
			NewName:     "new_module",
		},

		// ========== Type Name Migration ==========
		{
			Description: "String -> str",
			Kind:        "migrate_type",
			OldName:     "String",
			NewName:     "str",
		},
		{
			Description: "Integer -> int",
			Kind:        "migrate_type",
			OldName:     "Integer",
			NewName:     "int",
		},
		{
			Description: "Boolean -> bool",
			Kind:        "migrate_type",
			OldName:     "Boolean",
			NewName:     "bool",
		},
	}
}
