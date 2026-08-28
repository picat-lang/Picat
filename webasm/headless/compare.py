#!/usr/bin/env python3
"""Native vs webasm runtime comparison over all packed wasm examples.

For every example in webasm/examples/ this measures, on this machine:

  native_ms        wall time of one `emu/picat <example>` process
  wasm_run_ms      wall time of the run itself on the webasm runtime
                   (a fresh module per run, like a page load; this is the
                   browser_rerun call, i.e. the program)
  wasm_boot_ms     one-time cost: wasm instantiation + preload +
                   browser_boot (initialize_bprolog)

and the ratio wasm_run_ms / native_ms (1.00 = equal speed).
Measurements are taken with up to --workers jobs at a time (default 32,
the cap used for the numbers in the README); each (example, runtime)
pair is measured --runs times (default 1) and the median is kept.

Two examples need special legs.  Embedded-ASP examples (aspic) run in
two stages and have no one-shot native equivalent: their wasm leg uses
run_asp_pi.js (wasm run time = stage-1 transpilation + stage-2 run)
and the native leg is skipped (n/a).  nn_lang_train.pi is measured
after the parallel pool, because other examples prepare files it
needs.

Outputs: a markdown table on stdout and headless/results.csv.

Usage:  python3 headless/compare.py [--runs N] [--workers N]
"""

import argparse
import csv
import math
import os
import re
import statistics
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
NATIVE = os.path.join(ROOT, 'emu', 'picat')
EXAMPLES = os.path.join(ROOT, 'webasm', 'examples')
RUN_PI = os.path.join(HERE, 'run_pi.js')
RUN_ASPPI = os.path.join(HERE, 'run_asp_pi.js')
DIST = os.path.join(ROOT, 'webasm', 'dist')
NATIVE_CWD = None  # set in main()

# Measured after the parallel pool: other examples prepare files this
# example needs.
RUN_LAST = {'nn_lang_train.pi'}

# Packed examples whose NATIVE leg is measured against an equivalent
# source file. The browser's example list shows /examples, so the
# udf aux files (preloaded into the wasm FS at build time) are not
# shipped in webasm/examples/ and the packed file's
# `include "udf_ops.pi"` (resolved against the program's directory)
# cannot resolve in a native run. x_udf_test.pi is code-identical to
# exs/cp/udf_test.pi, which runs natively with the udf library from
# lib2/.
NATIVE_EQUIV = {
    'x_udf_test.pi': os.path.join(ROOT, 'exs', 'cp', 'udf_test.pi'),
}


def preload_sources():
    """The DATA_SRC entries of webasm/Makefile (paths relative to webasm/)."""
    out, inblock = [], False
    makefile = os.path.join(HERE, os.pardir, 'Makefile')
    for raw in open(makefile):
        line = raw.rstrip('\n')
        s = line.strip()
        if s.startswith('DATA_SRC'):
            inblock = True
            s = s.split(':=', 1)[1]
        elif not inblock:
            continue
        s = s.replace('\\', ' ').strip()
        for t in s.split():
            if t.startswith('#'):
                break
            out.append(t)
        if inblock and not line.rstrip().endswith('\\'):
            inblock = False
    return out


def make_native_cwd():
    """A directory mirroring the wasm root for native runs.

    The wasm runtime preloads every DATA_SRC file at the root of its
    emulated filesystem and the wasm CWD is "/", so the examples open
    their data files by bare name.  A native run must therefore happen
    in a directory containing those same files; symlinks keep it
    updated with the Makefile."""
    d = os.path.join(HERE, 'native_cwd')
    webasm = os.path.abspath(os.path.join(HERE, os.pardir))
    os.makedirs(d, exist_ok=True)
    for s in preload_sources():
        target = os.path.join(webasm, s)
        link = os.path.join(d, os.path.basename(s))
        if os.path.exists(target) and not os.path.lexists(link):
            os.symlink(target, link)
    return d


