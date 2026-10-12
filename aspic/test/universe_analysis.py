#!/usr/bin/env python3
"""Analyze the universe() iteration cost in the aspic translations.

For every working (MATCH) example of the two reference harnesses - the
clingo-repo examples (clingo_examples.py) and the "Programming with
CLINGO" book programs (pwc_aspic_test.py) - this script inspects the
generated Picat program and reports every place where the full
universe() is iterated, classified by context:

  optsum    - the #minimize/#maximize objective sum (ASPIC_OPTCOND3)
  aggregate - an aggregate (aspic_sumaggr / aspic_showaggr) body
  conj      - an aspic_conj (rule-body / constraint-body) comprehension
  inst      - an aspic_instances / support comprehension
  other     - anything else

For each occurrence it also reports the iterator variable so the
replacement can be judged: what a targeted structure would iterate over
instead of the full universe.

Usage: python3 universe_analysis.py [clingo|book|all]
"""
import os
import re
import sys
import glob

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, "/home/jovyan/snapshotable/aspic/pwc-programs")
import clingo_examples as ce
import pwc_aspic_test as pwc

HERE = os.path.dirname(os.path.abspath(__file__))

# the MATCH subsets, re-derived from the harnesses' own comparison logic
def clingo_match():
    out = []
    for name, files in ce.programs():
        cm, cerr = ce.clingo_models(files)
        if cm is None:
            continue
        am, aerr = ce.aspic_models(files, name)
        if am is None or am != cm:
            continue
        out.append(name)
    return out


def book_match():
    out = []
    for name, files, consts, kind in pwc.PROGS:
        sols, aerr = pwc.aspic_solution(files, name, consts)
        if sols is None:
            continue
        out.append(name)
    return out


CTX_ORDER = ["optsum", "aggregate", "conj", "inst", "other"]


def classify(line):
    if "ASPIC_OPTCOND3" in line:
        return "optsum"
    if "aspic_sumaggr" in line or "aspic_maxaggr" in line or \
       "aspic_card" in line or "aspic_showaggr" in line:
        return "aggregate"
    if "aspic_conj" in line or "ASPIC_CL3" in line:
        return "conj"
    if "aspic_instances" in line or "aspic_disj" in line:
        return "inst"
    return "other"


def iterator_vars(line):
    """The variables iterated in the same comprehension as universe()."""
    m = re.search(r"\[([A-Za-z0-9_,$:()\. ]*universe\(\)[A-Za-z0-9_,$:()\. \+]*)\]", line)
    if not m:
        return []
    seg = m.group(1)
    return sorted(set(re.findall(r"([A-Za-z_][A-Za-z0-9_]*)\s+in\s+universe\(\)", seg)))


def analyze(pi_path):
    res = {}
    if not os.path.exists(pi_path):
        return None
    for line in open(pi_path, encoding="utf-8", errors="replace"):
        n = line.count("universe()")
        if not n:
            continue
        ctx = classify(line)
        entry = res.setdefault(ctx, {"count": 0, "vars": set(), "sample": None})
        entry["count"] += n
        entry["vars"].update(iterator_vars(line))
        if entry["sample"] is None:
            entry["sample"] = line.strip()[:150]
    return res


def report(suite_name, names, pi_of):
    grand = {c: 0 for c in CTX_ORDER}
    rows = []
    for name in sorted(names):
        res = analyze(pi_of(name))
        if res is None:
            continue
        total = sum(e["count"] for e in res.values())
        if not total:
            continue
        rows.append((name, total, res))
        for c, e in res.items():
            grand[c] += e["count"]

    print(f"=== {suite_name}: {len(rows)} programs with universe() iteration ===")
    for name, total, res in sorted(rows, key=lambda r: -r[1]):
        parts = ", ".join(f"{c}={res[c]['count']}" for c in CTX_ORDER
                          if c in res and res[c]["count"])
        print(f"{name} ({total}): {parts}")
        for c in CTX_ORDER:
            if c in res and res[c]["count"] and res[c]["sample"]:
                vs = ",".join(sorted(res[c]["vars"])) or "?"
                print(f"    [{c}] vars={vs} sample: {res[c]['sample'][:120]}")
    print(f"--- {suite_name} totals by context: " +
          ", ".join(f"{c}={grand[c]}" for c in CTX_ORDER))
    return grand


def main():
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    if which in ("clingo", "all"):
        names = clingo_match()
        pi_of = lambda n: os.path.join(ce.WORK, n + ".pi")
        report("clingo", names, pi_of)
    if which in ("book", "all"):
        names = book_match()
        pi_of = lambda n: os.path.join(pwc.WORK, n + ".pi")
        report("book", names, pi_of)


if __name__ == "__main__":
    main()
