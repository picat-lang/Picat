#!/usr/bin/env node
// Headless two-shot: run one .pi file containing embedded "asp ... end"
// blocks (aspic) on the webasm runtime under node, mirroring the
// browser's two-stage flow:
//   stage 1: a wrapper module runs the aspic pre-transpiler (staged in
//            the wasm CWD, "/"), writing /user_code_final.pi and
//            /aspic_runtime.pi
//   stage 2: the generated program, in a FRESH interpreter module
//            (a fresh instance also re-creates the filesystem from the
//            preload image, so the stage-1 outputs are carried over)
//
//   node headless/run_asp_pi.js <file.pi>
//
// Prints one line:
//   rc=<rc> instantiate_ms=<N> boot_ms=<N> pre_ms=<N> run_ms=<N>
// Output goes to node's stdout/stderr as usual; exit status 0 iff the
// run ended with status 1 (rc=1 means "ran to a successful end").
'use strict';
const fs = require('fs');
const path = require('path');
const ROOT = path.resolve(__dirname, '..');
const file = process.argv[2] ? path.resolve(process.argv[2]) : '';
if (!file || !fs.existsSync(file)) {
  console.error('usage: node headless/run_asp_pi.js <file.pi>');
  process.exit(2);
}
const stage1 =
  'module user_code.\n' +
  'import aspic_prep.\n' +
  'main=>\n' +
  '  aspic_prep("/user_code_raw.pi","/","/user_code_final.pi","/aspic_runtime_template.pi","sat").\n';
function makeOpts() {
  return {
    print: () => {},
    printErr: (s) => { process.stderr.write(s + '\n'); },
  };
}
process.chdir(path.join(ROOT, 'dist'));  // picat.data is resolved from CWD
const t0 = Date.now();
require(path.join(ROOT, 'dist', 'picat.js'))(makeOpts()).then((M) => {
  const instantiate_ms = Date.now() - t0;
  const b0 = Date.now();
  M.ccall('browser_boot', 'null', ['string'], ['/lib2']);
  const boot_ms = Date.now() - b0;
  M.FS.writeFile('/user_code_raw.pi', fs.readFileSync(file, 'utf8'));
  M.FS.writeFile('/user_code.pi', stage1);
  let rc = -1, failed = false;
  const p0 = Date.now();
  try { rc = M.ccall('browser_rerun', 'number'); } catch (e) { failed = true; }
  const pre_ms = Date.now() - p0;
  if (failed || rc !== 1) {
    console.log('rc=' + rc + (failed ? ' threw' : '') +
                ' [stage 1: pre-translation failed] ' +
                'instantiate_ms=' + instantiate_ms +
                ' boot_ms=' + boot_ms + ' pre_ms=' + pre_ms);
    process.exit(1);
  }
  let fin = M.FS.readFile('/user_code_final.pi', {encoding: 'utf8'});
  fin = fin.replace(/^\s*module\s+[A-Za-z_]\w*\s*\./m, 'module user_code.');
  let runtime;
  try { runtime = M.FS.readFile('/aspic_runtime.pi', {encoding: 'utf8'}); }
  catch (e) { runtime = null; }
  // fresh interpreter for stage 2 (a fresh instance would also
  // re-create the FS, hence the explicit carry-over above).  A fresh
  // factory instance (node require cache cleared) is what makes the
  // second preload pass work cleanly.
  delete require.cache[require.resolve(path.join(ROOT, 'dist', 'picat.js'))];
  const picat2 = require(path.join(ROOT, 'dist', 'picat.js'));
  picat2(makeOpts()).then((M2) => {
    M2.ccall('browser_boot', 'null', ['string'], ['/lib2']);
    if (runtime) M2.FS.writeFile('/aspic_runtime.pi', runtime);
    M2.FS.writeFile('/user_code.pi', fin);
    const r0 = Date.now();
    rc = -1; failed = false;
    try { rc = M2.ccall('browser_rerun', 'number'); } catch (e) { failed = true; }
    const run_ms = Date.now() - r0;
    console.log('rc=' + rc + (failed ? ' threw' : '') +
                ' instantiate_ms=' + instantiate_ms +
                ' boot_ms=' + boot_ms +
                ' pre_ms=' + pre_ms + ' run_ms=' + run_ms);
    process.exit(rc === 1 && !failed ? 0 : 1);
  }).catch((e) => { console.error('LOADFAIL ' + e); process.exit(1); });
}).catch((e) => { console.error('LOADFAIL ' + e); process.exit(1); });