def native_once(path):
    """One native run: returns (rc, ms)."""
    env = dict(os.environ, PICATPATH=os.path.join(ROOT, 'lib2'))
    t0 = time.monotonic()
    p = subprocess.run([NATIVE, path], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, env=env, cwd=NATIVE_CWD)
    ms = int((time.monotonic() - t0) * 1000)
    return p.returncode, max(ms, 1)


def is_asp(path):
    """True if the example embeds an ASP block (transpiled at run time
    by aspic).  Those run in two stages via run_asp_pi.js and have no
    one-shot native equivalent, so native is skipped for them."""
    with open(path, encoding='utf8', errors='replace') as f:
        return bool(re.search(r'(?<![A-Za-z0-9_])asp[ \t]*\n', f.read()))


def wasm_once(path, asp=False):
    """One wasm run: returns (rc, run_ms, boot_ms, instantiate_ms).
    For two-stage ASP examples run_ms is stage 1 (transpilation) +
    stage 2 (the run)."""
    t = subprocess.run(['node', RUN_ASPPI if asp else RUN_PI, path],
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                       cwd=DIST, timeout=1800)
    line = t.stdout.decode('utf8', 'replace').strip()
    m = re.search(r'rc=([0-9_-]+)(\s+threw)?\s+instantiate_ms=(\d+)\s+'
                  r'boot_ms=(\d+)(\s+pre_ms=(\d+))?\s+run_ms=(\d+)', line)
    if not m:
        sys.stderr.write('bad wasm output for %s: %r\n' % (path, line[:200]))
        return -1, 0, 0, 0
    run_ms = int(m.group(7)) + (int(m.group(6)) if m.group(6) else 0)
    return int(m.group(1)), run_ms, int(m.group(4)), int(m.group(3))


