module.exports = grammar({
  name: 'vmake',

  extras: $ => [/\s/, /#.*/],

  rules: {
    source_file: $ => repeat($._statement),

    _statement: $ => choice(
      $.target_call,
    ),

    target_call: $ => seq(
      field('kind', choice('exe', 'dylib', 'staticlib')),
      '(',
      optional(seq(
        field('name', $.string),
        optional(',')
      )),
      optional(seq(
        $.keyword_args
      )),
      ')'
    ),

    keyword_args: $ => seq(
      $.keyword_arg,
      repeat(seq(',', $.keyword_arg)),
      optional(',')
    ),

    keyword_arg: $ => seq(
      field('key', $.identifier),
      '=',
      field('value', choice($.string, $.string_array))
    ),

    string_array: $ => seq(
      '[',
      optional(seq(
        $.string,
        repeat(seq(',', $.string)),
        optional(',')
      )),
      ']'
    ),

    string: $ => choice(
      seq('"', repeat(choice(/[^"\\]/, /\\./)), '"'),
      seq("'", repeat(choice(/[^'\\]/, /\\./)), "'")
    ),

    identifier: $ => /[A-Za-z_][A-Za-z0-9_-]*/
  }
});