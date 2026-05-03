# tree-sitter-organ

Block-level [tree-sitter](https://tree-sitter.github.io) grammar for
[Org mode](https://orgmode.org/) — headlines, sections, lists, blocks,
drawers, tables, planning lines, footnote definitions, inline tasks.

The block grammar is vanilla Org: every rule tracks
[Worg](https://orgmode.org/worg/dev/org-syntax.html) and Emacs
`org-element-parse-buffer`. The formal spec is in `spec/org.abnf`.

Inline-level constructs (markup, links, timestamps, citations,
footnote refs, macros, export snippets, LaTeX entities) live in a
sibling grammar, `tree-sitter-organ-inline`, which is a superset of
vanilla Org around timestamp repeaters. Files that parse cleanly
under vanilla Org parse identically under both grammars.

## Build

```sh
pnpm install
make
```

Outputs `build/<arch>/parser.{so,dylib,dll}`.

## Test

```sh
make test            # tree-sitter test against test/corpus/
```

## Queries

This grammar ships node types only. Highlight queries
(`highlights.scm`), injections (`injections.scm`), local-symbol
queries (`locals.scm`), and tree-sitter directives (predicates such as
`#org-has-todo-kw?`) live in the consumer plugin and are not part of
this repository.
