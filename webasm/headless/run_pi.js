#!/usr/bin/env node
// Headless one-shot: run one .pi file on the webasm runtime under node,
// with a fresh interpreter module (like a page load).
//
//   node headless/run_pi.js <file.pi>
//
// Prints one line:
//   rc=<rc> instantiate_ms=<N> boot_ms=<N> run_ms=<N>
// where
//   instantiate_ms  node + wasm instantiation + preload (one-time)
//   boot_ms         browser_boot = initialize_bprolog (one-time)
//   run_ms          the run itself (file write + browser_rerun)
// Output goes to node's stdout/stderr as usual; exit status 0 iff the
// run ended with status 1 (rc=1 means "ran to a successful end").
'use strict';
const fs = require('fs');
const path = require('path');
const ROOT = path.resolve(__dirname, '..');
const file = process.argv[2] ? path.resolve(process.argv[2]) : '';
if (!file || !fs.existsSync(file)) {
  console.error('usage: node headless/run_pi.js <file.pi>');
  process.exit(2);
}
process.chdir(path.join(ROOT, 'dist'));  // picat.data is resolved from CWD
const t0 = Date.now();
const PicatWasm = require(path.join(ROOT, 'dist', 'picat.js'));
PicatWasm({
  print: () => {},
  printErr: (s) => { process.stderr.write(s + '\n'); },
}).then((M) => {
  const instantiate_ms = Date.now() - t0;
  M.FS.writeFile('/user_code.pi',
    fs.readFileSync(file, 'utf8')
      .replace(/^\s*module\s+[A-Za-z_]\w*\s*\./m, 'module user_code.'));
  const b0 = Date.now();
  M.ccall('browser_boot', 'null', ['string'], ['/lib2']);
  const boot_ms = Date.now() - b0;
  const r0 = Date.now();
  let rc = -1, failed = false;
  try { rc = M.ccall('browser_rerun', 'number'); } catch (e) { failed = true; }
  const run_ms = Date.now() - r0;
  console.log('rc=' + rc + (failed ? ' threw' : '') +
              ' instantiate_ms=' + instantiate_ms +
              ' boot_ms=' + boot_ms +
              ' run_ms=' + run_ms);
  process.exit(rc === 1 && !failed ? 0 : 1);
}).catch((e) => { console.error('LOADFAIL ' + e); process.exit(1); });
