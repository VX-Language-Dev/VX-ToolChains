/**
 * tree-sitter-vx —— VX 语言（adVanced eXecutable language）的 Tree-Sitter 语法
 *
 * 语法规则依据 vx-toolkit/src/compiler/parser.cpp（参考实现）与
 * compiler/parser.vx（自举实现）制定：
 *
 *  - 缩进块（Python 风格 INDENT/DEDENT，由外部扫描器 src/scanner.c 产出）
 *  - `#` 行注释；块头（func/if/while/...）后必须有 `:`
 *  - 块内语句可写在 `:` 同一行，用 `;` 分隔
 *  - 关键字若作表达式位置使用则回退为标识符（软关键字：
 *    loop/match/enum/union/macro/move；`new` 可作方法/成员名）
 *  - 类型标注支持泛型形式 `Vec<int>`；`var` 是内建类型之一
 */

const PREC = {
  OR: 1,       // or ||
  AND: 2,      // and &&
  EQUALS: 3,   // == !=
  COMPARE: 4,  // < > <= >=
  PLUS: 5,     // + -
  TIMES: 6,    // * / %
  POWER: 7,    // ^
  UNARY: 8,    // - not !
  CALL: 9,     // f(...), new X(...)
  SUBSCRIPT: 10, // a[i]
  MEMBER: 11,  // a.b
};

