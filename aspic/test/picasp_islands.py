#!/usr/bin/env python3
"""Verify the known-working programs as asp islands in picasp.

Every program the existing harnesses confirm works with aspic (MATCH:
the model(s) clingo-valid and, for the optimization programs, the cost
equal to clingo's optimum) is embedded as an "asp ... end" island in a
Picat driver and solved through the picasp pure-Picat flow:

  1. aspic_prep.pi ("pre") transpiles the driver: the island's content
     becomes the predicate aspic_1(ASPIC_OPT_1) (the rules, the
     completion and the objective), and aspic_runtime.pi is generated
     from the template into the same directory.
  2. The driver calls aspic_solve_sub(ASPIC_OPT_1) (the island's
     optimization direction comes from the #maximize of the block) and
     prints the island's model and cost.

The island's answer must pass the same clingo validity check as the
direct aspic answer (clingo_check for the book programs, membership in
clingo's answer-set set for the clingo examples), and for optimization
programs the island's cost must equal clingo's optimum.

Suites:
  clingo  the clingo repo examples (clingo_examples.py, MATCH subset)
  book    the "Programming with CLINGO" programs (pwc_aspic_test.py, MATCH)
  all     both (default)

Usage: python3 picasp_islands.py [--keep] [suite]
Environment:
  ASPIC_DIR  the aspic directory (default: the repo's aspic/)
  PWC_DIR    the pwc-programs corpus directory
  WORK       scratch dir for the generated islands (default:
             /home/jovyan/snapshotable/aspic/scratch/isle)
Requires: clingo python module, picat on PATH (~/bin/picat).
"""
import clingo
import subprocess
import sys
import os
import re
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
ASPIC_DIR = os.environ.get("ASPIC_DIR", os.path.dirname(HERE))
PWC = os.environ.get("PWC_DIR", "/home/jovyan/snapshotable/aspic/pwc-programs")
WORK = os.environ.get("WORK", "/home/jovyan/snapshotable/aspic/scratch/isle")
ASPIC = os.path.expanduser("~/bin/picat")

# the clingo examples harness reads WORK at import: keep both suites'
# scratch in the same folder
os.environ.setdefault("WORK", WORK)

sys.path.insert(0, ASPIC_DIR)
sys.path.insert(0, PWC)

import clingo_examples
import pwc_aspic_test

os.makedirs(WORK, exist_ok=True)

ATOM_RE = re.compile(r"^[a-z][A-Za-z0-9_']*(\(.*\))?$", re.S)


def split_atoms(s):
    atoms, depth, cur = [], 0, ""
    for ch in s:
        if ch == "(":
            depth += 1
            cur += ch
        elif ch == ")":
            depth -= 1
            cur += ch
        elif ch == "," and depth == 0:
            atoms.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        atoms.append(cur.strip())
    return atoms


