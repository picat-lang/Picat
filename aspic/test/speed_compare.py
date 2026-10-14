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

Usage: python3 speed_compare.py [clingo|book|all|scaled]   (default all)
Env:   WORK (scratch dir for the combined files; default under the
       workspace scratch, NOT /tmp), ASPIC_DIR, CLINGO_EX as in
       clingo_examples.py.  Requires: clingo python module + the clingo
       binary on PATH (the scaled mode uses the CLI for a hard per-run
       timeout), picat on PATH.

The scaled mode: parameterized encodings (queens, graph coloring,
independent sets, subset-sum, magic rows, partition) run over a ladder
of generated instances (seeded), one run per side, a hard 10 s cap per
run (the clingo CLI subprocess timeout, the aspic subprocess timeout);
a family stops scaling when either tool exceeds the cap.  The search
(not the ~120 ms per-invocation overhead) dominates the times at these
sizes.
"""
import os
import random
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
    if which in ("scaled", "all"):
        run_scaled()


# ---- scaled mode -----------------------------------------------------------
# Parameterized encodings over generated instance ladders; one run per
# side; a hard 10 s cap per run.  A family stops scaling when either tool
# exceeds the cap.

SCALED_TIMEOUT = 10
CAP = SCALED_TIMEOUT * 1000.0


def gen_graph(v, e, seed=42):
    """A seeded random undirected graph: vertex/1 + edge/2 facts."""
    rng = random.Random(seed)
    edges = set()
    while len(edges) < e:
        a, b = rng.sample(range(1, v + 1), 2)
        edges.add((min(a, b), max(a, b)))
    txt = "".join(f"vertex({i}).\n" for i in range(1, v + 1))
    txt += "".join(f"edge({a},{b}).\n" for a, b in sorted(edges))
    return txt


def gen_numbers(n, seed=42):
    """N distinct numbers 1..999 and the half-sum bound for subset-sum."""
    rng = random.Random(seed)
    nums = sorted(rng.sample(range(1, 1000), n))
    txt = "".join(f"number({x}).\n" for x in nums)
    return txt, sum(nums) // 2


def sub_const(text, name, value):
    """Replace the #const NAME = <int>. directive in an encoding text."""
    return re.sub(rf"#const\s+{name}\s*=\s*\d+\s*\.", f"#const {name}={value}.",
                  text)


def drop_facts(text, prefixes):
    """Drop the lines whose first word starts with one of prefixes (the
    instance facts to regenerate)."""
    lines = [l for l in text.splitlines()
             if not l.lstrip().startswith(tuple(prefixes))]
    return "\n".join(lines) + "\n"