def measure(example, runs):
    """All measurements for one example (native first, then wasm, each
    `runs` times, medians kept).  Runs sequentially, so with the outer
    pool at W workers at most W OS processes are running at once."""
    path = os.path.join(EXAMPLES, example)
    asp = is_asp(path)
    was = [wasm_once(path, asp) for _ in range(runs)]
    med = lambda xs: statistics.median(xs) if xs else 0
    res = {
        'wasm_ok': all(r == 1 for r, *_ in was),
        'wasm_run_ms': med([ms for _, ms, _, _ in was]),
        'wasm_boot_ms': med([ms for _, _, ms, _ in was]),
        'wasm_instantiate_ms': med([ms for _, _, _, ms in was]),
    }
    if asp:
        res.update(native_ran=False, native_ok=None, native_ms=0)
        return res
    nat_path = NATIVE_EQUIV.get(example, path)
    nat = [native_once(nat_path) for _ in range(runs)]
    res['native_ran'] = True
    res['native_ok'] = all(rc == 0 for rc, _ in nat)
    res['native_ms'] = med([ms for _, ms in nat])
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--runs', type=int, default=1)
    ap.add_argument('--workers', type=int, default=32)
    ap.add_argument('--limit', type=int, default=0,
                    help='only the first N examples (debugging)')
    ap.add_argument('--out', default=os.path.join(HERE, 'results.csv'))
    args = ap.parse_args()

    examples = sorted(f for f in os.listdir(EXAMPLES) if f.endswith('.pi'))
    if args.limit:
        examples = examples[:args.limit]
    if not os.access(NATIVE, os.X_OK):
        sys.exit('missing native %s (build it first)' % NATIVE)
    if not os.path.isfile(os.path.join(DIST, 'picat.js')):
        sys.exit('missing %s (cd webasm && make first)' % DIST)
    global NATIVE_CWD
    NATIVE_CWD = make_native_cwd()

    first = [e for e in examples if e not in RUN_LAST]
    last = [e for e in examples if e in RUN_LAST]
    t0 = time.monotonic()
    results = {}
    with ThreadPoolExecutor(max_workers=args.workers) as ex:
        futs = {ex.submit(measure, e, args.runs): e for e in first}
        for fut in sorted(futs, key=lambda f: futs[f]):
            results[futs[fut]] = fut.result()
    # after the pool: these examples need files prepared by others
    for e in last:
        results[e] = measure(e, args.runs)
    wall_s = time.monotonic() - t0

    rows = []
    for e in examples:
        r = results[e]
        r['example'] = e
        r['ratio'] = (r['wasm_run_ms'] / max(r['native_ms'], 1)
                      if r['native_ran'] else None)
        rows.append(r)

    def legs_ok(r):
        return r['wasm_ok'] and r['native_ok'] is not False

    ok = [r for r in rows if legs_ok(r)]

    with open(args.out, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['example', 'native_ms', 'wasm_run_ms', 'wasm_boot_ms',
                    'wasm_instantiate_ms', 'ratio', 'native_ok', 'wasm_ok'])
        for r in rows:
            w.writerow([r['example'],
                        r['native_ms'] if r['native_ran'] else 'na',
                        r['wasm_run_ms'], r['wasm_boot_ms'],
                        r['wasm_instantiate_ms'],
                        '' if r['ratio'] is None else '%.2f' % r['ratio'],
                        'na' if not r['native_ran'] else r['native_ok'],
                        r['wasm_ok']])

    print('| example | native ms | wasm run ms | wasm boot (ms) | ratio |')
    print('| --- | ---: | ---: | ---: | ---: |')
    for r in rows:
        flag = '' if legs_ok(r) else ' *'
        nat = 'n/a' if not r['native_ran'] else '%d' % r['native_ms']
        ratio = '-' if r['ratio'] is None else '%.2f' % r['ratio']
        print('| %s%s | %s | %d | %d | %s |' % (
            r['example'].replace('.pi', ''), flag, nat,
            r['wasm_run_ms'], r['wasm_boot_ms'], ratio))
    nbad = len(rows) - len(ok)
    if nbad:
        print('* = run failed on at least one runtime (excluded from the '
              'statistics)')
    print()

    cmp_rows = [r for r in ok if r['ratio'] is not None]
    ratios = [r['ratio'] for r in cmp_rows]
    geo = (math.exp(sum(math.log(x) for x in ratios) / len(ratios))
           if ratios else 0.0)
    faster = [r for r in cmp_rows if r['ratio'] < 1.0]
    slowest = max(cmp_rows, key=lambda r: r['ratio'])
    fastest = min(cmp_rows, key=lambda r: r['ratio'])
    boot = [r['wasm_boot_ms'] for r in rows]
    inst = [r['wasm_instantiate_ms'] for r in rows]

    print('\n== summary ==')
    print('median one-time wasm start: instantiate %d ms + boot %d ms' % (
        statistics.median(inst), statistics.median(boot)))
    if ratios:
        print('geomean ratio (wasm/native): %.2f  (median %.2f)' % (
            geo, statistics.median(ratios)))
        print('wasm faster: %d of %d' % (len(faster), len(cmp_rows)))
        for r in sorted(faster, key=lambda r: r['ratio'])[:5]:
            print('  %s  %.2f (%d vs %d ms)' % (
                r['example'], r['ratio'], r['wasm_run_ms'], r['native_ms']))
        print('most extreme slowdown: %s  %.1fx (%d vs %d ms)' % (
            slowest['example'], slowest['ratio'], slowest['wasm_run_ms'],
            slowest['native_ms']))
        print('best wasm/native: %s  %.2f' % (
            fastest['example'], fastest['ratio']))
    print('failed rows: %s' % (
        ', '.join(r['example'] for r in rows if not legs_ok(r)) or 'none'))
    print('suite wall: %.0f s (workers %d, runs %d)' % (
        wall_s, args.workers, args.runs))


if __name__ == '__main__':
    main()