module.exports = grammar({
  name: 'vx',

  word: $ => $.identifier,

  extras: $ => [
    /[\s\uFEFF\u2060\u200B]/, // 全部空白（含换行）；
                              // 换行符在语句间由 extras 消费，
                              // 语句边界处的换行由外部扫描器产出的 _newline 表达
    $.comment,
  ],

  externals: $ => [
    $._newline,
    $._indent,
    $._dedent,
  ],

  supertypes: $ => [
    $._expression,
    $._type,
  ],

  rules: {
    // ============================================================
    // 程序结构
    // ============================================================

    source_file: $ => repeat($._statement),

    _statement: $ => choice(
      $._simple_statements,
      $._compound_statement,
    ),

    // 同一行的多条简单语句：a(); b() 或 a(); b() 换行
    _simple_statements: $ => seq(
      $._simple_statement,
      repeat(seq(';', $._simple_statement)),
      optional(';'),
      $._newline,
    ),

    _simple_statement: $ => choice(
      $.import_statement,
      $.export_statement,
      $.variable_declaration,
      $.annotated_assignment,
      $.extern_declaration,
      $.return_statement,
      $.break_statement,
      $.continue_statement,
      $.expression_statement,
    ),

    _compound_statement: $ => choice(
      $.function_definition,
      $.class_definition,
      $.struct_definition,
      $.enum_definition,
      $.macro_definition,
      $.if_statement,
      $.while_statement,
      $.for_statement,
    ),

    // 块：缩进块 / `:` 同行语句 / 空块
    block: $ => choice(
      seq($._indent, repeat($._statement), $._dedent),
      $._simple_statements,
      $._newline,
    ),

    // ============================================================
    // 简单语句
    // ============================================================

    import_statement: $ => seq(
      'import',
      field('path', $.dotted_name),
      optional(seq('as', field('alias', $._name))),
    ),

    dotted_name: $ => seq($._name, repeat(seq('.', $._name))),

    export_statement: $ => seq('export', field('name', $._name)),

    variable_declaration: $ => seq(
      field('modifier', choice('var', 'mut', 'const')),
      field('name', $._name),
      optional(seq(':', field('type', $._type))),
      optional(seq('=', field('value', $._expression))),
    ),

    // extern [func] name(params) [-> type]
    extern_declaration: $ => seq(
      'extern',
      optional('func'),
      field('name', $._name),
      field('parameters', $.parameters),
      optional(seq('->', field('return_type', $._type))),
    ),

    // prec.right：macro 花括号体内 `return (expr)` 优先将表达式并入返回值
    return_statement: $ => prec.right(seq('return', optional(field('value', $._expression)))),

    break_statement: $ => 'break',

    continue_statement: $ => 'continue',

    expression_statement: $ => choice(
      $.assignment,
      $._expression,
    ),

    assignment: $ => seq(
      field('left', $._expression),
      field('operator', choice('=', '+=', '-=', '*=', '/=')),
      field('right', $._expression),
    ),

    // 带类型标注的赋值：i: int = 0 / bytes: Vec<int> = other.to_vec()
    annotated_assignment: $ => seq(
      field('left', $._name),
      ':',
      field('type', $._type),
      '=',
      field('right', $._expression),
    ),

    // ============================================================
    // 复合语句
    // ============================================================

    function_definition: $ => seq(
      'func',
      field('name', $._name),
      field('parameters', $.parameters),
      optional(seq('->', field('return_type', $._type))),
      ':',
      $.block,
    ),

    parameters: $ => seq(
      '(',
      optional(seq(commaSep1($.parameter), optional(','))),
      ')',
    ),

    parameter: $ => seq(
      field('name', $._name),
      optional(seq(':', field('type', $._type))),
    ),

    // class Name:
    //     field: type
    //     func method(self): ...
    class_definition: $ => seq(
      'class',
      field('name', $._name),
      ':',
      $.class_body,
    ),

    class_body: $ => choice(
      seq(
        $._indent,
        repeat(choice(
          $.field_declaration,
          $.constant_member,
          $.function_definition,
        )),
        $._dedent,
      ),
      $._newline,
    ),

    // 类常量成员：NIL = "Nil"
    constant_member: $ => seq(
      field('name', $._name),
      '=',
      field('value', $._expression),
      $._newline,
    ),

    // struct Name:
    //     field: type
    struct_definition: $ => seq(
      'struct',
      field('name', $._name),
      ':',
      $.struct_body,
    ),

    struct_body: $ => choice(
      seq($._indent, repeat($.field_declaration), $._dedent),
      $._newline,
    ),

    field_declaration: $ => seq(
      field('name', $._name),
      ':',
      field('type', $._type),
      $._newline,
    ),

    // enum Name:
    //     Member = value
    enum_definition: $ => seq(
      'enum',
      field('name', $._name),
      ':',
      $.enum_body,
    ),

    enum_body: $ => choice(
      seq($._indent, repeat($.enum_member), $._dedent),
      $._newline,
    ),

    enum_member: $ => seq(
      field('name', $._name),
      '=',
      field('value', $._expression),
      $._newline,
    ),

    // macro name(params) { ... } —— 花括号块（编译时宏）
    macro_definition: $ => seq(
      'macro',
      field('name', $._name),
      field('parameters', $.parameters),
      $.macro_body,
    ),

    macro_body: $ => seq('{', repeat($._macro_statement), '}'),

    macro_block: $ => seq('{', repeat($._macro_statement), '}'),

    _macro_statement: $ => choice(
      $.variable_declaration,
      $.annotated_assignment,
      $.return_statement,
      $.expression_statement,
      $.macro_if_statement,
      $.macro_while_statement,
      $.macro_for_statement,
    ),

    macro_if_statement: $ => seq(
      'if',
      field('condition', $._expression),
      $.macro_block,
      repeat($.macro_elif_clause),
      optional($.macro_else_clause),
    ),

    macro_elif_clause: $ => seq(
      'elif',
      field('condition', $._expression),
      $.macro_block,
    ),

    macro_else_clause: $ => seq('else', $.macro_block),

    macro_while_statement: $ => seq(
      'while',
      field('condition', $._expression),
      $.macro_block,
    ),

    macro_for_statement: $ => seq(
      'for',
      field('name', $._name),
      'in',
      field('iterable', $._expression),
      $.macro_block,
    ),

    if_statement: $ => seq(
      'if',
      field('condition', $._expression),
      ':',
      $.block,
      repeat($.elif_clause),
      optional($.else_clause),
    ),

    elif_clause: $ => seq(
      'elif',
      field('condition', $._expression),
      ':',
      $.block,
    ),

    else_clause: $ => seq('else', ':', $.block),

    while_statement: $ => seq(
      'while',
      field('condition', $._expression),
      ':',
      $.block,
    ),

    for_statement: $ => seq(
      'for',
      field('name', $._name),
      'in',
      field('iterable', $._expression),
      ':',
      $.block,
    ),

    // ============================================================
    // 表达式
    // ============================================================

    _expression: $ => choice(
      $.binary_expression,
      $.unary_expression,
      $._primary_expression,
    ),

    binary_expression: $ => choice(
      prec.left(PREC.OR, seq(
        field('left', $._expression),
        field('operator', choice('or', '||')),
        field('right', $._expression),
      )),
      prec.left(PREC.AND, seq(
        field('left', $._expression),
        field('operator', choice('and', '&&')),
        field('right', $._expression),
      )),
      prec.left(PREC.EQUALS, seq(
        field('left', $._expression),
        field('operator', choice('==', '!=')),
        field('right', $._expression),
      )),
      prec.left(PREC.COMPARE, seq(
        field('left', $._expression),
        field('operator', choice('<', '>', '<=', '>=')),
        field('right', $._expression),
      )),
      prec.left(PREC.PLUS, seq(
        field('left', $._expression),
        field('operator', choice('+', '-')),
        field('right', $._expression),
      )),
      prec.left(PREC.TIMES, seq(
        field('left', $._expression),
        field('operator', choice('*', '/', '%')),
        field('right', $._expression),
      )),
      prec.left(PREC.POWER, seq(
        field('left', $._expression),
        field('operator', '^'),
        field('right', $._expression),
      )),
    ),

    unary_expression: $ => prec(PREC.UNARY, seq(
      field('operator', choice('-', 'not', '!')),
      field('argument', $._expression),
    )),

    _primary_expression: $ => choice(
      $.identifier,
      $.soft_keyword,
      $.string,
      $.integer,
      $.float,
      $.boolean,
      $.nil,
      $.list,
      $.dictionary,
      $.parenthesized_expression,
      $.call,
      $.attribute,
      $.subscript,
      $.new_expression,
    ),

    call: $ => prec(PREC.CALL, seq(
      field('function', $._primary_expression),
      field('arguments', $.argument_list),
    )),

    attribute: $ => prec(PREC.MEMBER, seq(
      field('object', $._primary_expression),
      '.',
      field('property', $._name),
    )),

    subscript: $ => prec(PREC.SUBSCRIPT, seq(
      field('object', $._primary_expression),
      '[',
      field('index', $._expression),
      ']',
    )),

    // new ClassName(args) / new Vec<int>()
    // prec.right：出现 '(' 时优先将其并入 argument_list 而非提前完成 new 表达式
    new_expression: $ => prec.right(PREC.CALL, seq(
      'new',
      field('class', $._type),
      optional(field('arguments', $.argument_list)),
    )),

    argument_list: $ => seq(
      '(',
      optional(seq(commaSep1($._expression), optional(','))),
      ')',
    ),

    list: $ => seq(
      '[',
      optional(seq(commaSep1($._expression), optional(','))),
      ']',
    ),

    dictionary: $ => seq(
      '{',
      optional(seq(commaSep1($.pair), optional(','))),
      '}',
    ),

    pair: $ => seq(
      field('key', $._expression),
      ':',
      field('value', $._expression),
    ),

    parenthesized_expression: $ => seq('(', $._expression, ')'),

    // ============================================================
    // 类型
    // ============================================================

    _type: $ => choice(
      $.identifier,
      $.primitive_type,
      $.generic_type,
      $.qualified_type,
    ),

    primitive_type: $ => choice('int', 'float', 'bool', 'void', 'var'),

    // 点分类型：vec.Vec / error.VXError
    // prec(1)：`new Foo.bar` 中 '.' 优先并入类型而非作为 new 表达式的属性
    qualified_type: $ => prec(1, seq($.identifier, repeat1(seq('.', $.identifier)))),

    // prec(1)：`new Vec<int>` 中 '<' 优先并入泛型参数而非作为比较运算符
    generic_type: $ => prec(1, seq(
      field('name', choice($.identifier, $.primitive_type)),
      '<',
      commaSep1(choice($._type, $.integer)),
      '>',
    )),

    // ============================================================
    // 词法单元
    // ============================================================

    // 软关键字：表达式位置回退为标识符（与参考实现 is_soft_kw 一致）
    soft_keyword: $ => choice('loop', 'match', 'enum', 'union', 'macro', 'move'),

    // 名称位置（函数名/参数名/成员名/导入别名）：除标识符与软关键字外，
    // 还允许 `new`（参考实现 parse_name 接受任意关键字，此处取实际用到的集合）
    _name: $ => choice(
      $.identifier,
      $.soft_keyword,
      'new',
    ),

    identifier: $ => /[_\p{L}][_\p{L}\p{N}]*/u,

    // 支持单双引号、\n \t \\ \' \" 转义，允许跨行（与自举实现一致）
    string: $ => choice(
      seq('"', repeat(choice($.escape_sequence, /[^\\"]/)), '"'),
      seq("'", repeat(choice($.escape_sequence, /[^\\']/)), "'"),
    ),

    escape_sequence: $ => token(/\\./),

    integer: $ => choice(/0[xX][0-9a-fA-F]+/, /\d+/),

    float: $ => /\d+\.\d+/,

    boolean: $ => choice('true', 'false'),

    nil: $ => 'nil',

    comment: $ => token(prec(-1, /#[^\r\n]*/)),
  },
});

function commaSep1(rule) {
  return seq(rule, repeat(seq(',', rule)));
}