def scaled_entries():
    """(family, name, parts, consts, opt) ladder entries.

    parts: a file path, or ("text", content) written inline.  The graph
    edge counts sit near the 3-coloring phase transition (m/v ~ 2.35);
    the subset-sum bound is half the total (a balanced partition, NP-hard);
    the shipped examples' #const parameters are substituted in the
    encoding text (sub_const) and their inline instance facts are dropped
    and regenerated (drop_facts).  vl_schur's shipped n=45 is past
    Schur's S(4)=44 (clingo >10s proving UNSAT), so it is scaled at SAT
    sizes n=30/40.
    """
    pw = lambda f: os.path.join(PW.HERE, "aspic", f)
    ex = lambda f: os.path.join(ASPIC_DIR, "examples", f)
    colors = "color(red). color(green). color(blue).\n"
    entries = []
    for n in (8, 12, 16, 20, 24):
        entries.append(("queens", f"queens_n{n}", [pw("queens.lp")],
                        {"n": n}, False))
    for v, e in ((30, 70), (60, 141), (90, 212)):
        entries.append(("coloring", f"coloring_v{v}", 
                        [("text", colors), ("text", gen_graph(v, e)),
                         pw("coloring.lp")], {}, False))
    for v, e, k in ((30, 70, 12), (30, 70, 16)):
        entries.append(("indep", f"indep_v{v}_k{k}",
                        [("text", gen_graph(v, e)),
                         pw("listing07_independent_sets.lp")],
                        {"n": k}, False))
    for n in (20, 30, 40):
        nums, m = gen_numbers(n)
        entries.append(("subsetsum", f"subsetsum_n{n}",
                        [("text", nums), pw("listing12_subset_sum.lp")],
                        {"m": m}, True))
    for n in (3, 4):
        l10 = open(pw("listing10_magic_squares.lp")).read()
        l10 = l10.replace("magic=15", f"magic={(n ** 3 + n) // 2}")
        entries.append(("magicrow", f"magicrow_n{n}",
                        [("text", l10), pw("p6_3b_magic_row.lp")],
                        {"n": n}, False))
    for n in (50, 500, 5000):
        entries.append(("partition", f"partition_n{n}", [pw("partition_n.lp")],
                        {"n": n}, False))
    # shipped examples/ families
    for n in (32, 64, 128, 256):
        q5 = sub_const(open(ex("queens5.lp")).read(), "n", n)
        entries.append(("queens5", f"queens5_n{n}", [("text", q5)], {}, False))
    for n in (16, 24, 32, 48):
        hq = sub_const(open(ex("hk_queens.lp")).read(), "n", n)
        entries.append(("hkqueens", f"hkqueens_n{n}", [("text", hq)], {}, False))
    for n in (10, 20, 30):
        rng = random.Random(42 + n)
        items = [(i, rng.randint(1, 20), rng.randint(1, 50))
                 for i in range(1, n + 1)]
        cap = sum(w for _, w, _ in items) // 2
        txt = "".join(f"item({i},{w},{v}).\n" for i, w, v in items)
        txt += f"capacity({cap}).\n"
        bp = drop_facts(open(ex("backpack.lp")).read(), ("item(", "capacity("))
        entries.append(("backpack", f"backpack_n{n}",
                        [("text", txt), ("text", bp)], {}, True))
    for v, e in ((30, 70), (60, 141), (90, 212)):
        gc = drop_facts(open(ex("graph_coloring.lp")).read(), ("node(", "edge("))
        entries.append(("gcexample", f"gcexample_v{v}",
                        [("text", gen_graph(v, e)), ("text", gc)], {}, False))
    for n in (30, 40):
        vs = sub_const(open(ex("vl_schur.lp")).read(), "n", n)
        entries.append(("schur", f"schur_n{n}", [("text", vs)], {}, False))
    return entries


def write_instance(name, parts, consts):
    combined = os.path.join(WORK, "scaled_" + name + ".lp")
    with open(combined, "w") as out:
        out.write("".join(f"#const {k}={v}.\n" for k, v in consts.items()))
        for p in parts:
            out.write(p[1] if isinstance(p, tuple) else open(p).read())
            out.write("\n")
    return combined


