; tree-sitter-vx 语法高亮查询
; VX 语言（adVanced eXecutable language）

; ---------- 关键字 ----------

[
  "if" "elif" "else" "while" "for" "in"
] @keyword.control

(break_statement) @keyword.control
(continue_statement) @keyword.control

"func" @keyword.function

"return" @keyword.return

[
  "import" "as" "export" "extern"
] @keyword.import

[
  "class" "struct"
] @keyword.type

"enum" @keyword.type

"macro" @keyword.function

"new" @keyword.operator

[
  "and" "or" "not"
] @keyword.operator

[
  "var" "mut" "const"
] @keyword.modifier

; ---------- 类型 ----------

(primitive_type) @type.builtin
(generic_type name: (identifier) @type)
(generic_type name: (primitive_type) @type.builtin)

; ---------- 标识符 ----------

(function_definition name: (identifier) @function)
(function_definition name: (soft_keyword) @function)
(function_definition name: "new" @constructor)

(call
  function: [(identifier) (soft_keyword)] @function.call)

(call
  function: (attribute
    property: [(identifier) (soft_keyword)] @method.call))

(attribute property: (identifier) @property)

(parameter name: (identifier) @variable.parameter)

; 类 / 结构体名
(class_definition name: (identifier) @type)
(struct_definition name: (identifier) @type)
(new_expression class: (identifier) @type)

(import_statement path: (dotted_name) @module)
(export_statement name: (identifier) @variable.public)

(field_declaration name: (identifier) @property)

; 枚举成员与类常量
(enum_member name: (identifier) @constant)
(constant_member name: (identifier) @constant)

; 宏定义与调用
(macro_definition name: (identifier) @function.macro)

; ---------- 字面量 ----------

(integer) @number
(float) @number.float

(string) @string
(escape_sequence) @string.escape

(boolean) @constant.builtin
(nil) @constant.builtin

(comment) @comment

; ---------- 运算符与标点 ----------

[
  "+" "-" "*" "/" "%" "^"
  "=" "+=" "-=" "*=" "/="
  "==" "!=" "<" ">" "<=" ">="
  "&&" "||" "!"
] @operator

[
  "(" ")" "[" "]" "{" "}"
] @punctuation.bracket

[
  "." "," ":" ";" "->"
] @punctuation.delimiter
