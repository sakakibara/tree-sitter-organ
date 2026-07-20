/* Shared shape of the five lesser blocks: open token, optional head
 * fields, newline, verbatim body, close token. */
const lesserBlock = ($, open, close, ...head) => seq(
  open,
  ...head,
  $._line_end,
  repeat($._lblock_body),
  close,
);

/* Field tail shared by headline_line and inlinetask_line.  Order is
 * Emacs org-complex-heading-regexp: todo, priority, COMMENT, title,
 * cookie, tags.  The priority separator must be optional(/[ \t]+/),
 * never a zero-or-more `[ \t]*` form - an empty-matching token
 * mis-lexes and can loop error recovery. */
const headlineTail = $ => [
  optional(seq(field('todo',     $.todo),     /[ \t]+/)),
  optional(seq(field('priority', $.priority), optional(/[ \t]+/))),
  optional(seq(field('comment',  $.comment_marker), /[ \t]+/)),
  optional(field('title',    $.title)),
  optional(field('cookie',   $.statistics_cookie)),
  optional(field('tag_list', $.tag_list)),
];

/* `: [value]` tail shared by formula, keyword, affiliated_keyword. */
const directiveTail = $ => [
  ':',
  optional(seq(optional(/[ \t]+/), field('value', $.directive_value))),
  $._line_end,
];

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
    $._list_counter,           // `[@N]` force-renumber cookie after a bullet
    $._item_tag_text,          // description-list term before ` :: `
    $._item_tag_sep,           // the ` :: ` separator after an item tag
    $._fn_empty_line,          // empty line inside a footnote definition body
    $._diary_sexp_body,        // diary sexp body up to the line's last `)`
    $._formula_line,           // `#+TBLFM:` prefix (name confirmed by scanner)
    $._block_switches,         // `-n 20 -r -l "fmt"` run (scanned, see scanner.c)
    $._export_format,          // export-block backend (scanned, see scanner.c)
    $._line_end,               // `[ \t]*\r?\n`, or zero-width at EOF
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
    /* A leading run of empty lines can belong to the document prefix
     * repeat or (when no zeroth_section follows) to the trailing
     * repeat; GLR explores both, the resulting trees are identical. */
    [$.document],
  ],

  rules: {
    document: $ => seq(
      repeat($._empty_line),
      optional($.zeroth_section),
      repeat(choice($._empty_line, $.headline)),
    ),

    zeroth_section: $ => prec.right(seq(
      $._meaningful_content_line,
      repeat($._content_line),
    )),

    headline: $ => prec.right(1, seq(
      $.headline_line,
      /* The newline ending the headline line is itself an
       * _empty_line token; planning and the property drawer are
       * only recognized DIRECTLY after it (Emacs parity). */
      optional(seq(
        $._empty_line,
        optional($.planning),
        optional($.property_drawer),
      )),
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
      ...headlineTail($),
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
     * keywords on one line). The scanner emits one `_planning_line`
     * token per entry, covering `[ws]*KEYWORD:`, so JS rules can
     * consume every keyword + timestamp pair on the line. */
    planning: $ => prec.right(repeat1($.planning_line)),

    planning_line: $ => seq(
      repeat1($.planning_entry),
      optional(token(prec(-1, /[^\r\n]+/))),
      $._line_end,
    ),

    /* The external `_planning_line` token covers `[ws]*KEYWORD:`
     * (keyword and colon included) and fires once per entry - the
     * scanner re-detects mid-line for a second keyword on the same
     * line.  Case-insensitive like Emacs `org-keyword-time-regexp`
     * under case-fold-search. */
    planning_entry: $ => seq(
      field('keyword',   alias($._planning_line, $.planning_keyword)),
      optional(/[ \t]+/),
      field('timestamp', $.planning_timestamp),
    ),

    planning_timestamp: $ => /<[^<>\n]+>|\[[^\]\n]+\]/,

    /* `_propdrawer_close` covers the whole `:END:` line — or is
     * zero-width when a headline or EOF truncates the drawer. */
    property_drawer: $ => seq(
      $._propdrawer_open,
      /[Pp][Rr][Oo][Pp][Ee][Rr][Tt][Ii][Ee][Ss]/,
      ':',
      $._line_end,
      repeat($.node_property),
      $._propdrawer_close,
    ),
    /* Property line inside a property_drawer: `:KEY: value`. Scanner
     * emits `_node_property_line` covering only the leading `:` so
     * JS rules expose `name` and optional `value` as named children. */
    node_property: $ => seq(
      $._node_property_line,
      field('name', $.property_name),
      ':',
      optional(seq(optional(/[ \t]+/), field('value', $.property_value))),
      $._line_end,
    ),

    property_name:  $ => /[A-Za-z_][A-Za-z0-9_+-]*/,
    property_value: $ => /[^ \t\n][^\n]*/,

    section: $ => prec.right(seq(
      $._meaningful_content_line,
      repeat($._content_line),
    )),

    // Body lines that may appear inside any container *except* a
    // footnote_definition.  Footnote definitions terminate at the next
    // `[fn:LABEL]` line per Emacs, so footnote_definition can't recurse
    // into its own body — see `_footnote_body_content_line` below.
    _non_fn_meaningful_content_line: $ => choice(
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
      $._fn_empty_line,
    ),

    /* Custom-named drawer `:NAME: ... :END:`. Scanner emits
     * `_drawer_open` covering only the leading `:` so JS rules expose
     * the name as a child field.  `_drawer_close` covers the whole
     * `:END:` line — or is zero-width when a headline or EOF
     * truncates the drawer. */
    drawer: $ => seq(
      $._drawer_open,
      field('name', $.drawer_name),
      ':',
      $._line_end,
      repeat($._content_line),
      $._drawer_close,
    ),

    drawer_name: $ => /[A-Za-z_][A-Za-z0-9_-]*/,
    greater_block: $ => seq(
      $._gblock_open,
      field('name', $.block_name),
      optional(seq(/[ \t]+/, field('args', $.block_args))),
      $._line_end,
      repeat($._content_line),
      $._gblock_close,
    ),
    dynamic_block: $ => seq(
      $._dynblock_open,
      optional(/[ \t]+/),
      field('name', $.block_name),
      optional(seq(/[ \t]+/, field('args', $.block_args))),
      $._line_end,
      repeat($._content_line),
      $._dynblock_close,
    ),
    block_name: $ => /[^ \t\r\n]+/,
    block_args: $ => /[^ \t\n][^\n]*/,

    /* Lesser blocks. The C scanner emits `_*_block_open` covering only
     * the directive prefix (`#+begin_src` / `#+begin_example` / …),
     * leaving the language identifier + header arguments + newline to
     * be parsed as JS rules. That makes `language`, `header_args` real
     * named children (a Babel-aware consumer can read them directly). */
    src_block: $ => lesserBlock($, $._src_block_open, $._src_block_close,
      optional(seq(/[ \t]+/, field('language',    $.src_block_language))),
      optional(seq(optional(/[ \t]+/), field('switches',    $.block_switches))),
      optional(seq(optional(/[ \t]+/), field('header_args', $.block_header_args)))),
    example_block: $ => lesserBlock($, $._example_block_open, $._example_block_close,
      optional(seq(optional(/[ \t]+/), field('switches',    $.block_switches))),
      optional(seq(optional(/[ \t]+/), field('header_args', $.block_header_args)))),
    export_block: $ => lesserBlock($, $._export_block_open, $._export_block_close,
      optional(seq(/[ \t]+/, field('format', $.export_format))),
      optional(seq(optional(/[ \t]+/), field('header_args', $.block_header_args)))),
    verse_block: $ => lesserBlock($, $._verse_block_open, $._verse_block_close,
      optional(seq(/[ \t]+/, field('header_args', $.block_header_args)))),
    comment_block: $ => lesserBlock($, $._comment_block_open, $._comment_block_close),

    /* Source-block language identifier (`lua`, `python`, `org`, ...).
     * Emacs takes the first non-whitespace run unconditionally, even
     * switch- or header-arg-shaped junk, so this is a catch-all non-ws
     * run; prec 2 makes it win the lexical conflict with
     * block_header_args at the language position. */
    src_block_language: $ => token(prec(2, /[^ \t\r\n]+/)),

    /* Babel-style header arguments: `:key value :key2 v2 ...`, or any
     * other tail Emacs couldn't parse as switches. Captured as one node. */
    block_header_args: $ => /[^ \t\r\n][^\n]*/,

    /* Block switches: `-n 20 -r -l "fmt"` etc. One node covering the
     * whole run; values are numbers or double-quoted strings. Scanned
     * externally (scan_block_switches in scanner.c) rather than as a
     * regex token competing with block_header_args' catch-all:
     * tree-sitter's internal lexer commits to one token per merged DFA
     * state with no backtracking, so a regex-only split cannot
     * simultaneously keep an existing multi-atom run intact
     * (`-n 20 -r -l "fmt"`) and hand a malformed run's tail
     * (`-n-20 ...`) to block_header_args without either an ERROR or the
     * header-args catch-all swallowing a valid leading run. */
    block_switches: $ => $._block_switches,

    /* Export-block backend name (`html`, `latex`, ...). Unlike
     * src_block_language this is NOT a catch-all: Emacs's
     * org-element-export-block-parser only extracts a backend when
     * nothing but trailing whitespace follows it to end of line;
     * anything else (`#+begin_export html <b>`) makes the backend nil
     * and the whole tail falls through to block_header_args instead.
     * Scanned externally (scan_export_format) since that "nothing else
     * to end of line" check needs lookahead a JS regex token can't do
     * without also winning the lexical race against block_header_args'
     * catch-all for the malformed case. */
    export_format: $ => $._export_format,
    latex_environment: $ => seq(
      $._latexenv_open,
      field('name', $.latexenv_name),
      '}',
      $._line_end,
      repeat($._latexenv_body),
      $._latexenv_close,
    ),
    latexenv_name: $ => /[A-Za-z0-9*]+/,

    list: $ => prec.right(seq(
      $._plain_list_open,
      repeat1($.list_item),
      $._plain_list_close,
    )),
    list_item: $ => prec.right(seq(
      field('bullet', alias($._list_item_bullet, $.bullet)),
      optional(field('counter', $.counter)),
      optional(field('checkbox', $.checkbox)),
      optional(seq(
        field('item_tag', $.item_tag),
        $._item_tag_sep,
      )),
      /* A single blank line stays inside the item (Emacs ends a
       * list only at two consecutive blanks - the scanner refuses
       * to continue past the second one). */
      repeat(choice($.paragraph, $.list, $._empty_line)),
    )),

    /* Force-renumber counter `[@N]` after a bullet (e.g. `1. [@5] ...`).
     * Emacs `org-element` exposes this as the item's `:counter`. */
    counter: $ => $._list_counter,

    /* Description-list term: the `TERM` in `- TERM :: definition`.  The
     * ` :: ` separator (`_item_tag_sep`) follows but is not part of the
     * node.  Emacs `org-element` exposes this as the item's `:tag`. */
    item_tag: $ => $._item_tag_text,

    /* Checkbox glyph at start of a list item: `[ ]` / `[x]` / `[X]`
     * / `[-]`. The `[X]` form is uppercase done; `[-]` is "in
     * progress" / "partially done". Consumers tally these to
     * compute statistics cookies on the parent headline. */
    checkbox: $ => $._list_checkbox,

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
    /* A row's last cell may omit its closing `|` (Emacs accepts
     * `| a | b` and aligns it to `| a | b |`); the trailing pipe-less
     * cell is the optional `_table_cell_content` before the row end. */
    table_row: $ => seq(
      $._table_row_start,
      repeat($.table_cell),
      optional(alias($._table_cell_content, $.table_cell)),
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
    inlinetask: $ => prec.right(1, seq(
      $.inlinetask_line,
      optional(seq(
        $._empty_line,
        optional($.planning),
        optional($.property_drawer),
      )),
      repeat($._content_line),
      $._inlinetask_close,
    )),

    inlinetask_line: $ => seq(
      field('stars',     alias($._inlinetask_open, $.stars)),
      /* No leading /[ \t]+/ — the `_inlinetask_open` token already
       * covers stars + one ws byte. */
      ...headlineTail($),
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
      $._line_end,
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
     * directive name.  The scanner emits `_formula_line` (instead of
     * `_keyword_line`) only when the directive name is exactly `TBLFM`
     * (case-insensitive); a name that merely starts with it, e.g.
     * `#+TBLFMx:`, stays a `keyword`. */
    formula: $ => seq(
      $._formula_line,
      /* Case-insensitive (Emacs `case-fold-search` on
       * `org-table-formula-regexp`).  `#+tblfm:` and `#+Tblfm:` work. */
      field('name', alias(/[Tt][Bb][Ll][Ff][Mm]/, $.directive_name)),
      ...directiveTail($),
    ),
    keyword: $ => seq(
      $._keyword_line,
      field('name', $.directive_name),
      ...directiveTail($),
    ),
    affiliated_keyword: $ => seq(
      $._affiliated_keyword_line,
      field('name', $.directive_name),
      ...directiveTail($),
    ),

    directive_name:  $ => /[A-Za-z][A-Za-z0-9_-]*/,
    directive_value: $ => /[^ \t\n][^\n]*/,
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
      $._line_end,
    ),
    fixed_width: $ => prec.right(repeat1($.fixed_width_line)),
    fixed_width_line: $ => seq(
      $._fixed_width_line,
      optional(field('body', alias($._fixed_width_body_text, $.fixed_width_body))),
      $._line_end,
    ),
    horizontal_rule: $ => $._hrule_line,
    /* Diary sexp. Two surface forms — bare `%%(...)` and the
     * active-timestamp `<%%(...)>` form — both decompose into a
     * `body` field plus a closing-punctuation choice. */
    diary_sexp: $ => seq(
      $._diary_sexp_line,
      optional(field('body', $.diary_sexp_body)),
      choice(')', ')>'),
      $._line_end,
    ),

    diary_sexp_body: $ => $._diary_sexp_body,
    paragraph: $ => prec.right(repeat1($._inline_content_line)),
  },
});
