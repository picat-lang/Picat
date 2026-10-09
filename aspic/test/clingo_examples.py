#!/usr/bin/env python3
"""Compare clingo's answer sets with aspic's for the clingo repo examples.

Multi-file examples are combined: each instance file is joined with its
sibling encoding (a .lp in the same directory or an encodings/ subdirectory
that is not itself an instance and not a control/script file).  For each
program: ground and solve with the clingo python module (all models) and
with the aspic generated program (all flow), and compare the sets of answer
sets (sorted lists of true atom strings).
"""
import clingo
import subprocess
import sys
import os
import glob
import re

ASPIC = os.path.expanduser("~/bin/picat")
ASPIC_DIR = os.environ.get("ASPIC_DIR", "/home/jovyan/snapshotable/aspic/picat/aspic")
CLINGO_EX = os.environ.get("CLINGO_EX", "/home/jovyan/snapshotable/aspic/scratch/clingo/clingo/examples/clingo")
WORK = os.environ.get("WORK", "/tmp/aspic_clingo_cmp")

os.makedirs(WORK, exist_ok=True)

CTRL = re.compile(r"(control|addclause|cannot-|consequences-|extending|external|"
                  r"onmodel|tmode|stats-|pydoc|solvable|load|expansion.*control)", re.I)


def is_instance(fn):
    return bool(re.search(r"instance|^-?\d", os.path.basename(fn)))


def programs():
    """Yield (name, [files]) for every comparable program."""
    seen = set()
    for path in sorted(glob.glob(os.path.join(CLINGO_EX, "**", "*.lp"), recursive=True)):
        d = os.path.dirname(path)
        fn = os.path.basename(path)
        if is_instance(fn):
            continue  # handled together with its encoding
        rel = os.path.relpath(path, CLINGO_EX)
        if rel in seen or CTRL.search(rel):
            continue
        seen.add(rel)
        # sibling instances: same dir, then instances/ subdir
        insts = sorted(glob.glob(os.path.join(d, "instance*.lp"))) or \
                sorted(glob.glob(os.path.join(d, "instances", "*.lp")))
        encdir = os.path.join(d, "encodings")
        encs = sorted(glob.glob(os.path.join(encdir, "*.lp"))) if os.path.isdir(encdir) else []
        if insts and not encs:
            encs = [path]
        if insts:
            for inst in insts:
                if CTRL.search(os.path.relpath(inst, CLINGO_EX)):
                    continue
                yield (re.sub(r"[^A-Za-z0-9]", "_", rel + "+" + os.path.basename(inst)),
                       [path, inst])
        else:
            yield (re.sub(r"[^A-Za-z0-9]", "_", rel), [path])


def clingo_models(files):
    ctl = clingo.Control()
    try:
        for f in files:
            ctl.load(f)
        ctl.ground([("base", [])])
    except Exception as e:
        return None, str(e).splitlines()[0][:90]
    models = []
    with ctl.solve(yield_=True) as h:
        for m in h:
            models.append(sorted(str(s) for s in m.symbols(atoms=True)))
    models.sort()
    return models, None


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


def aspic_models(files, name):
    combined = os.path.join(WORK, name + ".lp")
    with open(combined, "w") as out:
        for f in files:
            out.write(open(f).read() + "\n")
    gen = os.path.join(WORK, name + ".gen.txt")
    with open(gen, "w") as f:
        try:
            subprocess.run([ASPIC, "-log", "aspic_transpiler.pi", "run", combined],
                           cwd=ASPIC_DIR, stdout=subprocess.PIPE, stderr=f, timeout=60)
        except subprocess.TimeoutExpired:
            return None, "timeout"
    err = open(gen).read()
    out = subprocess.run([ASPIC, "-log", "aspic_transpiler.pi", "run", combined],
                         cwd=ASPIC_DIR, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                         timeout=60).stdout.decode()
    if "notparsed = []" not in out:
        return None, "notparsed"
    if "error(" in err:
        return None, "aspic error"
    m = re.search(r"(?m)^import (sat|cp)\.", err)
    if not m:
        return None, "no generated program"
    pi = os.path.join(WORK, name + ".pi")
    with open(pi, "w") as f:
        f.write(err[m.start():])
    run = os.path.join(WORK, name + ".run.txt")
    env = dict(os.environ, SATEXT_SOLVER="")
    try:
        with open(run, "w") as f:
            subprocess.run([ASPIC, "-log", "-s", "1234567890", pi],
                           cwd=ASPIC_DIR, stdout=f, stderr=subprocess.STDOUT, timeout=60, env=env)
    except subprocess.TimeoutExpired:
        return None, "timeout"
    models = []
    for line in open(run):
        mm = re.search(r"solution = \[(.*)\]", line)
        if mm:
            atoms = split_atoms(mm.group(1))
            models.append(sorted(a for a in atoms if a))
    models.sort()
    return models, None


def main():
    n = matched = mismatched = notparsed = errors = 0
    for name, files in programs():
        n += 1
        cm, cerr = clingo_models(files)
        if cm is None:
            print(f"SKIP  {name}: {cerr}")
            continue
        am, aerr = aspic_models(files, name)
        if am is None:
            if aerr == "notparsed":
                notparsed += 1
            else:
                errors += 1
                print(f"ERR   {name}: {aerr}")
            continue
        if am == cm:
            matched += 1
            print(f"MATCH {name} ({len(cm)} model(s))")
        else:
            mismatched += 1
            print(f"DIFF  {name}")
            print(f"      clingo ({len(cm)}): {cm[0][:4]}")
            print(f"      aspic  ({len(am)}): {am[0][:4] if am else []}")
    print(f"\n{n} programs: {matched} matching, {mismatched} DIFFERING, "
          f"{notparsed} not parseable by aspic, {errors} other errors")


if __name__ == "__main__":
    main()
