#!/usr/bin/env node
//
// check-abnf-sync.js — verify spec/org.abnf is in sync with grammar.js.
//
// Reports rule-name drift in either direction.  Exits 0 on perfect sync,
// 1 on drift, 2 on input errors.
//
// Usage:
//   node scripts/check-abnf-sync.js [grammar.js] [spec.abnf]
//
// Defaults: grammar.js + spec/org.abnf at the repo root.
//
// The ignore list (rules that exist in the ABNF as documentation but
// don't have a 1:1 grammar.js node — primitives, character classes,
// shape helpers) lives in `spec/.spec-check-ignores`.

'use strict';

const fs = require('fs');
const path = require('path');

const grammarPath = process.argv[2] || 'grammar.js';
const abnfPath    = process.argv[3] || 'spec/org.abnf';
const ignoresPath = path.join(path.dirname(abnfPath), '.spec-check-ignores');

function read(p) {
  if (!fs.existsSync(p)) {
    process.stderr.write(`error: not found: ${p}\n`);
    process.exit(2);
  }
  return fs.readFileSync(p, 'utf8');
}

// Public nodes in grammar.js: top-level `<name>: $ => ...` lines whose name
// does NOT start with `_` (private convention), plus the public entries of
// the `externals` array.  The `externals`, `conflicts`, and `extras` array
// headers match the surface pattern but aren't rules - skip them.
function extractGrammarNodes(src) {
  const out = [];
  const skip = new Set(['externals', 'conflicts', 'extras']);
  for (const line of src.split('\n')) {
    const m = line.match(/^\s+([a-z][a-zA-Z0-9_]*)\s*:\s*\$\s*=>/);
    if (m && !skip.has(m[1])) out.push(m[1]);
  }
  return out.concat(extractExternalNodes(src));
}

// A public `$.name` entry in the `externals` array is a named node too:
// the scanner supplies its bytes, so it has no `<name>: $ => ...` rule for
// the loop above to find.  Private externals are `_`-prefixed and so
// never match the leading `[a-z]`.
function extractExternalNodes(src) {
  const start = src.indexOf('externals:');
  if (start < 0) return [];
  const end = src.indexOf('\n  ],', start);
  if (end < 0) return [];
  return [...src.slice(start, end).matchAll(/\$\.([a-z][a-zA-Z0-9_]*)/g)]
    .map(m => m[1]);
}

// ABNF rules: top-level lines `<name> = …`.  Continuation lines start
// with whitespace, so they don't match.
function extractAbnfRules(src) {
  const out = [];
  for (const line of src.split('\n')) {
    const m = line.match(/^([a-z][a-z0-9-]*)\s*=/);
    if (m) out.push(m[1]);
  }
  return out;
}

function readIgnores() {
  if (!fs.existsSync(ignoresPath)) return new Set();
  return new Set(
    fs.readFileSync(ignoresPath, 'utf8')
      .split('\n')
      .map(l => l.trim())
      .filter(l => l && !l.startsWith('#'))
  );
}

const snakeToKebab = s => s.replace(/_/g, '-');

const grammarSrc = read(grammarPath);
const abnfSrc    = read(abnfPath);
const ignores    = readIgnores();

const grammarNodes = extractGrammarNodes(grammarSrc);
const abnfRules    = extractAbnfRules(abnfSrc);

const grammarSet = new Set(grammarNodes.map(snakeToKebab));
const abnfSet    = new Set(abnfRules);

const grammarOnly = [...grammarSet].filter(x => !abnfSet.has(x)    && !ignores.has(x)).sort();
const abnfOnly    = [...abnfSet]   .filter(x => !grammarSet.has(x) && !ignores.has(x)).sort();

console.log(`ABNF sync check: ${grammarPath} vs ${abnfPath}`);
if (grammarOnly.length === 0 && abnfOnly.length === 0) {
  console.log(`  in sync — ${grammarNodes.length} nodes, ${abnfRules.length} rules`);
  process.exit(0);
}
if (grammarOnly.length > 0) {
  console.log();
  console.log(`  in grammar.js but missing from ABNF (${grammarOnly.length}):`);
  for (const k of grammarOnly) console.log(`    ${k}`);
}
if (abnfOnly.length > 0) {
  console.log();
  console.log(`  in ABNF but missing from grammar.js (${abnfOnly.length}):`);
  for (const k of abnfOnly) console.log(`    ${k}`);
  console.log();
  console.log(`  (rules that document structural shape with no 1:1 grammar`);
  console.log(`   node should be added to ${ignoresPath})`);
}
process.exit(1);