def island_solve(name, asp_lines, timeout=180):
    """Embed the asp_lines in a Picat driver as one asp ... end island,
    transpile with aspic_prep.pi (the picasp pure-Picat flow) and run.
    Returns (sorted_plain_atoms_or_None, cost_or_None, err_or_None).
    atoms == [] means the empty model (a valid model with no true
    atoms); atoms is None with err == "UNSAT" means the island reports
    the program unsatisfiable."""
    wd = os.path.join(WORK, "i_" + name)
    shutil.rmtree(wd, ignore_errors=True)
    os.makedirs(wd, exist_ok=True)
    src = os.path.join(wd, "island.pi")
    with open(src, "w") as f:
        f.write("import aspic_runtime.\n"
                "main=>\n"
                "asp\n" + "\n".join(asp_lines) + "\nend\n"
                ",\n"
                "aspic_solve_sub(ASPIC_OPT_1),\n"
                "M=get_heap_map(aspic_m(aspic_atoms)),\n"
                "println(solution=[aspic_showaggr(K):K in M.keys.sort,V=M.get(K),V==1,"
                "not aspic_isfdvariable(K),aspic_isaggratom(K)]\n"
                "++[K.show:K in M.keys.sort,V=M.get(K),V==1,"
                "not aspic_isfdvariable(K),not aspic_isaggratom(K)]\n"
                "++[X=X.showfd:X in [F:K in M.keys,aspic_isfdvariable(K),K=..L,"
                "L2=L.removelast,F=..L2].remove_dups.sort]),\n"
                "println(cost=ASPIC_OPT_1).\n")
    prep = subprocess.run(
        [ASPIC, "-log", os.path.join(ASPIC_DIR, "aspic_prep.pi"), "pre", src, wd,
         os.path.join(wd, "final.pi"),
         os.path.join(ASPIC_DIR, "aspic_runtime_template.pi"), "sat"],
        cwd=ASPIC_DIR, capture_output=True, text=True, timeout=timeout)
    if prep.returncode != 0 or "ASP block" not in prep.stdout:
        return None, None, "prep failed: " + \
            ((prep.stderr or prep.stdout).strip()[-100:] or "no output")
    try:
        run = subprocess.run([ASPIC, "-log", "-s", "1234567890", "final.pi"],
                             cwd=wd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, None, "run timeout"
    text = run.stdout + run.stderr
    if "UNSAT" in text:
        return None, None, "UNSAT"
    if "error(" in text:
        bad = [l for l in text.splitlines() if "error(" in l][0]
        return None, None, bad.strip()[:90]
    sol = cost = None
    for line in text.splitlines():
        mm = re.search(r"solution\s*=\s*\[(.*)\]", line)
        if mm:
            sol = split_atoms(mm.group(1))
        mc = re.search(r"cost\s*=\s*(-?\d+)", line)
        if mc:
            cost = int(mc.group(1))
    if sol is None:
        return None, None, "no solution line"
    return sorted(a for a in sol if ATOM_RE.match(a)), cost, None


def clingo_suite():
    n = match = diff = err = 0
    for name, files in clingo_examples.programs():
        cm, cerr = clingo_examples.clingo_models(files)
        if cm is None:
            continue  # clingo cannot ground it: outside both harnesses' scope
        am, aerr = clingo_examples.aspic_models(files, name)
        if am is None or am != cm:
            continue  # not in the MATCH subset (not known to work with aspic)
        combined = os.path.join(clingo_examples.WORK, name + ".lp")
        asp_lines = open(combined).read().splitlines()
        n += 1
        atoms, cost, ierr = island_solve(name, asp_lines)
        if atoms is None and ierr == "UNSAT":
            ok = (cm == [])
        elif atoms is None:
            err += 1
            print(f"ERR   {name}: {ierr}")
            continue
        else:
            ok = any(atoms == m for m in cm)
        if ok:
            match += 1
            print(f"MATCH {name}")
        else:
            diff += 1
            print(f"DIFF  {name}")
            print(f"      island ({len(atoms)}): {atoms[:4] if atoms else 'UNSAT'}")
            print(f"      clingo ({len(cm)}): {cm[0][:4] if cm else []}")
    print(f"\nclingo: {n} islands, {match} matched, {diff} DIFFERING, {err} errors")
    return n, match, diff, err


def book_suite():
    n = match = diff = err = 0
    for name, files, consts, kind in pwc_aspic_test.PROGS:
        paths = [f if f.startswith(pwc_aspic_test.WORK)
                 else os.path.join(pwc_aspic_test.HERE, f) for f in files]
        sols, aerr = pwc_aspic_test.aspic_solution(files, name, consts)
        if sols is None:
            continue  # aspic cannot handle it: outside the MATCH subset
        cost, cerr = pwc_aspic_test.clingo_opt_cost(paths, consts) if kind else (None, None)
        if cerr:
            continue
        ok_all = sols == []
        if not ok_all:
            for sol, obj in sols:
                ism, scost = pwc_aspic_test.clingo_check(paths, consts, sol)
                ok = bool(ism)
                if ok and kind:
                    ok = scost == cost and obj is not None and (
                        obj == -scost if kind == "max" else obj == scost)
                if not ok:
                    ok_all = False
                    break
                ok_all = True
        if not ok_all:
            continue  # the DIFF subset: not known to work with aspic
        combined = os.path.join(pwc_aspic_test.WORK, name + ".lp")
        asp_lines = open(combined).read().splitlines()
        n += 1
        atoms, icost, ierr = island_solve(name, asp_lines)
        if atoms is None and ierr == "UNSAT":
            ok = (sols == [])
        elif atoms is None:
            err += 1
            print(f"ERR   {name}: {ierr}")
            continue
        else:
            ism, _ = pwc_aspic_test.clingo_check(paths, consts, atoms)
            ok = bool(ism)
            if ok and kind and icost is not None:
                ok = icost == cost if kind == "min" else -icost == cost
        if ok:
            match += 1
            print(f"MATCH {name}")
        else:
            diff += 1
            print(f"DIFF  {name}")
            print(f"      island: {atoms[:4] if atoms else 'UNSAT'}")
    print(f"\nbook: {n} islands, {match} matched, {diff} DIFFERING, {err} errors")
    return n, match, diff, err


def main():
    which = "all"
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    if args:
        which = args[0]
    if which in ("clingo", "all"):
        clingo_suite()
    if which in ("book", "all"):
        book_suite()


if __name__ == "__main__":
    main()
