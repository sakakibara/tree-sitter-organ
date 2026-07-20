#!/usr/bin/env node
// Byte-exact parse tests for inputs a corpus file cannot carry
// (bare-CR, classic-Mac line endings).  Writes each case to a temp
// file, parses it, and asserts required node names appear and ERROR
// does not.  Mirrors scripts/test-crlf.js's structure and cases one
// for one where the shape overlaps; the last two cases are bare-CR-
// specific because they exercise a mechanism CRLF never touches:
// tree-sitter's own `lexer->get_column()` only resets to 0 on a real
// `\n`, so a bare-CR file needs scanner.c's own `bol_col` tracking
// (see src/scanner.c) to know when it is at the start of a physical
// line - column-0 detection is exactly where a heading closing an
// open container, or a list item's bullet/close check, could regress.
'use strict';

const fs = require('fs');
const path = require('path');
const cp = require('child_process');
const os = require('os');

const treeSitterBin = path.resolve('node_modules/.bin/tree-sitter');

const CASES = [
  {
    name: 'bare-CR property drawer',
    input: '* H\r:PROPERTIES:\r:ID: abc\r:END:\rBody\r',
    expect: ['property_drawer', 'node_property', 'paragraph'],
  },
  {
    // Span-qualified: a name-only check for `block_header_args` also
    // passes on the pre-fix parser, where the field's `[^\n]`-only
    // regex ran straight past the bare-CR terminator and swallowed
    // `print(1)\r#+end_src\r` all the way to EOF with no ERROR node -
    // silently worse than an ERROR, not just an unhandled one. The
    // exact end column pins the field to `:results output`, not EOF.
    name: 'bare-CR src block with header args',
    input: '#+begin_src lua :results output\rprint(1)\r#+end_src\r',
    expect: ['src_block', 'src_block_language',
      'block_header_args [0, 16] - [0, 31]'],
  },
  {
    name: 'bare-CR headline with tags',
    input: '* Title :a:b:\r',
    expect: ['headline_line', 'tag_list', 'tag'],
  },
  {
    name: 'bare-CR table',
    input: '| a | b |\r|---|---|\r| c | d |\r',
    expect: ['table', 'table_row', 'table_rule'],
  },
  {
    name: 'bare-CR planning',
    input: '* H\rSCHEDULED: <2026-05-01 Fri>\r',
    expect: ['planning', 'planning_keyword', 'planning_timestamp'],
  },
  {
    // Span-qualified: a name-only check for `list_item` also passes on
    // the pre-fix parser, where the second bullet's `get_column() == 0`
    // gate never re-fires on a bare-CR file (tree-sitter's own column
    // count never resets without a real `\n`) and "- two" is absorbed
    // into the first item's paragraph instead of starting a second
    // item - one `list_item` spanning the whole input, not two.
    name: 'bare-CR list',
    input: '- one\r- two\r',
    expect: ['list', 'list_item [0, 0] - [0, 6]', 'list_item [0, 6] - [0, 12]', 'bullet'],
  },
  {
    name: 'bare-CR table, final cell has no closing pipe (cell-content guard)',
    input: '| a\r| b |\r',
    expect: ['table', 'table_row', 'table_cell [0, 1] - [0, 3]'],
  },
  {
    // A column-0 heading must close an open src block on a bare-CR
    // file exactly like it does on LF/CRLF (Emacs: any construct is
    // terminated by a heading, regardless of line-ending convention).
    // tree-sitter's `get_column()` never resets on this input (no real
    // `\n` anywhere), so this only stays two siblings - not one
    // ERROR - because of the scanner's own `bol_col` bookkeeping.
    name: 'bare-CR heading closes an open src block',
    input: '* Real\r#+begin_src org\r* foo\rmore\r#+end_src\rbody\r',
    expect: ['headline [0, 0] - [0, 23]', 'headline [0, 23] - [0, 49]', 'src_block'],
  },
  {
    // A pseudo block-open line indented under a list item, where the
    // grammar has no src_block slot, degrades to plain paragraph
    // continuation (src/scanner.c's b2 TT_LBLOCK_OPEN "no slot"
    // fallback) - the whole item stays ONE paragraph across all four
    // bare-CR-terminated lines, matching the LF shape.
    name: 'bare-CR list item absorbs a nested pseudo block-open as paragraph text',
    input: '- item\r  #+begin_src lua\r  print(1)\r  #+end_src\r',
    expect: ['list_item', 'paragraph [0, 2] - [0, 48]'],
  },
];

let failed = 0;
for (const c of CASES) {
  const tmp = path.join(os.tmpdir(), `org-bare-cr-${process.pid}-${Date.now()}.org`);
  fs.writeFileSync(tmp, c.input, 'binary');
  const r = cp.spawnSync(treeSitterBin, ['parse', tmp], {
    encoding: 'utf8',
    timeout: 10000,
  });
  fs.unlinkSync(tmp);
  const tree = r.stdout || '';
  // A span-qualified expectation (already carries " [row, col] - ...")
  // is unambiguous as a plain substring check; a bare node name still
  // needs the trailing-boundary check to avoid a name-prefix false
  // match (e.g. "table" inside "table_cell").
  const missing = c.expect.filter(n => n.includes(' [')
    ? !tree.includes(`(${n}`)
    : !tree.includes(`(${n} `) && !tree.includes(`(${n})`));
  const hasError = tree.includes('(ERROR') || tree.includes('(MISSING');
  if (missing.length > 0 || hasError || r.status === null) {
    failed++;
    console.log(`FAIL  ${c.name}`);
    if (r.status === null) console.log('      (parse timed out)');
    if (missing.length) console.log(`      missing: ${missing.join(', ')}`);
    if (hasError) console.log('      tree contains ERROR/MISSING');
    console.log(tree.split('\n').map(l => '      ' + l).join('\n'));
  } else {
    console.log(`PASS  ${c.name}`);
  }
}
console.log(`${CASES.length - failed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
