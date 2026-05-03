module.exports = grammar({
  name: 'org',

  externals: $ => [
    // Order MUST match grammar/src/scanner.c's `enum OrgExternal`.
    $._heading_open,
    $._heading_close,
    $._planning_line,
    $._propdrawer_open,
    $._propdrawer_close,
    $._node_property_line,
    $._drawer_open,
    $._drawer_close,
    $._gblock_open,
    $._gblock_close,
    $._src_block_open,
    $._src_block_close,
    $._example_block_open,
    $._example_block_close,
    $._export_block_open,
    $._export_block_close,
    $._verse_block_open,
    $._verse_block_close,
    $._comment_block_open,
    $._comment_block_close,
    $._lblock_body,
    $._dynblock_open,
    $._dynblock_close,
    $._latexenv_open,
    $._latexenv_body,
    $._latexenv_close,
    $._keyword_line,
    $._affiliated_keyword_line,
    $._comment_line,
    $._fixed_width_line,
    $._hrule_line,
    $._table_row_start,
    $._table_pipe,
    $._table_cell_content,
    $._table_row_end,
    $._table_rule_line,
    $._list_item_bullet,
    $._plain_list_open,
    $._plain_list_close,
    $._footnote_def_line,
    $._inlinetask_open,
    $._inlinetask_close,
    $._clock_line,
    $._diary_sexp_line,
    $._inline_content_line,
    $._empty_line,
  ],

  extras: _ => [],

  conflicts: $ => [
    /* Empty lines between heading_open boundaries can belong to either
     * the inner headline's tail (`repeat(_empty_line, headline)`) or the
     * outer document/headline's repeat.  GLR explores both; precedence
     * via prec.right(headline) keeps them inside the inner headline. */
    [$.headline],
  ],

  rules: {
    document: $ => seq(
      optional($.zeroth_section),
      repeat(choice($._empty_line, $.headline)),
    ),

    zeroth_section: $ => prec.right(seq(
      $._meaningful_content_line,
      repeat($._content_line),
    )),

    headline: $ => prec.right(seq(
      $._heading_open,
      repeat($._empty_line),
      optional($.section),
      repeat(choice($._empty_line, $.headline)),
      $._heading_close,
    )),

    planning: $ => prec.right(repeat1($._planning_line)),

    property_drawer: $ => seq(
      $._propdrawer_open,
      repeat($.node_property),
      $._propdrawer_close,
    ),
    node_property: $ => $._node_property_line,

    section: $ => prec.right(seq(
      $._meaningful_content_line,
      repeat($._content_line),
    )),

    // Body lines that may appear inside any container *except* a
    // footnote_definition.  Footnote definitions terminate at the next
    // `[fn:LABEL]` line per Emacs, so footnote_definition can't recurse
    // into its own body — see `_footnote_body_content_line` below.
    _non_fn_meaningful_content_line: $ => choice(
      $.planning,
      $.property_drawer,
      $.drawer,
      $.greater_block,
      $.dynamic_block,
      $.src_block,
      $.example_block,
      $.export_block,
      $.verse_block,
      $.comment_block,
      $.latex_environment,
      $.list,
      $.table,
      $.inlinetask,
      $.clock,
      $.keyword,
      $.affiliated_keyword,
      $.comment,
      $.fixed_width,
      $.horizontal_rule,
      $.diary_sexp,
      $.paragraph,
    ),

    _meaningful_content_line: $ => choice(
      $._non_fn_meaningful_content_line,
      $.footnote_definition,
    ),

    _content_line: $ => choice(
      $._meaningful_content_line,
      $._empty_line,
    ),

    _footnote_body_content_line: $ => choice(
      $._non_fn_meaningful_content_line,
      $._empty_line,
    ),

    drawer: $ => seq($._drawer_open, repeat($._content_line), $._drawer_close),
    greater_block: $ => seq($._gblock_open, repeat($._content_line), $._gblock_close),
    dynamic_block: $ => seq($._dynblock_open, repeat($._content_line), $._dynblock_close),
    src_block:     $ => seq($._src_block_open,     repeat($._lblock_body), $._src_block_close),
    example_block: $ => seq($._example_block_open, repeat($._lblock_body), $._example_block_close),
    export_block:  $ => seq($._export_block_open,  repeat($._lblock_body), $._export_block_close),
    verse_block:   $ => seq($._verse_block_open,   repeat($._lblock_body), $._verse_block_close),
    comment_block: $ => seq($._comment_block_open, repeat($._lblock_body), $._comment_block_close),
    latex_environment: $ => seq($._latexenv_open, repeat($._latexenv_body), $._latexenv_close),

    list: $ => prec.right(seq(
      $._plain_list_open,
      repeat1($.list_item),
      $._plain_list_close,
    )),
    list_item: $ => prec.right(seq(
      $._list_item_bullet,
      optional($.paragraph),
      repeat($.list),
    )),

    table: $ => prec.right(repeat1(choice(
      $.table_row,
      $.table_rule,
    ))),
    table_row: $ => seq(
      $._table_row_start,
      repeat($.table_cell),
      $._table_row_end,
    ),
    table_cell: $ => seq(
      optional($._table_cell_content),
      $._table_pipe,
    ),
    table_rule: $ => $._table_rule_line,

    footnote_definition: $ => prec.right(seq($._footnote_def_line, repeat($._footnote_body_content_line))),
    inlinetask: $ => seq($._inlinetask_open, repeat($._content_line), $._inlinetask_close),

    clock: $ => $._clock_line,
    keyword: $ => $._keyword_line,
    affiliated_keyword: $ => $._affiliated_keyword_line,
    comment: $ => prec.right(repeat1($._comment_line)),
    fixed_width: $ => prec.right(repeat1($._fixed_width_line)),
    horizontal_rule: $ => $._hrule_line,
    diary_sexp: $ => $._diary_sexp_line,
    paragraph: $ => prec.right(repeat1($._inline_content_line)),
  },
});
