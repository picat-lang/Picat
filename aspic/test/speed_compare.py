#!/usr/bin/env python3
"""Time clingo vs aspic on the clingo repo examples and the book programs.

For every program both tools can parse, the same pipeline the comparison
harnesses use is timed:
  clingo - in-process (python module): load + ground + solve (all models
           for the examples; the optimal solve for the book programs).
  aspic  - two subprocesses: the transpiler (picat aspic_transpiler.pi run
           FILE) and the generated program (picat -log -s 1234567890 PI).
One warmup run first (the transpiler compiles its modules once, writing
.qi bytecode the following runs load), then 3 timed runs, medians
reported.  Programs aspic cannot parse are listed with their clingo time
only; the counts match the comparison harnesses.

Usage: python3 speed_compare.py [clingo|book|all]     (default all)
Env:   WORK (scratch dir for the combined files; default under the
       workspace scratch, NOT /tmp), ASPIC_DIR, CLINGO_EX as in
       clingo_examples.py.  Requires: clingo python module, picat on PATH.
"""
import os
import re
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
PWC = "/home/jovyan/snapshotable/aspic/pwc-programs"
sys.path.insert(0, PWC)

import clingo
import clingo_examples as CE
import pwc_aspic_test as PW

ASPIC = CE.ASPIC
ASPIC_DIR = CE.ASPIC_DIR
WORK = os.environ.get("WORK", "/home/jovyan/snapshotable/aspic/scratch/speedcmp")
RUNS = 3
os.makedirs(WORK, exist_ok=True)
os.makedirs(PW.WORK, exist_ok=True)

# the comparison harnesses disable the satext portfolio for the aspic runs
ENV = dict(os.environ, SATEXT_SOLVER="")


def median_ms(fn, runs=RUNS):
    """Run fn() runs+1 times (run 0 is the warmup), return the median ms."""
    times = []
    for i in range(runs + 1):
        t0 = time.perf_counter()
        fn()
        t1 = time.perf_counter()
        if i > 0:
            times.append((t1 - t0) * 1000.0)
    return statistics.median(times)


def clingo_time(files, consts=None, opt=False):
    """Median ms of clingo load+ground+solve, fresh Control per run.
    Returns None if clingo cannot load/ground the program."""
    cargs = PW.cargs(consts or {}, ["--opt-mode=opt"] if opt else [])
    fail = []
    times = []

    def run():
        if fail:
            return
        ctl = clingo.Control(cargs)
        try:
            for f in files:
                ctl.load(f)
            ctl.ground([("base", [])])
        except Exception as e:
            fail.append(str(e).splitlines()[0][:60])
            return
        with ctl.solve(yield_=True) as h:
            for m in h:
                pass

    for i in range(RUNS + 1):
        t0 = time.perf_counter()
        run()
        t1 = time.perf_counter()
        if i > 0 and not fail:
            times.append((t1 - t0) * 1000.0)
    if fail:
        return None
    return statistics.median(times)


def aspic_times(files, name, consts=None):
    """(transpile_ms, solve_ms) medians, or (None, reason) if aspic fails.

    The transpile and solve are timed as separate subprocesses; the
    generated program is extracted from the transpiler's stderr exactly
    as the harnesses do.
    """
    combined = os.path.join(WORK, name + ".lp")
    with open(combined, "w") as out:
        out.write("".join(f"#const {k}={v}.\n" for k, v in (consts or {}).items()))
        for f in files:
            if f.startswith(PW.WORK):
                out.write(open(f).read() + "\n")
            elif os.path.isabs(f):
                out.write(open(f).read() + "\n")
            else:
                out.write(open(os.path.join(PW.HERE, f)).read() + "\n")

    def transpile():
        with open(os.path.join(WORK, name + ".gen.txt"), "w") as g:
            subprocess.run([ASPIC, "-log", "aspic_transpiler.pi", "run", combined],
                           cwd=ASPIC_DIR, stdout=subprocess.PIPE, stderr=g,
                           timeout=60, env=ENV)

    def solve():
        subprocess.run([ASPIC, "-log", "-s", "1234567890",
                        os.path.join(WORK, name + ".pi")],
                       cwd=ASPIC_DIR, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=60, env=ENV)

    # probe run: transpile once, check parseability, extract the program
    transpile()
    err = open(os.path.join(WORK, name + ".gen.txt")).read()
    out = subprocess.run([ASPIC, "-log", "aspic_transpiler.pi", "run", combined],
                         cwd=ASPIC_DIR, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, timeout=60, env=ENV).stdout.decode()
    if "notparsed = []" not in out:
        return None, "not parseable"
    if "error(" in err:
        return None, "aspic error"
    m = re.search(r"(?m)^import (sat|cp)\.", err)
    if not m:
        return None, "no generated program"
    with open(os.path.join(WORK, name + ".pi"), "w") as f:
        f.write(err[m.start():])
    t_tr = median_ms(transpile)
    t_so = median_ms(solve)
    return t_tr, t_so


def bench(name, files, consts=None, opt=False, is_book=False):
    """One row: (name, clingo_ms, transpile_ms, solve_ms) or an error tag."""
    ct = clingo_time(files, consts, opt)
    if ct is None:
        return (name, 0.0, None, None, "clingo cannot load")
    if is_book:
        at = aspic_times(files, "book_" + name, consts)
    else:
        at = aspic_times(files, name, consts)
    if at[0] is None:
        return (name, ct, None, None, at[1])
    return (name, ct, at[0], at[1], None)


def rows_clingo_examples():
    for name, files in CE.programs():
        yield bench(name, files, None, False, False)


def rows_book():
    for name, files, consts, kind in PW.PROGS:
        paths = [f if f.startswith(PW.WORK) else os.path.join(PW.HERE, f) for f in files]
        yield bench(name, paths, consts, bool(kind), True)


def report(title, rows):
    print(f"\n== {title} ==")
    print(f"{'program':38s} {'clingo':>9s} {'transp':>9s} {'solve':>9s} "
          f"{'aspic':>9s} {'ratio':>6s}")
    both = []
    only = []
    for name, ct, tr, so, err in rows:
        if tr is None:
            only.append(name)
            print(f"{name[:37]:38s} {ct:9.1f} {'-':>9s} {'-':>9s} "
                  f"{'-':>9s}  ({err})")
            continue
        at = tr + so
        both.append((name, ct, at))
        print(f"{name[:37]:38s} {ct:9.1f} {tr:9.1f} {so:9.1f} "
              f"{at:9.1f} {at / ct:6.1f}")
    if both:
        tot_c = sum(r[1] for r in both)
        tot_a = sum(r[2] for r in both)
        ratios = [r[2] / r[1] for r in both if r[1] > 0]
        print(f"{'TOTAL (both handle):':38s} {tot_c:9.1f} {'':9s} {'':9s} "
              f"{tot_a:9.1f} {tot_a / tot_c:6.1f}")
        print(f"median ratio: {statistics.median(ratios):.1f}x over "
              f"{len(both)} programs; geomean: "
              f"{statistics.geometric_mean(ratios):.1f}x")
    if only:
        print(f"aspic cannot parse {len(only)}: {', '.join(only[:6])}"
              + (" ..." if len(only) > 6 else ""))


def main():
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    if which in ("clingo", "all"):
        report("clingo repo examples", list(rows_clingo_examples()))
    if which in ("book", "all"):
        report("book programs (pwc corpus)", list(rows_book()))


if __name__ == "__main__":
    main()
