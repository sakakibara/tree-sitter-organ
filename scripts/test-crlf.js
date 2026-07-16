#!/usr/bin/env node
// Byte-exact parse tests for inputs a corpus file cannot carry
// (CRLF line endings).  Writes each case to a temp file, parses it,
// and asserts required node names appear and ERROR does not.
'use strict';

const fs = require('fs');
const path = require('path');
const cp = require('child_process');
const os = require('os');

const treeSitterBin = path.resolve('node_modules/.bin/tree-sitter');

const CASES = [
  {
    name: 'crlf property drawer',
    input: '* H\r\n:PROPERTIES:\r\n:ID: abc\r\n:END:\r\nBody\r\n',
    expect: ['property_drawer', 'node_property', 'paragraph'],
  },
  {
    name: 'crlf src block',
    input: '#+begin_src lua\r\nprint(1)\r\n#+end_src\r\n',
    expect: ['src_block', 'src_block_language'],
  },
  {
    name: 'crlf headline with tags',
    input: '* Title :a:b:\r\n',
    expect: ['headline_line', 'tag_list', 'tag'],
  },
  {
    name: 'crlf table',
    input: '| a | b |\r\n|---|---|\r\n| c | d |\r\n',
    expect: ['table', 'table_row', 'table_rule'],
  },
  {
    name: 'crlf planning',
    input: '* H\r\nSCHEDULED: <2026-05-01 Fri>\r\n',
    expect: ['planning', 'planning_keyword', 'planning_timestamp'],
  },
  {
    name: 'crlf list',
    input: '- one\r\n- two\r\n',
    expect: ['list', 'list_item', 'bullet'],
  },
];

let failed = 0;
for (const c of CASES) {
  const tmp = path.join(os.tmpdir(), `org-crlf-${process.pid}-${Date.now()}.org`);
  fs.writeFileSync(tmp, c.input, 'binary');
  const r = cp.spawnSync(treeSitterBin, ['parse', tmp], {
    encoding: 'utf8',
    timeout: 10000,
  });
  fs.unlinkSync(tmp);
  const tree = r.stdout || '';
  const missing = c.expect.filter(
    n => !tree.includes(`(${n} `) && !tree.includes(`(${n})`));
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
