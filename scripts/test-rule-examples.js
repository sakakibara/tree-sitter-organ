#!/usr/bin/env node
//
// test-rule-examples.js -- per-rule positive/negative example tests.
//
// For each `spec/examples/<rule-name>.txt`, parses every `+ <input>`
// line and asserts the named node `<rule-name>` (snake_cased from the
// filename) appears in the parse tree; parses every `- <input>` line
// and asserts the node does NOT appear.
//
// Behavior matching, not name-set matching: this is the empirical
// proof that grammar.js implements the ABNF rule rather than just
// declaring it.
//
// Fixture format (whitespace-tolerant):
//
//   # Optional comment lines starting with '#' (outside fences).
//   + valid input that must produce a `bold` node
//   + another valid input
//   - invalid input that must NOT produce a `bold` node
//
//   # Multi-line inputs use a `~~~` fence after the sigil:
//   +~~~
//   #+begin_src lua
//   print(1)
//   #+end_src
//   ~~~
//
//   -~~~
//   not a src block
//   ~~~
//
// Outside fences, blank lines and `#` comments are skipped.  Inside
// a fence, every line (including blanks and `#`) is part of the
// input verbatim.  The leading `+ ` / `- ` (sigil + space) is the
// only marker for single-line cases.
//
// Exit codes: 0 all pass, 1 any fail, 2 input/setup error.
//
// Usage: node scripts/test-rule-examples.js [examples-dir]

'use strict';

const fs = require('fs');
const path = require('path');
const cp = require('child_process');
const os = require('os');

const examplesDir = process.argv[2] || 'spec/examples';
const treeSitterBin = path.resolve('node_modules/.bin/tree-sitter');

function fail(msg) {
  process.stderr.write(`error: ${msg}\n`);
  process.exit(2);
}

if (!fs.existsSync(examplesDir)) {
  fail(`examples directory not found: ${examplesDir}`);
}
if (!fs.existsSync(treeSitterBin)) {
  fail(`tree-sitter CLI not found at ${treeSitterBin} (run \`npm install\` first)`);
}

// Filenames are kebab-case to match ABNF rule names; node names in
// grammar.js are snake_case.  Translate.
const kebabToSnake = s => s.replace(/-/g, '_');

function parse(input) {
  const tmp = path.join(os.tmpdir(), `tsx-${process.pid}-${Date.now()}.txt`);
  fs.writeFileSync(tmp, input);
  try {
    const r = cp.spawnSync(treeSitterBin, ['parse', tmp], {
      encoding: 'utf8',
      maxBuffer: 8 * 1024 * 1024,
    });
    if (r.status !== 0 && !r.stdout) {
      throw new Error(`tree-sitter parse failed: ${r.stderr}`);
    }
    return r.stdout;
  } finally {
    try { fs.unlinkSync(tmp); } catch (_) {}
  }
}

// Node `name` is present if the parse tree mentions `(name ` (open
// paren + name + space) anywhere.  Robust: tree-sitter's S-expression
// always emits this shape for named nodes.
function hasNode(tree, name) {
  return tree.indexOf(`(${name} `) >= 0 || tree.indexOf(`(${name})`) >= 0;
}

function loadFixture(fp) {
  const raw = fs.readFileSync(fp, 'utf8').split('\n');
  const cases = [];
  let i = 0;
  while (i < raw.length) {
    const line = raw[i].replace(/\s+$/, '');
    // Fenced multi-line: `+~~~` or `-~~~` opens; sole `~~~` closes.
    if (line === '+~~~' || line === '-~~~') {
      const kind = line[0];
      const startLine = i + 1;
      const buf = [];
      i++;
      while (i < raw.length && raw[i].replace(/\s+$/, '') !== '~~~') {
        buf.push(raw[i]);
        i++;
      }
      if (i >= raw.length) {
        fail(`${fp}:${startLine}: unterminated ${kind}~~~ fence`);
      }
      i++; // consume closing fence
      // Append a trailing newline so block-level org constructs
      // (drawers, src blocks, planning lines) terminate cleanly.
      cases.push({ kind, input: buf.join('\n') + '\n', lineno: startLine });
      continue;
    }
    if (line === '' || line.startsWith('#')) {
      i++;
      continue;
    }
    if (line.startsWith('+ ') || line === '+') {
      // Single-line cases have NO trailing newline.  If a construct
      // needs a line terminator, use the multi-line `+~~~` fence.
      cases.push({ kind: '+', input: line === '+' ? '' : line.slice(2), lineno: i + 1 });
    } else if (line.startsWith('- ') || line === '-') {
      cases.push({ kind: '-', input: line === '-' ? '' : line.slice(2), lineno: i + 1 });
    } else {
      fail(`${fp}:${i + 1}: malformed line (expected '+ ' / '- ' / '#' / fence prefix): ${line}`);
    }
    i++;
  }
  return cases;
}

const fixtures = fs.readdirSync(examplesDir)
  .filter(f => f.endsWith('.txt'))
  .sort();

if (fixtures.length === 0) {
  process.stdout.write(`no fixtures in ${examplesDir}\n`);
  process.exit(0);
}

let totalPass = 0;
let totalFail = 0;
const failures = [];

for (const file of fixtures) {
  const fp = path.join(examplesDir, file);
  const ruleKebab = file.replace(/\.txt$/, '');
  const ruleSnake = kebabToSnake(ruleKebab);
  const cases = loadFixture(fp);
  if (cases.length === 0) {
    process.stdout.write(`SKIP  ${ruleKebab} (no cases)\n`);
    continue;
  }
  let filePass = 0, fileFail = 0;
  for (const c of cases) {
    const tree = parse(c.input);
    const present = hasNode(tree, ruleSnake);
    const ok = (c.kind === '+') ? present : !present;
    if (ok) {
      filePass++;
    } else {
      fileFail++;
      failures.push({
        rule: ruleSnake,
        file: fp,
        line: c.lineno,
        kind: c.kind,
        input: c.input,
        present,
        tree: tree.split('\n').slice(0, 8).join('\n'),
      });
    }
  }
  totalPass += filePass;
  totalFail += fileFail;
  const status = fileFail === 0 ? 'PASS' : 'FAIL';
  process.stdout.write(`${status}  ${ruleKebab}  (${filePass}/${cases.length})\n`);
}

if (failures.length > 0) {
  process.stdout.write('\nFailures:\n');
  for (const f of failures) {
    process.stdout.write(`  ${f.rule}  ${path.basename(f.file)}:${f.line}\n`);
    process.stdout.write(`    ${f.kind} ${JSON.stringify(f.input)}\n`);
    const expectation = f.kind === '+' ? 'expected node present' : 'expected node absent';
    process.stdout.write(`    ${expectation}, but got present=${f.present}\n`);
    process.stdout.write(`    parse:\n`);
    for (const ln of f.tree.split('\n')) {
      process.stdout.write(`      ${ln}\n`);
    }
  }
}

process.stdout.write(`\n${totalPass} passed, ${totalFail} failed\n`);
process.exit(totalFail === 0 ? 0 : 1);
