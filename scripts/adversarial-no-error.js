#!/usr/bin/env node
// adversarial-no-error.js -- no-ERROR contract harness.
//
// Generates a deterministic adversarial input space (corpus seeds +
// mutations + hostile compositions + known-bad regressions), parses
// every input with the compiled parser via batched `tree-sitter parse
// -q` (the same binary scripts/run-corpus-tests.js drives), and
// reports every input whose tree contains ERROR or MISSING nodes, whose
// parse fails to terminate (RUNAWAY), or whose parse aborts against the
// memory cap (OOM_SUSPECT).
//
// Modes:
//   --discover  full report: failing inputs grouped by tree signature
//   --gate      CI gate: zero findings or exit 1; bounded wall clock
//
// Deterministic: fixed seed constants, no runtime randomness.
//
// Every parse child runs under both a hard wall-clock timeout and a
// ulimit -v memory cap (see MEM_CAP_KB below) - the parser has known
// single-input kernel-OOM classes, and an unbounded child can invoke the
// kernel OOM killer, which may reap an unrelated process instead. Two
// spawnSync notes make the bound work as intended:
//   - a runaway parse ignores the default SIGTERM kill; killSignal
//     SIGKILL is required for the timeout to actually reap it.
//   - spawnSync sets `.error.code === 'ETIMEDOUT'` only when its own
//     timeout fired the kill; that is the sole reliable signal for
//     RUNAWAY vs OOM_SUSPECT (see classifyFailure below).

'use strict';

const fs = require('fs');
const path = require('path');
const cp = require('child_process');

const MODE = process.argv.includes('--gate') ? 'gate' : 'discover';
const GATE_DEADLINE_MS = 60000;
const CHUNK = 200;
const CHUNK_TIMEOUT_MS = 15000;
const SINGLE_TIMEOUT_MS = 3000;
// Every input this harness generates is capped at 4096 bytes (add()
// below drops anything longer) - this gate has zero no-ERROR coverage
// above that size. scripts/large-input-stress-test.js separately
// parse-time-bounds several 8000-LINE shapes, but greps nothing for
// ERROR, so it doesn't fill the gap either. Ad hoc probes (20KB
// paragraph/headline/table/TBLFM, and a line straddling scanner.c's
// ORG_LINE_BUF_MAX=8192) parsed clean when checked manually - there is
// no known >4KB no-ERROR violation - but that's an absence of testing,
// not a tested absence.
const MAX_INPUT_LEN = 4096;

const repo = path.resolve(__dirname, '..');
// Match the Makefile's `uname`-based triple (darwin-arm64, linux-aarch64,
// linux-x86_64) rather than Node's os.arch() naming (arm64/x64).
const unameArch = cp.execSync('uname -m').toString().trim();
const unameOs = cp.execSync('uname -s').toString().trim().toLowerCase();
const buildDir = path.join(repo, 'build', `${unameOs}-${unameArch}`);
const libdir = path.join(buildDir, 'ts-lib');
const workDir = path.join(buildDir, 'no-error-fuzz');
const tsBin = path.join(repo, 'node_modules', 'tree-sitter-cli',
  process.platform === 'win32' ? 'tree-sitter.exe' : 'tree-sitter');

if (!fs.existsSync(path.join(libdir, 'org.so'))) {
  process.stderr.write(`error: ${libdir}/org.so missing - run \`make test\` once (or mkdir + cp from ${buildDir}/org.so)\n`);
  process.exit(2);
}

