#!/usr/bin/env node
// run-corpus-tests.js -- runs `tree-sitter test` under a bounded wall-clock
// timeout so a scanner hang fails `make test` instead of hanging it
// forever.  Uses Node's built-in spawnSync timeout (portable: no
// dependency on the `timeout`(1) coreutils binary, which macOS doesn't
// ship by default).
//
// Invokes the native binary directly rather than `node_modules/.bin/
// tree-sitter` - that path is `tree-sitter-cli/cli.js`, a shim that
// spawns this same binary as an ASYNC grandchild and forwards its exit
// via `.on('close', process.exit)`.  `close` fires as `(code, signal)`;
// when the child dies by signal, `code` is `null`, and `process.exit(null)`
// reports success (status 0) - silently masking a killed process.  It
// also means a timeout on the shim doesn't kill the binary underneath
// it, leaving a runaway process orphaned.  Spawning the binary directly
// makes it this script's immediate child, so both the exit signal and
// the timeout kill are observed and applied where they belong.
//
// Usage: node scripts/run-corpus-tests.js [tree-sitter test args...]

'use strict';

const cp = require('child_process');
const path = require('path');

const exeName = process.platform === 'win32' ? 'tree-sitter.exe' : 'tree-sitter';
const treeSitterBin = path.resolve('node_modules/tree-sitter-cli', exeName);
const TIMEOUT_MS = 60000;

const startedAt = Date.now();
const r = cp.spawnSync(treeSitterBin, ['test', ...process.argv.slice(2)], {
  stdio: 'inherit',
  timeout: TIMEOUT_MS,
});
const elapsedMs = Date.now() - startedAt;

if (r.error) {
  process.stderr.write(`error: failed to run tree-sitter test: ${r.error.message}\n`);
  process.exit(1);
}
if (r.signal) {
  // r.signal may be this script's own SIGTERM (fired once TIMEOUT_MS
  // elapses) or an external SIGKILL (e.g. the OS OOM-killing a runaway
  // process well before that) - either way, the process never finished.
  process.stderr.write(
    `\nerror: tree-sitter test was killed by ${r.signal} after ${elapsedMs}ms ` +
    `(bounded at ${TIMEOUT_MS}ms) - likely a scanner hang\n`);
  process.exit(1);
}
process.exit(r.status === null ? 1 : r.status);
