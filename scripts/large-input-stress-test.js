#!/usr/bin/env node
// Bounded large-input parse-time regression tier.
//
// scripts/adversarial-no-error.js caps every generated input at 4096
// bytes, so it cannot see a super-linear (O(N^2)) parse-time blow-up
// that only manifests on large repeated-construct inputs - exactly the
// class the property-drawer look-ahead shipped by accident (fixed in
// d3669128, "only anchor property drawers directly under a headline or
// inlinetask") and that only a dedicated large-N check caught. This
// script generalizes that one-off check into a handful of high-risk
// repeated-construct shapes covering the scanner's per-line look-ahead
// and dispatch paths, so a future quadratic in another construct is
// caught here instead of by luck.
//
// Each shape parses a large N under a wall-clock spawnSync bound with
// SIGKILL: a linear-time parser finishes in tens of ms, a quadratic one
// blows well past the bound and gets killed, so the check FAILS instead
// of hanging.

'use strict';

const fs = require('fs');
const path = require('path');
const cp = require('child_process');
const os = require('os');

const treeSitterBin = path.resolve('node_modules/.bin/tree-sitter');

const N = 8000;
const TIMEOUT_MS = 3000;

const SHAPES = [
  {
    // Regression cover for d3669128: a run of nested `:PROPERTIES:`
    // lines used to re-trigger the whole-body property-drawer
    // look-ahead once per nesting level - O(N^2) total.
    name: 'propdrawer nesting',
    gen: () => '* H\n' + ':PROPERTIES:\n'.repeat(N) + 'bad\n',
  },
  {
    name: 'stacked headlines',
    gen: () => '* H\n'.repeat(N),
  },
  {
    name: 'list-item continuation runs',
    gen: () => '- item\n' + '  continuation line\n'.repeat(N),
  },
  {
    name: 'table with many rows',
    gen: () => '| a | b |\n'.repeat(N),
  },
  {
    name: 'block with many body lines',
    gen: () => '#+begin_src\n' + 'line\n'.repeat(N) + '#+end_src\n',
  },
  {
    name: 'repeated planning lines',
    gen: () => '* H\n' + 'SCHEDULED: <2024-01-01 Mon>\n'.repeat(N),
  },
  {
    // Regression cover for 0ff6a2f: a lesser block's close line with
    // trailing structured content, repeated so a per-close-line
    // re-scan of the remaining input would show up as quadratic.
    name: 'close-line trailers',
    gen: () => '#+begin_src\nx\n#+end_src trailing\n'.repeat(N),
  },
];

let failed = false;
for (const shape of SHAPES) {
  const input = shape.gen();
  const tmp = path.join(os.tmpdir(), `org-large-stress-${process.pid}-${shape.name.replace(/\W+/g, '_')}.org`);
  fs.writeFileSync(tmp, input);

  const start = Date.now();
  const r = cp.spawnSync(treeSitterBin, ['parse', '-q', tmp], {
    encoding: 'utf8',
    timeout: TIMEOUT_MS,
    killSignal: 'SIGKILL',
  });
  const elapsed = Date.now() - start;
  fs.unlinkSync(tmp);

  if (r.error && r.error.code === 'ETIMEDOUT') {
    console.log(`FAIL  ${shape.name}: N=${N} did not terminate within ${TIMEOUT_MS}ms (quadratic regression?)`);
    failed = true;
    continue;
  }
  if (r.signal || r.status === null) {
    console.log(`FAIL  ${shape.name}: N=${N} parse aborted (signal=${r.signal})`);
    failed = true;
    continue;
  }
  console.log(`PASS  ${shape.name}: N=${N} lines parsed in ${elapsed}ms (bound ${TIMEOUT_MS}ms)`);
}

process.exit(failed ? 1 : 0);
