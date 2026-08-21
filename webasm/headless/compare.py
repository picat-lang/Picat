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
DIST = os.path.join(ROOT, 'webasm', 'dist')
NATIVE_CWD = None  # set in main()


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


def wasm_once(path):
    """One wasm run: returns (rc, run_ms, boot_ms, instantiate_ms)."""
    t = subprocess.run(['node', RUN_PI, path],
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                       cwd=DIST, timeout=1800)
    line = t.stdout.decode('utf8', 'replace').strip()
    m = re.search(r'rc=([0-9_-]+)(\s+threw)?\s+instantiate_ms=(\d+)\s+'
                  r'boot_ms=(\d+)\s+run_ms=(\d+)', line)
    if not m:
        sys.stderr.write('bad wasm output for %s: %r\n' % (path, line[:200]))
        return -1, 0, 0, 0
    return int(m.group(1)), int(m.group(5)), int(m.group(4)), int(m.group(3))


def measure(example, runs):
    """All measurements for one example (native first, then wasm, each
    `runs` times, medians kept).  Runs sequentially, so with the outer
    pool at W workers at most W OS processes are running at once."""
    path = os.path.join(EXAMPLES, example)
    nat = [native_once(path) for _ in range(runs)]
    was = [wasm_once(path) for _ in range(runs)]
    med = lambda xs: statistics.median(xs) if xs else 0
    return {
        'native_ok': all(rc == 0 for rc, _ in nat),
        'native_ms': med([ms for _, ms in nat]),
        'wasm_ok': all(r == 1 for r, *_ in was),
        'wasm_run_ms': med([ms for _, ms, _, _ in was]),
        'wasm_boot_ms': med([ms for _, _, ms, _ in was]),
        'wasm_instantiate_ms': med([ms for _, _, _, ms in was]),
    }


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

    t0 = time.monotonic()
    results = {}
    with ThreadPoolExecutor(max_workers=args.workers) as ex:
        futs = {ex.submit(measure, e, args.runs): e for e in examples}
        for fut in sorted(futs, key=lambda f: futs[f]):
            results[futs[fut]] = fut.result()
    wall_s = time.monotonic() - t0

    rows = []
    for e in examples:
        r = results[e]
        r['example'] = e
        r['ratio'] = r['wasm_run_ms'] / max(r['native_ms'], 1)
        rows.append(r)

    ok = [r for r in rows if r['native_ok'] and r['wasm_ok']]

    with open(args.out, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['example', 'native_ms', 'wasm_run_ms', 'wasm_boot_ms',
                    'wasm_instantiate_ms', 'ratio', 'native_ok', 'wasm_ok'])
        for r in rows:
            w.writerow([r['example'], r['native_ms'], r['wasm_run_ms'],
                        r['wasm_boot_ms'], r['wasm_instantiate_ms'],
                        '%.2f' % r['ratio'], r['native_ok'], r['wasm_ok']])

    print('| example | native ms | wasm run ms | wasm boot (ms) | ratio |')
    print('| --- | ---: | ---: | ---: | ---: |')
    for r in rows:
        flag = '' if (r['native_ok'] and r['wasm_ok']) else ' *'
        print('| %s%s | %d | %d | %d | %.2f |' % (
            r['example'].replace('.pi', ''), flag, r['native_ms'],
            r['wasm_run_ms'], r['wasm_boot_ms'], r['ratio']))
    nbad = len(rows) - len(ok)
    if nbad:
        print('* = run failed on at least one runtime (excluded from the '
              'statistics)')
    print()

    ratios = [r['ratio'] for r in ok]
    geo = math.exp(sum(math.log(x) for x in ratios) / len(ratios))
    faster = [r for r in ok if r['ratio'] < 1.0]
    slowest = max(ok, key=lambda r: r['ratio'])
    fastest = min(ok, key=lambda r: r['ratio'])
    boot = [r['wasm_boot_ms'] for r in rows]
    inst = [r['wasm_instantiate_ms'] for r in rows]

    print('\n== summary ==')
    print('median one-time wasm start: instantiate %d ms + boot %d ms' % (
        statistics.median(inst), statistics.median(boot)))
    print('geomean ratio (wasm/native): %.2f  (median %.2f)' % (
        geo, statistics.median(ratios)))
    print('wasm faster: %d of %d' % (len(faster), len(ok)))
    for r in sorted(faster, key=lambda r: r['ratio'])[:5]:
        print('  %s  %.2f (%d vs %d ms)' % (
            r['example'], r['ratio'], r['wasm_run_ms'], r['native_ms']))
    print('most extreme slowdown: %s  %.1fx (%d vs %d ms)' % (
        slowest['example'], slowest['ratio'], slowest['wasm_run_ms'],
        slowest['native_ms']))
    print('best wasm/native: %s  %.2f' % (fastest['example'], fastest['ratio']))
    print('failed rows: %s' % (
        ', '.join(r['example'] for r in rows
                  if not (r['native_ok'] and r['wasm_ok'])) or 'none'))
    print('suite wall: %.0f s (workers %d, runs %d)' % (
        wall_s, args.workers, args.runs))


if __name__ == '__main__':
    main()
