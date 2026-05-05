# Contributing to tree-sitter-organ

## Spec ↔ grammar workflow

`spec/org.abnf` is the **authoritative specification** of what this parser
should accept. `grammar.js` is the **implementation** — full of tree-sitter
quirks (external scanner tokens, GLR conflicts, precedence) that exist to
make the parser work, not to define the language.

When the two disagree, that's a `grammar.js` bug, not an ABNF bug.

**Default workflow for spec changes:**

1. Edit `spec/org.abnf` to express the intended shape.
2. Update `grammar.js` (and `src/scanner.c` if external tokens are
   involved) to implement it.
3. Run `make spec-check` — it verifies the rule-name sets line up.
4. Run `make test` — runs the tree-sitter corpus tests.

`make spec-check` runs in CI on every PR (`.github/workflows/build.yml`,
`spec-check` job).

### What `make spec-check` enforces

- Every public node in `grammar.js` (any rule whose name doesn't start
  with `_`) corresponds to a rule in `spec/org.abnf`.
- Every rule in `spec/org.abnf` corresponds to a node in `grammar.js`,
  EXCEPT for rules listed in `spec/.spec-check-ignores` — those are
  shape helpers (primitives, character classes, structural sub-rules)
  that exist in the spec for documentation but are collapsed into
  external scanner tokens / regex character classes by the parser.

### What it does NOT enforce

Rule **bodies**. The script compares rule-name sets, not productions.
A rule's RHS can drift between ABNF and grammar.js without `make
spec-check` catching it. Treat the ABNF body as the source of truth and
review by hand when changing one.

## Building

```sh
make           # tree-sitter generate + cc → build/<platform>/org.so
make test      # tree-sitter corpus tests
make spec-check
```
