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
    $._comment_body_text,
    $._fixed_width_body_text,
  ],

  extras: _ => [],

  conflicts: $ => [
    /* Empty lines between heading_open boundaries can belong to either
     * the inner headline's tail (`repeat(_empty_line, headline)`) or the
     * outer document/headline's repeat.  GLR explores both; precedence
     * via prec.right(headline) keeps them inside the inner headline. */
    [$.headline],
    /* `table` has two structurally-distinct shapes (with/without
     * header) that match overlapping byte sequences; GLR explores
     * both and prec.dynamic(2) on the header path wins ties. */
    [$.table],
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
      /* The trailing separator after `[#X]` is optional — mirrors Emacs
       * `org-priority-regexp` ("\\] ?"), so `[#A]Foo` (no space) parses
       * as priority + title=Foo.  `[ \t]*` (not `[ \t]+`) handles both. */
      optional(seq(field('priority', $.priority), /[ \t]*/)),
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

    /* Case-insensitive: Emacs's `org-keyword-time-regexp` is matched
     * with `case-fold-search = t`, so `scheduled:` / `Closed:` etc.
     * are recognised the same as the canonical uppercase form. */
    planning_keyword:   $ => /[Ss][Cc][Hh][Ee][Dd][Uu][Ll][Ee][Dd]|[Dd][Ee][Aa][Dd][Ll][Ii][Nn][Ee]|[Cc][Ll][Oo][Ss][Ee][Dd]/,
    planning_timestamp: $ => /[<\[][^\n>\]]+[>\]]/,

    property_drawer: $ => seq(
      $._propdrawer_open,
      /[Pp][Rr][Oo][Pp][Ee][Rr][Tt][Ii][Ee][Ss]/,
      ':',
      /[ \t]*\r?\n/,
      repeat($.node_property),
      $._propdrawer_close,
      /[Ee][Nn][Dd]/,
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
      $.formula,
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
      /[Ee][Nn][Dd]/,
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

    /* Table. Header rows (rows immediately followed by a `|---|`
     * rule) are not exposed as a distinct node here — tree-sitter's
     * GLR + dynamic precedence couldn't pick the header-path
     * unambiguously without extensive grammar restructuring. Per
     * Emacs convention this distinction is recoverable by walking
     * siblings: a `table_row` whose next sibling is `table_rule`
     * is the header. */
    /* A table EITHER opens with a header (a row immediately followed
     * by a `|---|` rule) OR has no header.  The two shapes are
     * structurally distinct — `seq(header, rule, ...)` vs `repeat1(...)` —
     * but the no-header path can also match the bytes of a header
     * shape; declare the conflict and bias toward the header path. */
    table: $ => choice(
      prec.dynamic(2, seq(
        field('header', alias($.table_row, $.table_header_row)),
        $.table_rule,
        repeat(choice($.table_row, $.table_rule)),
      )),
      prec.right(repeat1(choice($.table_row, $.table_rule))),
    ),
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

    /* Footnote definition: `[fn:LABEL]` followed by body lines.
     * Scanner emits `_footnote_def_line` covering only `[fn:`; JS
     * consumes label + `]` and then any body content. */
    footnote_definition: $ => prec.right(seq(
      $._footnote_def_line,
      field('label', $.footnote_label),
      ']',
      repeat($._footnote_body_content_line),
    )),

    footnote_label: $ => /[A-Za-z0-9_-]+/,
    /* Inlinetask: a 15+-star "task" mini-headline that nests inside
     * a section. The opening line is decomposed (same fields as a
     * regular headline) — scanner emits `_inlinetask_open` covering
     * only the leading stars + one space. The close `*************** END`
     * line stays as a single token (`_inlinetask_close`). */
    inlinetask: $ => seq(
      $.inlinetask_line,
      repeat($._content_line),
      $._inlinetask_close,
    ),

    inlinetask_line: $ => seq(
      field('stars',     alias($._inlinetask_open, $.stars)),
      /* No leading /[ \t]+/ — the `_inlinetask_open` token already
       * covers stars + one ws byte. */
      optional(seq(field('todo',     $.todo),     /[ \t]+/)),
      optional(seq(field('comment',  $.comment_marker), /[ \t]+/)),
      /* The trailing separator after `[#X]` is optional — mirrors Emacs
       * `org-priority-regexp` ("\\] ?"), so `[#A]Foo` (no space) parses
       * as priority + title=Foo.  `[ \t]*` (not `[ \t]+`) handles both. */
      optional(seq(field('priority', $.priority), /[ \t]*/)),
      optional(field('title',    $.title)),
      optional(field('cookie',   $.statistics_cookie)),
      optional(field('tag_list', $.tag_list)),
    ),

    /* Clock entry: `CLOCK: [start]` (running) or
     * `CLOCK: [start]--[end] => H:MM` (closed).  Scanner emits
     * `_clock_line` covering `[ws]*CLOCK:` (the prefix); JS rules
     * consume the timestamp(s) + optional duration as named fields. */
    clock: $ => seq(
      $._clock_line,
      /[ \t]+/,
      field('start', $.clock_timestamp),
      optional(seq(
        /[ \t]*--[ \t]*/,
        field('end', $.clock_timestamp),
        optional(seq(
          /[ \t]*=>[ \t]+/,
          field('duration', $.clock_duration),
        )),
      )),
      /[ \t]*\r?\n/,
    ),

    clock_timestamp: $ => /\[[^\]\n]+\]/,
    clock_duration:  $ => /\d+:\d{2}/,
    /* File-level / element-level directive line (`#+TITLE: foo`).
     * The scanner emits `_keyword_line` covering only the `#+`
     * prefix; JS rules consume the keyword name, separator, and value
     * as separate named children.
     *
     * `formula` is a `#+TBLFM:` directive — a separate node type so
     * consumers can locate table formulas without string-matching the
     * directive name.  prec(2) on the literal name beats the more
     * general `directive_name` regex. */
    formula: $ => seq(
      $._keyword_line,
      field('name', alias(token(prec(2, 'TBLFM')), $.directive_name)),
      ':',
      optional(seq(/[ \t]+/, field('value', $.directive_value))),
      /[ \t]*\r?\n/,
    ),
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
    /* Comment / fixed-width paragraphs.  The body content is exposed
     * as a `comment_body` / `fixed_width_body` field via an external
     * scanner token (NOT a `/[^\n]+/` regex — that earlier attempt
     * caused a parse-table hang when src_block + inlinetask appeared
     * together).  Scanner emits `_comment_body_text` /
     * `_fixed_width_body_text` covering body bytes (excluding leading
     * `#` / `:` and trailing newline). */
    comment: $ => prec.right(repeat1($.comment_line)),
    comment_line: $ => seq(
      $._comment_line,
      optional(field('body', alias($._comment_body_text, $.comment_body))),
      /\r?\n/,
    ),
    fixed_width: $ => prec.right(repeat1($.fixed_width_line)),
    fixed_width_line: $ => seq(
      $._fixed_width_line,
      optional(field('body', alias($._fixed_width_body_text, $.fixed_width_body))),
      /\r?\n/,
    ),
    horizontal_rule: $ => $._hrule_line,
    /* Diary sexp. Two surface forms — bare `%%(...)` and the
     * active-timestamp `<%%(...)>` form — both decompose into a
     * `body` field plus a closing-punctuation choice. */
    diary_sexp: $ => seq(
      $._diary_sexp_line,
      field('body', $.diary_sexp_body),
      choice(')', ')>'),
      /[ \t]*\r?\n/,
    ),

    diary_sexp_body: $ => /[^\n)]*/,
    paragraph: $ => prec.right(repeat1($._inline_content_line)),
  },
});