// ---- deterministic PRNG (mulberry32, fixed seed) --------------------------
const SEED = 0x5eed0709;
function mulberry32(a) {
  return function () {
    a |= 0; a = (a + 0x6D2B79F5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
const rand = mulberry32(SEED);
const pick = (arr) => arr[Math.floor(rand() * arr.length)];
const randInt = (n) => Math.floor(rand() * n);

// ---- corpus seed extraction ----------------------------------------------
// Corpus format: `===` header lines around the name, input, then a line of
// dashes followed by the expected S-expression (first non-blank line after
// the dashes starts with `(`).
function extractCorpusInputs(corpusDir) {
  const inputs = [];
  for (const f of fs.readdirSync(corpusDir).filter((n) => n.endsWith('.txt')).sort()) {
    const lines = fs.readFileSync(path.join(corpusDir, f), 'utf8').split('\n');
    let i = 0;
    const isHeader = (j) => /^=+$/.test(lines[j]) && j + 2 < lines.length && /^=+$/.test(lines[j + 2]);
    while (i < lines.length) {
      if (!isHeader(i)) { i++; continue; }
      i += 3;
      const buf = [];
      while (i < lines.length) {
        if (/^-+$/.test(lines[i])) {
          let j = i + 1;
          while (j < lines.length && lines[j].trim() === '') j++;
          if (j < lines.length && lines[j].startsWith('(')) break;
        }
        if (isHeader(i)) break;
        buf.push(lines[i]);
        i++;
      }
      while (buf.length && buf[buf.length - 1] === '') buf.pop();
      if (buf.length) inputs.push(buf.join('\n') + '\n');
      while (i < lines.length && !isHeader(i)) i++;
    }
  }
  return inputs;
}

// ---- mutation engine ------------------------------------------------------
const STRUCT_BYTES = new Set('*#+:|-[]<>%\\ \t\n=~'.split(''));
const TAMPER = ['.', 'x', ' ', '\t', ':', '*', '#', '+', '|', '[', ']', '-', '\u00e9', '\0'];

function* mutations(seed) {
  const n = seed.length;
  // Truncation at every offset (stride on long seeds keeps the space bounded).
  const stride = n > 400 ? 7 : 1;
  for (let i = 1; i < n; i += stride) yield seed.slice(0, i);
  // Structure-relevant single-byte substitutions / deletions / insertions.
  for (let i = 0; i < n; i++) {
    if (!STRUCT_BYTES.has(seed[i])) continue;
    if (rand() < (n > 200 ? 0.35 : 1.0)) {
      yield seed.slice(0, i) + pick(TAMPER) + seed.slice(i + 1);
      yield seed.slice(0, i) + seed.slice(i + 1);
      yield seed.slice(0, i) + pick(TAMPER) + seed.slice(i);
    }
  }
  // Indent perturbations.
  yield seed.replace(/^/gm, '  ');
  yield seed.replace(/^/gm, '\t');
  yield seed.replace(/^ +/gm, '');
  // CRLF variant + case flips on directives.
  yield seed.replace(/\n/g, '\r\n');
  yield seed.replace(/#\+(\w+)/g, (m, w) => '#+' + w.toUpperCase());
  yield seed.replace(/#\+(\w+)/g, (m, w) => '#+' + w[0].toUpperCase() + w.slice(1));
  // Delimiter tampering on begin/end/drawer lines.
  yield seed.replace(/#\+end_/g, '#+end');
  yield seed.replace(/#\+begin_(\w+)/g, '#+begin_$1.');
  yield seed.replace(/:END:/gi, ':END');
  yield seed.replace(/^(\s*):(\w+):/gm, '$1:$2');
}

// ---- hostile compositions -------------------------------------------------
const FRAGMENTS = [
  '* H1\n', '** H2 :tag:\n', '*** TODO [#A] H3 [1/2] :a:b:\n',
  '#+begin_src lua\n', '#+end_src\n', '#+begin_example -n 2\n', '#+end_example\n',
  '#+begin_center\n', '#+end_center\n', '#+begin_quote\n', '#+end_quote\n',
  '#+begin: dyn :p 1\n', '#+end:\n',
  ':PROPERTIES:\n', ':CUSTOM_ID: x\n', ':END:\n', ':LOGBOOK:\n', ':mydrawer:\n',
  '- item\n', '  - nested\n', '1. [@5] counted\n', '- [ ] box\n', '- term :: def\n',
  '| a | b |\n', '|---+---|\n', '| c\n',
  'SCHEDULED: <2024-01-01 Mon>\n', 'DEADLINE: <2024-02-02>\n',
  'CLOCK: [2024-01-01 Mon 10:00]--[2024-01-01 Mon 11:00] =>  1:00\n',
  '[fn:1] a footnote\n', '*************** Inline\n', '*************** END\n',
  '#+CAPTION: cap\n', '#+TITLE: t\n', '#+TBLFM: $2=$1\n',
  '\\begin{align}\n', 'x = 1\n', '\\end{align}\n',
  '# comment\n', ': fixed\n', '-----\n', '%%(diary-date 1 2 3)\n',
  'plain paragraph text\n', '\n', '  \n',
  '#+begin_src\n', '#+end\n', '   #+end_src\n', '* \n', '*\n',
];
function* hostile(count, maxFrags) {
  for (let k = 0; k < count; k++) {
    const len = 1 + randInt(maxFrags);
    let s = '';
    for (let i = 0; i < len; i++) {
      let frag = pick(FRAGMENTS);
      const r = rand();
      if (r < 0.15) frag = '  '.repeat(1 + randInt(4)) + frag;
      else if (r < 0.22) frag = frag.replace(/\n$/, '');
      s += frag;
    }
    yield s;
  }
}

// ---- known-bad regressions (minimized findings; keep even if the PRNG
// or fragment list changes, so past bugs stay covered forever) --------------
const KNOWN_BAD = [
  '[fn:1]+ x\n', '[fn:1]1. x\n', '[fn:1]- x\n',
  ':mydrawer:\n| c |\n:END:\n', ':PROPERTIES:\n| c\n', ':LOGBOOK:\n| c\n',
  ':PROPERTIES:\n#+begin_quote\n',
  '| a | b |\n-| c | d |\n', '| a    |* b    |\n', '|* a\n', '| c |*d |\n',
  '#+begin_src.\nfoo\n', '#+begin_src#org\n', '#+begin_src* org\n',
  '#+TBLFMx: $3=$1+$2\n', '#+TBLFM: \0\n', '#+begin_quote\0\n',
  '#+begin:myblock :param 1\ncontent\n#+end:\n',
  '* H\nSCHEDULED: <2026-04-29> rest junk\n',
  '* H\n:PROPERTIES:\n+ID: x\n:END:\n', '* H\n:PROPERTIES:\nID: abc\n:END:\n',
  '* H\n:PROPERTIES:\n:ID\u00e9 x\n:END:\n', '* H\n:PROPERTIES:\n:ID: x\0\n',
  '* T\n:LOGBOOK:\nCLOCK: [2026-04-25 Sat 09:00--[2026-04-25 Sat 10:00] =>  1:00\n:END:\n',
  '* T\n:LOGBOOK:\nCLOCK: [2026-04-25 Sat 09:00]--[2026-04-25 Sat 10:00] =>  100\n:END:\n',
  'CLOCK: [2024-01-01 Mon 10:00]--[2024-01-01 Mon 11:00] =>  1:00x = 1\n',
  '#+begin_src lua -n-20 :results output\nx\n#+end_src\n',
  '#+begin_export html <b>\nx\n#+end_export\n',
  '#', '# ', ': f', '| a |', ':d:', '#+begin_src lua', '#+TITLE: Doc',
  '#- next\n',
];

// ---- close-line trailing-content family -----------------------------------
// A container's CLOSE line carrying trailing content on the SAME line
// (`#+end_example  #+TBLFM: $1=1`) is a shape none of mutations()/
// hostile() ever produce: mutations() only tampers within an existing
// seed's bytes, and no FRAGMENT above is itself "a close marker plus
// trailing junk". Structured trailing content (one that re-triggers a
// line-shape classifier - an affiliated keyword, another block open/
// close, a table row, a heading, a list bullet, a drawer marker) is the
// dangerous case: it can drive the CLOSE dispatch down a path with no
// inline-content fallback, distinct from plain trailing text which
// several already-fixed paths already absorb cleanly. Covers every
// lesser block, the greater block, a plain drawer, a dynamic block, and
// a latex environment - the full set of constructs with their own CLOSE
// token in this grammar.
const CLOSE_LINE_TRAILERS = [
  ' trailing text',
  ' junk here',
  '  #+TBLFM: $1=1',
  '  #+CAPTION: cap',
  '  #+begin_src',
  '  #+end_example',
  '  | a | b |',
  '  * head',
  '  - bullet',
  '  :drawer:',
];
const CLOSABLE_CONTAINERS = [
  ['#+begin_src sh\n', 'code\n', '#+end_src'],
  ['#+begin_example\n', 'body\n', '#+end_example'],
  ['#+begin_export html\n', 'x\n', '#+end_export'],
  ['#+begin_comment\n', 'x\n', '#+end_comment'],
  ['#+begin_verse\n', 'x\n', '#+end_verse'],
  ['#+begin_quote\n', 'x\n', '#+end_quote'],
  [':mydrawer:\n', 'x\n', ':END:'],
  ['#+begin: dyn :p 1\n', 'x\n', '#+end:'],
  ['\\begin{align}\n', 'x\n', '\\end{align}'],
];
function* closeLineTrailers() {
  for (const [open, body, close] of CLOSABLE_CONTAINERS) {
    for (const trailer of CLOSE_LINE_TRAILERS) {
      yield open + body + close + trailer + '\n';
      yield open + close + trailer + '\n';               // no body
      yield '  ' + open + '  ' + body + '  ' + close + trailer + '\n'; // indented
    }
  }
}

// ---- deep-nesting family ---------------------------------------------------
// Stacked container opens past the scope stack's capacity desync the
// scanner's scope model from the token stream if a push silently drops
// while the classifier still emits an *_OPEN token (C1: 34 nested
// `:LOGBOOK:` lines ERRORed at 32-slot capacity). hostile() never stacks
// more than ~6 fragments (see hostile() above), so depths anywhere near
// a 32-slot cap are otherwise unreached by the rest of this space.
// NEST_CAP mirrors PREPASS_STACK_MAX (src/prepass.c) and
// ORG_LIST_STACK (src/scanner.c) - both 32 today; keep in sync if either
// changes.
//
// Drawer / greater-block / dynamic-block bodies fully re-dispatch each
// line, so they self-nest by direct repetition. A lesser block and a
// latex environment are opaque once open (their body never re-dispatches,
// so repeating their own open line only ever opens the first one) and a
// property drawer only opens directly under a headline/inlinetask, so
// those three are nested inside `:wrap:\n` drawer filler lines instead -
// which does re-dispatch - to reach the target push depth, with the
// container under test as the final, capacity-deciding push. Plain-list
// nesting uses the scanner's own independent ORG_LIST_STACK, unrelated to
// the prepass scope stack.
const NEST_CAP = 32;
const NEST_DEPTHS = [NEST_CAP - 1, NEST_CAP, NEST_CAP + 1, NEST_CAP + 8];
const INLINETASK_STARS = '*'.repeat(15); // ORG_INLINETASK_MIN_LEVEL
function* deepNesting() {
  for (const depth of NEST_DEPTHS) {
    yield ':LOGBOOK:\n'.repeat(depth);                            // drawer
    yield '#+begin_center\n'.repeat(depth);                       // greater block
    yield '#+begin: dyn :p 1\n'.repeat(depth);                    // dynamic block
    yield ':wrap:\n'.repeat(depth - 1) + '#+begin_src lua\n';     // lesser block
    yield ':wrap:\n'.repeat(depth - 1) + '\\begin{align}\n';      // latex environment
    yield ':wrap:\n'.repeat(depth - 2)
      + INLINETASK_STARS + ' T\n:PROPERTIES:\n';                 // property drawer
    yield ':wrap:\n'.repeat(depth - 1) + INLINETASK_STARS + ' T\n'; // inlinetask
    let list = '';
    for (let i = 0; i < depth; i++) list += '  '.repeat(i) + '- item\n';
    yield list;                                                   // plain list
  }
}

// ---- bare-CR line-ending family (gated) ------------------------------------
// Bare-CR (classic Mac) line endings are a real Emacs line-ending
// convention - `insert-file-contents` auto-detects and normalizes LF,
// CRLF, and bare-CR alike on read - and the grammar treats a bare `\r`
// as a first-class line terminator alongside `\r\n` and bare `\n` (see
// scan_line_end / bol_col in src/scanner.c). Bare-CR is no longer a
// tracked exception: every input already in `space` gets a bare-CR
// sibling below, feeding the SAME gated set everything else does - a
// bare-CR regression fails the build exactly like an LF/CRLF one
// would. `\n` -> `\r` also collapses any `\r\n` already in the input
// to `\r\r` (two consecutive bare-CR terminators, i.e. a blank line),
// which is intentional: it stays a well-formed bare-CR-only document.
function bareCrVariant(seed) {
  return seed.replace(/\n/g, '\r');
}

// ---- input space assembly -------------------------------------------------
const seeds = extractCorpusInputs(path.join(repo, 'test', 'corpus'));
const space = new Map();
function add(s) {
  if (s.length === 0 || s.length > MAX_INPUT_LEN) return;
  if (!space.has(s)) space.set(s, space.size);
}
for (const s of KNOWN_BAD) add(s);
for (const s of seeds) add(s);
for (const s of seeds) for (const m of mutations(s)) add(m);
for (const s of hostile(4000, 6)) add(s);
for (const s of closeLineTrailers()) add(s);
for (const s of deepNesting()) add(s);
const lfCrlfCount = space.size;
for (const s of [...space.keys()]) add(bareCrVariant(s));
process.stderr.write(`no-error harness: ${seeds.length} corpus seeds -> ${lfCrlfCount} LF/CRLF inputs + ${space.size - lfCrlfCount} bare-CR inputs -> ${space.size} unique inputs\n`);

const inputDir = path.join(workDir, 'inputs');
fs.rmSync(inputDir, { recursive: true, force: true });
fs.mkdirSync(inputDir, { recursive: true });
const files = [];
for (const [content, id] of space) {
  const fp = path.join(inputDir, `i${String(id).padStart(6, '0')}.org`);
  fs.writeFileSync(fp, content);
  files.push(fp);
}

// ---- memory bound -----------------------------------------------------
// ulimit -v cap (KB), applied to every parse child via a shell wrapper.
// Measured on this repo: starting the tree-sitter CLI and parsing the
// largest legitimate input this harness ever generates (a 4096-byte
// well-formed document, alone or batched 200-up) needs ~19-20MB of
// virtual memory. This cap is ~6.4x that - generous headroom for any
// legitimate parse - while the parser's known single-input kernel-OOM
// classes blow past it within milliseconds (measured: ~80-170ms to
// SIGSEGV/SIGABRT, versus the multi-second wall-clock timeout they'd
// otherwise consume).
const MEM_CAP_KB = 131072;

// spawnSync only sets `.error.code === 'ETIMEDOUT'` when its own timeout
// fired the kill. Any other signal death, or a null status without that
// code, means the ulimit cap killed the child (malloc failure -> abort
// or a NULL-deref segfault) rather than the wall clock - that is the
// sole reliable way to tell RUNAWAY (did not terminate) apart from
// OOM_SUSPECT (terminated fast, but only because memory ran out).
function classifyFailure(r) {
  if (r.error && r.error.code === 'ETIMEDOUT') return 'RUNAWAY';
  if (r.signal || r.status === null) return 'OOM_SUSPECT';
  return null;
}

// ---- batched parse --------------------------------------------------------
function spawnBounded(args, timeoutMs, maxBuffer) {
  return cp.spawnSync('sh',
    ['-c', `ulimit -v ${MEM_CAP_KB}; exec "$0" "$@"`, tsBin, ...args],
    {
      cwd: repo,
      env: { ...process.env, TREE_SITTER_LIBDIR: libdir },
      encoding: 'utf8',
      maxBuffer,
      timeout: timeoutMs,
      killSignal: 'SIGKILL',
    });
}
function parseQ(paths, timeoutMs) {
  return spawnBounded(['parse', '-q', ...paths], timeoutMs, 64 * 1024 * 1024);
}

const startedAt = Date.now();
const flagged = [];
const runawayFiles = [];
const oomFiles = [];
function gateDeadlineCheck(where) {
  if (MODE === 'gate' && Date.now() - startedAt > GATE_DEADLINE_MS) {
    process.stderr.write(`error: no-error gate exceeded ${GATE_DEADLINE_MS}ms budget (${where})\n`);
    process.exit(1);
  }
}
for (let i = 0; i < files.length; i += CHUNK) {
  gateDeadlineCheck('chunk loop');
  const chunk = files.slice(i, i + CHUNK);
  const r = parseQ(chunk, CHUNK_TIMEOUT_MS);
  const failure = classifyFailure(r);
  if (!failure) {
    for (const line of (r.stdout || '').split('\n')) {
      const m = line.match(/^(\S+\.org)\t/);
      if (m) flagged.push(m[1]);
    }
    continue;
  }
  // Chunk contained a bad input (timeout or memory-cap abort): isolate per file.
  for (const fp of chunk) {
    gateDeadlineCheck('per-file isolation');
    const r1 = parseQ([fp], SINGLE_TIMEOUT_MS);
    const f1 = classifyFailure(r1);
    if (f1 === 'RUNAWAY') runawayFiles.push(fp);
    else if (f1 === 'OOM_SUSPECT') oomFiles.push(fp);
    else {
      for (const line of (r1.stdout || '').split('\n')) {
        const m = line.match(/^(\S+\.org)\t/);
        if (m) flagged.push(m[1]);
      }
    }
  }
}
const elapsed = Date.now() - startedAt;

// ---- report ---------------------------------------------------------------
function signatureOf(tree) {
  const lines = tree.split('\n');
  for (let i = 0; i < lines.length; i++) {
    if (/\((ERROR|MISSING)/.test(lines[i])) {
      const parent = lines.slice(0, i).reverse().find((l) => /\(\w/.test(l)) || '(top)';
      return (parent.trim().split(' ')[0] + ' > ' + lines[i].trim().split(' ')[0])
        .replace(/[()]/g, '');
    }
  }
  return '(no ERROR node in tree)';
}

// Identity of a run's finding set, independent of path/timing noise:
// sorted basenames across all three failure kinds, hashed. Two runs are
// deterministic iff this fingerprint matches, not merely their counts.
function fingerprint() {
  const names = [
    ...flagged.map((f) => path.basename(f)),
    ...runawayFiles.map((f) => path.basename(f)),
    ...oomFiles.map((f) => path.basename(f)),
  ].sort();
  return require('crypto').createHash('sha256').update(names.join('\n')).digest('hex');
}

if (flagged.length === 0 && runawayFiles.length === 0 && oomFiles.length === 0) {
  process.stdout.write(`no-error harness: ${files.length} inputs, 0 ERROR/MISSING, 0 runaway, 0 oom-suspect (${elapsed}ms)\n`);
  process.stdout.write(`fingerprint: ${fingerprint()}\n`);
  process.exit(0);
}

process.stdout.write(`no-error harness: ${files.length} inputs -> ${flagged.length} ERROR/MISSING, ${runawayFiles.length} runaway, ${oomFiles.length} oom-suspect (${elapsed}ms)\n`);
process.stdout.write(`fingerprint: ${fingerprint()}\n\n`);
for (const fp of runawayFiles.slice(0, MODE === 'gate' ? 10 : runawayFiles.length)) {
  process.stdout.write(`RUNAWAY ${path.basename(fp)}: ${JSON.stringify(fs.readFileSync(fp, 'utf8').slice(0, 120))}\n`);
}
for (const fp of oomFiles.slice(0, MODE === 'gate' ? 10 : oomFiles.length)) {
  process.stdout.write(`OOM_SUSPECT ${path.basename(fp)}: ${JSON.stringify(fs.readFileSync(fp, 'utf8').slice(0, 120))}\n`);
}
const classes = new Map();
const detail = MODE === 'discover' ? flagged : flagged.slice(0, 50);
for (const fp of detail) {
  const r = spawnBounded(['parse', fp], SINGLE_TIMEOUT_MS, 16 * 1024 * 1024);
  const sig = signatureOf(r.stdout || '');
  if (!classes.has(sig)) classes.set(sig, []);
  classes.get(sig).push(fp);
}
for (const [sig, items] of [...classes.entries()].sort((a, b) => b[1].length - a[1].length)) {
  process.stdout.write(`\n--- ${items.length}x  ${sig}\n`);
  for (const fp of items.slice(0, 4)) {
    process.stdout.write(`    ${path.basename(fp)}: ${JSON.stringify(fs.readFileSync(fp, 'utf8').slice(0, 100))}\n`);
  }
}
process.exit(1);