def clingo_cli_time(combined, opt, timeout=SCALED_TIMEOUT):
    """(ms, err) of one clingo CLI run, hard timeout."""
    args = ["clingo"] + (["--opt-mode=opt"] if opt else []) + [combined]
    t0 = time.perf_counter()
    try:
        r = subprocess.run(args, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, f">{timeout}s"
    t1 = time.perf_counter()
    if r.returncode not in (0, 10, 20, 30):
        return None, f"clingo exit {r.returncode}"
    return (t1 - t0) * 1000.0, None


def aspic_scaled_times(combined, name, timeout=SCALED_TIMEOUT):
    """(transpile_ms, solve_ms, clingo_value, aspic_value, err), one run
    per side, hard timeout.  clingo_value/aspic_value: the optimization
    costs for the opt programs (None otherwise), compared for validity."""
    gen = os.path.join(WORK, "scaled_" + name + ".gen.txt")

    def transpile():
        with open(gen, "w") as g:
            subprocess.run([ASPIC, "-log", "aspic_transpiler.pi", "run", combined],
                           cwd=ASPIC_DIR, stdout=subprocess.PIPE, stderr=g,
                           timeout=timeout, env=ENV)

    t0 = time.perf_counter()
    try:
        transpile()
    except subprocess.TimeoutExpired:
        return None, None, None, None, f"transpile >{timeout}s"
    t1 = time.perf_counter()
    tr = (t1 - t0) * 1000.0
    err = open(gen).read()
    out = subprocess.run([ASPIC, "-log", "aspic_transpiler.pi", "run", combined],
                         cwd=ASPIC_DIR, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, timeout=timeout,
                         env=ENV).stdout.decode()
    if "notparsed = []" not in out:
        return None, None, None, None, "not parseable"
    if "error(" in err:
        return None, None, None, None, "aspic error"
    m = re.search(r"(?m)^import (sat|cp)\.", err)
    if not m:
        return None, None, None, None, "no generated program"
    pi = os.path.join(WORK, "scaled_" + name + ".pi")
    with open(pi, "w") as f:
        f.write(err[m.start():])
    run_txt = os.path.join(WORK, "scaled_" + name + ".run.txt")
    run_txt_p = os.path.join(WORK, "scaled_" + name + ".runp.txt")

    def solve_run(txt_path, env):
        t0 = time.perf_counter()
        try:
            with open(txt_path, "w") as f:
                subprocess.run([ASPIC, "-log", "-s", "1234567890", pi],
                               cwd=ASPIC_DIR, stdout=f,
                               stderr=subprocess.STDOUT,
                               timeout=timeout, env=env)
        except subprocess.TimeoutExpired:
            return None
        t1 = time.perf_counter()
        return (t1 - t0) * 1000.0

    def opt_val(txt_path):
        try:
            text = open(txt_path).read()
        except OSError:
            return None
        mv = re.search(r"maximization\((-?\d+)\)", text)
        ov = re.search(r"optimization\((-?\d+)\)", text)
        return int(mv.group(1)) if mv else (int(ov.group(1)) if ov else None)

    so = solve_run(run_txt, ENV)          # internal sat solver (portfolio off)
    sop = solve_run(run_txt_p, None)      # the satext portfolio (inherited env)
    aval = opt_val(run_txt)
    if so is None and sop is None:
        return tr, None, None, None, f"both solves >{timeout}s"
    return tr, so, sop, aval, None


def run_scaled():
    print(f"\n== scaled instances (one run per side, {SCALED_TIMEOUT}s cap) ==")
    print(f"{'instance':22s} {'clingo':>9s} {'transp':>9s} {'solve':>9s} "
          f"{'aspic':>9s} {'ratio':>7s} {'portf':>9s} {'r.portf':>8s}  check")
    stopped = set()
    rows = []
    for family, name, parts, consts, opt in scaled_entries():
        if family in stopped:
            continue
        combined = write_instance(name, parts, consts)
        ct, cerr = clingo_cli_time(combined, opt)
        if cerr:
            print(f"{name:22s} {'-':>9s} {'-':>9s} {'-':>9s} {'-':>9s} "
                  f"{'-':>9s} {'-':>7s}  ({cerr})")
            if ">" in cerr:
                stopped.add(family)
            continue
        tr, so, sop, aval, aerr = aspic_scaled_times(combined, name)
        if aerr:
            print(f"{name:22s} {ct:9.1f} {'-':>9s} {'-':>9s} {'-':>9s} "
                  f"{'-':>7s} {'-':>9s} {'-':>8s}  ({aerr})")
            if aerr.startswith(">"):
                stopped.add(family)
            continue
        at = tr + (so if so is not None else 0)
        apt = tr + (sop if sop is not None else 0)
        check = ""
        if opt:
            # clingo's optimum from the same CLI run (one more solve)
            cv = clingo_opt_value(combined)
            check = "opt match" if (cv is not None and aval is not None
                                    and cv == -aval) else f"clingo={cv} aspic={aval}"
        so_s = f"{so:9.1f}" if so is not None else f"{'>'+str(SCALED_TIMEOUT)+'s':>9s}"
        sop_s = f"{sop:9.1f}" if sop is not None else f"{'>'+str(SCALED_TIMEOUT)+'s':>9s}"
        at_s = f"{at / ct:6.1f}x" if so is not None else f"{'-':>7s}"
        apt_s = f"{apt / ct:6.1f}x" if sop is not None else f"{'-':>8s}"
        rows.append((family, name, ct, at, apt))
        print(f"{name:22s} {ct:9.1f} {tr:9.1f} {so_s} {at:9.1f} "
              f"{at_s} {sop_s} {apt_s}  {check}")
        if at > CAP:
            stopped.add(family)
    if rows:
        tot_c = sum(r[2] for r in rows)
        tot_a = sum(r[3] for r in rows)
        tot_p = sum(r[4] for r in rows)
        print(f"{'TOTAL (capped runs):':22s} {tot_c:9.1f} {'':9s} {'':9s} "
              f"{tot_a:9.1f} {tot_a / tot_c:6.1f}x "
              f"{'':9s} {tot_p / tot_c:6.1f}x")


def clingo_opt_value(combined, timeout=SCALED_TIMEOUT):
    """clingo's optimum from one CLI run with output captured."""
    try:
        r = subprocess.run(["clingo", "--opt-mode=opt", combined],
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                           timeout=timeout, text=True)
    except subprocess.TimeoutExpired:
        return None
    vals = re.findall(r"Optimization\s*:\s*(-?\d+)", r.stdout)
    return int(vals[-1]) if vals else None


if __name__ == "__main__":
    main()
