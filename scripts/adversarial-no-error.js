#!/usr/bin/env node
// adversarial-no-error.js -- no-ERROR contract harness.
//
// Generates a deterministic adversarial input space (corpus seeds +
// mutations + hostile compositions + known-bad regressions), parses
// every input with the compiled parser via batched `tree-sitter parse
// -q` (the same binary scripts/run-corpus-tests.js drives), and
// reports every input whose tree contains ERROR or MISSING nodes or
// whose parse fails to terminate.
//
// Modes:
//   --discover  full report: failing inputs grouped by tree signature
//   --gate      CI gate: zero findings or exit 1; bounded wall clock
//
// Deterministic: fixed seed constants, no runtime randomness.
//
// spawnSync note: a runaway parse ignores the default SIGTERM kill;
// killSignal SIGKILL is required for the timeout to actually reap it.

'use strict';

const fs = require('fs');
const path = require('path');
const cp = require('child_process');

const MODE = process.argv.includes('--gate') ? 'gate' : 'discover';
const GATE_DEADLINE_MS = 60000;
const CHUNK = 200;
const CHUNK_TIMEOUT_MS = 15000;
const SINGLE_TIMEOUT_MS = 3000;
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
process.stderr.write(`no-error harness: ${seeds.length} corpus seeds -> ${space.size} unique inputs\n`);

const inputDir = path.join(workDir, 'inputs');
fs.rmSync(inputDir, { recursive: true, force: true });
fs.mkdirSync(inputDir, { recursive: true });
const files = [];
for (const [content, id] of space) {
  const fp = path.join(inputDir, `i${String(id).padStart(6, '0')}.org`);
  fs.writeFileSync(fp, content);
  files.push(fp);
}

// ---- batched parse --------------------------------------------------------
function parseQ(paths, timeoutMs) {
  return cp.spawnSync(tsBin, ['parse', '-q', ...paths], {
    cwd: repo,
    env: { ...process.env, TREE_SITTER_LIBDIR: libdir },
    encoding: 'utf8',
    maxBuffer: 64 * 1024 * 1024,
    timeout: timeoutMs,
    killSignal: 'SIGKILL',
  });
}

const startedAt = Date.now();
const flagged = [];
const killed = [];
for (let i = 0; i < files.length; i += CHUNK) {
  if (MODE === 'gate' && Date.now() - startedAt > GATE_DEADLINE_MS) {
    process.stderr.write(`error: no-error gate exceeded ${GATE_DEADLINE_MS}ms budget\n`);
    process.exit(1);
  }
  const chunk = files.slice(i, i + CHUNK);
  const r = parseQ(chunk, CHUNK_TIMEOUT_MS);
  if (!r.signal && r.status !== null) {
    for (const line of (r.stdout || '').split('\n')) {
      const m = line.match(/^(\S+\.org)\t/);
      if (m) flagged.push(m[1]);
    }
    continue;
  }
  // Chunk contained a runaway input: isolate per file.
  for (const fp of chunk) {
    const r1 = parseQ([fp], SINGLE_TIMEOUT_MS);
    if (r1.signal || r1.status === null) killed.push(fp);
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

if (flagged.length === 0 && killed.length === 0) {
  process.stdout.write(`no-error harness: ${files.length} inputs, 0 ERROR/MISSING, 0 runaway (${elapsed}ms)\n`);
  process.exit(0);
}

process.stdout.write(`no-error harness: ${files.length} inputs -> ${flagged.length} ERROR/MISSING, ${killed.length} runaway (${elapsed}ms)\n\n`);
for (const fp of killed.slice(0, MODE === 'gate' ? 10 : killed.length)) {
  process.stdout.write(`RUNAWAY ${path.basename(fp)}: ${JSON.stringify(fs.readFileSync(fp, 'utf8').slice(0, 120))}\n`);
}
const classes = new Map();
const detail = MODE === 'discover' ? flagged : flagged.slice(0, 50);
for (const fp of detail) {
  const r = cp.spawnSync(tsBin, ['parse', fp], {
    cwd: repo, env: { ...process.env, TREE_SITTER_LIBDIR: libdir },
    encoding: 'utf8', maxBuffer: 16 * 1024 * 1024,
    timeout: SINGLE_TIMEOUT_MS, killSignal: 'SIGKILL',
  });
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
