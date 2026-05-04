module.exports = grammar({
  name: 'org',

  externals: $ => [
    // Order MUST match grammar/src/scanner.c's `enum OrgExternal`.
    $._heading_open,
    $._heading_close,
    $._headline_todo,     // uppercase TODO keyword
    $._headline_comment,  // literal "COMMENT" keyword
    $._headline_priority, // `[#A]` cookie
    $._headline_title,    // text from current pos to before tag_list / EOL
    $._headline_stats_cookie, // `[N%]` or `[N/M]` between title and tags
    $._headline_tag_list_open, // zero-width validator at tag-list start
    $._list_checkbox,          // `[ ]` / `[x]` / `[X]` / `[-]` after bullet
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
      $.headline_line,
      repeat($._empty_line),
      optional($.section),
      repeat(choice($._empty_line, $.headline)),
      $._heading_close,
    )),

    /* The heading line itself, decomposed into named nodes:
     *
     *   * TODO [#A] Title text :tag1:tag2:\n
     *   ^^^^                                 stars (from external scanner)
     *        ^^^^^                           todo (regex; config-aware
     *                                              classification done by
     *                                              consumers)
     *              ^^^^                      priority
     *                   ^^^^^^^^^^           title
     *                              ^^^^^^^^^^ tag_list (with tag children)
     *                                       ^ newline
     *
     * `stars` aliases the `_heading_open` external token (which covers
     * `*+`). The remainder of the line is parsed by JS rules below.
     */
    headline_line: $ => seq(
      field('stars',     alias($._heading_open, $.stars)),
      /[ \t]+/,
      optional(seq(field('todo',     $.todo),     /[ \t]+/)),
      optional(seq(field('comment',  $.comment_marker), /[ \t]+/)),
      optional(seq(field('priority', $.priority), /[ \t]+/)),
      optional(field('title',    $.title)),
      optional(field('cookie',   $.statistics_cookie)),
      optional(field('tag_list', $.tag_list)),
    ),

    /* TODO keyword. External token: uppercase word (>= 2 chars,
     * letters/digits/_/-) followed by whitespace. Consumers classify
     * against the configured `org-todo-keywords` sequence. */
    todo: $ => $._headline_todo,

    /* `COMMENT` keyword: marks the heading as a "commented" subtree
     * (excluded from agenda / export per Emacs `org-element-comment-p`).
     * Always literal "COMMENT" + ws. */
    comment_marker: $ => $._headline_comment,

    /* Priority cookie `[#A]` / `[#B]` / `[#1]`. */
    priority: $ => $._headline_priority,

    /* Statistics cookie at the end of a headline: `[N%]` or `[N/M]`.
     * Per Emacs convention the cookie is a per-headline progress
     * indicator (typically auto-updated from child checkboxes). */
    statistics_cookie: $ => $._headline_stats_cookie,

    /* Headline title. Single external token: the scanner peeks from
     * current position to find either end-of-line OR the start of a
     * trailing tag block, then emits everything before that. Trailing
     * whitespace before tags is included. */
    title: $ => $._headline_title,

    /* Trailing tag block. Validation handled by external token
     * `_headline_tag_list_open` (zero-width) — fires only at a real
     * `:tag1:tag2:[ws]*\n` position. JS rules then consume the
     * `:` separators and `tag` names with regex tokens, exposing
     * each tag as a named child. */
    tag_list: $ => seq(
      $._headline_tag_list_open,
      ':',
      repeat1(seq(field('tag', $.tag), ':')),
      /[ \t]*/,
    ),

    tag: $ => /[A-Za-z0-9_@#%]+/,

    /* Planning section beneath a heading. Each line carries 1+
     * SCHEDULED / DEADLINE / CLOSED entries (Emacs allows multiple
     * keywords on one line). The scanner emits a zero-width
     * `_planning_line` at the start of each such line so JS rules
     * can consume every keyword + timestamp pair on it. */
    planning: $ => prec.right(repeat1($.planning_line)),

    planning_line: $ => seq(
      $._planning_line,
      repeat1($.planning_entry),
      /[ \t]*\r?\n/,
    ),

    planning_entry: $ => seq(
      /[ \t]*/,
      field('keyword',   $.planning_keyword),
      ':',
      /[ \t]+/,
      field('timestamp', $.planning_timestamp),
    ),

    planning_keyword:   $ => choice('SCHEDULED', 'DEADLINE', 'CLOSED'),
    planning_timestamp: $ => /[<\[][^\n>\]]+[>\]]/,

    property_drawer: $ => seq(
      $._propdrawer_open,
      'PROPERTIES',
      ':',
      /[ \t]*\r?\n/,
      repeat($.node_property),
      $._propdrawer_close,
      'END',
      ':',
      /[ \t]*\r?\n/,
    ),
    /* Property line inside a property_drawer: `:KEY: value`. Scanner
     * emits `_node_property_line` covering only the leading `:` so
     * JS rules expose `name` and optional `value` as named children. */
    node_property: $ => seq(
      $._node_property_line,
      field('name', $.property_name),
      ':',
      optional(seq(/[ \t]+/, field('value', $.property_value))),
      /[ \t]*\r?\n/,
    ),

    property_name:  $ => /[A-Za-z_][A-Za-z0-9_+-]*/,
    property_value: $ => /[^\n]+/,

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

    /* Custom-named drawer `:NAME: ... :END:`. Scanner emits both
     * `_drawer_open` and `_drawer_close` covering only the leading
     * `:` so JS rules expose the name as a child field. */
    drawer: $ => seq(
      $._drawer_open,
      field('name', $.drawer_name),
      ':',
      /[ \t]*\r?\n/,
      repeat($._content_line),
      $._drawer_close,
      'END',
      ':',
      /[ \t]*\r?\n/,
    ),

    drawer_name: $ => /[A-Za-z_][A-Za-z0-9_-]*/,
    greater_block: $ => seq($._gblock_open, repeat($._content_line), $._gblock_close),
    dynamic_block: $ => seq($._dynblock_open, repeat($._content_line), $._dynblock_close),

    /* Lesser blocks. The C scanner emits `_*_block_open` covering only
     * the directive prefix (`#+begin_src` / `#+begin_example` / …),
     * leaving the language identifier + header arguments + newline to
     * be parsed as JS rules. That makes `language`, `header_args` real
     * named children (a Babel-aware consumer can read them directly). */
    src_block: $ => seq(
      $._src_block_open,
      optional(seq(/[ \t]+/, field('language',    $.src_block_language))),
      optional(seq(/[ \t]+/, field('header_args', $.block_header_args))),
      /[ \t]*\r?\n/,
      repeat($._lblock_body),
      $._src_block_close,
    ),
    example_block: $ => seq(
      $._example_block_open,
      optional(seq(/[ \t]+/, field('header_args', $.block_header_args))),
      /[ \t]*\r?\n/,
      repeat($._lblock_body),
      $._example_block_close,
    ),
    export_block: $ => seq(
      $._export_block_open,
      optional(seq(/[ \t]+/, field('format',      $.src_block_language))),
      optional(seq(/[ \t]+/, field('header_args', $.block_header_args))),
      /[ \t]*\r?\n/,
      repeat($._lblock_body),
      $._export_block_close,
    ),
    verse_block: $ => seq(
      $._verse_block_open,
      optional(seq(/[ \t]+/, field('header_args', $.block_header_args))),
      /[ \t]*\r?\n/,
      repeat($._lblock_body),
      $._verse_block_close,
    ),
    comment_block: $ => seq(
      $._comment_block_open,
      /[ \t]*\r?\n/,
      repeat($._lblock_body),
      $._comment_block_close,
    ),

    /* Source-block language identifier (`lua`, `python`, `org`, …). */
    src_block_language: $ => /[A-Za-z][A-Za-z0-9_+-]*/,

    /* Babel-style header arguments: `:key value :key2 v2 …`. Captured
     * as one node; consumers can split on `:` for individual pairs. */
    block_header_args: $ => /:[^\n]*/,
    latex_environment: $ => seq($._latexenv_open, repeat($._latexenv_body), $._latexenv_close),

    list: $ => prec.right(seq(
      $._plain_list_open,
      repeat1($.list_item),
      $._plain_list_close,
    )),
    list_item: $ => prec.right(seq(
      field('bullet', alias($._list_item_bullet, $.bullet)),
      optional(field('checkbox', $.checkbox)),
      optional($.paragraph),
      repeat($.list),
    )),

    /* Checkbox glyph at start of a list item: `[ ]` / `[x]` / `[X]`
     * / `[-]`. The `[X]` form is uppercase done; `[-]` is "in
     * progress" / "partially done". Consumers tally these to
     * compute statistics cookies on the parent headline. */
    checkbox: $ => $._list_checkbox,

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
    /* File-level / element-level directive line (`#+TITLE: foo`).
     * The scanner emits `_keyword_line` covering only the `#+`
     * prefix; JS rules consume the keyword name, separator, and value
     * as separate named children. */
    keyword: $ => seq(
      $._keyword_line,
      field('name', $.directive_name),
      ':',
      optional(seq(/[ \t]+/, field('value', $.directive_value))),
      /[ \t]*\r?\n/,
    ),
    affiliated_keyword: $ => seq(
      $._affiliated_keyword_line,
      field('name', $.directive_name),
      ':',
      optional(seq(/[ \t]+/, field('value', $.directive_value))),
      /[ \t]*\r?\n/,
    ),

    directive_name:  $ => /[A-Za-z][A-Za-z0-9_-]*/,
    directive_value: $ => /[^\n]+/,
    comment: $ => prec.right(repeat1($._comment_line)),
    fixed_width: $ => prec.right(repeat1($._fixed_width_line)),
    horizontal_rule: $ => $._hrule_line,
    diary_sexp: $ => $._diary_sexp_line,
    paragraph: $ => prec.right(repeat1($._inline_content_line)),
  },
});
