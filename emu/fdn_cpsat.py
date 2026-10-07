#!/usr/bin/env python3
"""CP-SAT server for picat-fdn's external solver (FDN_EXTSOLVER=cp-sat).

Reads one JSON request per line on stdin, solves the model with ortools
CP-SAT, writes one JSON response per line on stdout.  Run with --server for
the persistent loop (one process for the whole picat run, so repeated solves
pay the interpreter start-up once); without it, one request is read, solved
and the process exits.

Request:
  {"mode": "solve"|"count", "maxsols": M, "time": T, "model": {...}}
    mode "solve": satisfaction (maxsols caps the enumeration, 0 = no cap) or,
      when the model has an objective (obj >= 0), optimization (one solution,
      the proven optimum).
    mode "count": the solutions are counted without being handed over.
    time: CP-SAT time limit in seconds, 0 = none.
Response (fixed key order; the C side scans it):
  {"status":"ok","obj":N,"count":N,"sols":[[...]],"wall":T}
    status "ok": the search completed (sols = [] means unsat);
    anything else ("limit" = a cap or the time limit cut the search,
    "feasible" = not proven optimal, "error") makes the C side fall back.

Model (variable ids 0-based, values absolute):
  glo, nvars, obj
  domains: [[[lo,hi],...], ...]
  edges:   [[x,d,t], ...]   x + d != t
  halls:   [[x,...], ...]   all different
  tuples:  [[x,...], ...]   the tuples of the element constraints
  elements:[[i,t,v], ...]   tuples[t][i] == v  (0-based tuple index)
  chans:   [[x,off,a,t],...] tuples[t][x-off-1] == a, a fixed
  maps:    [[o,s,k,t], ...]  t == s*o + k  (X = Y+C / X = C-Y)
  lins:    [{"op":0/1/2,"c":c,"terms":[[a,x],...]}, ...]
                             op 0/2: sum(a_i x_i) + c = 0; op 1: >= 0
  abs:     [[x,y,n], ...]    |x-y| = n
  divs:    [[x,y,z], ...]    floored division x div y = z (y fixed), i.e.
                             0 <= x - y*z <= |y|-1, exact for both signs
  mods:    [[x,y,z], ...]    x mod y = z (the remainder of the division
                             rounding toward 0, as CP-SAT's and Picat's)
  mm:      [{"m":0/1,"r":r,"terms":[[id,v],...]}, ...]
                             r = min/max of the terms, id < 0: the constant v
  reif:    [{"mode":m,"b":b,"x":x,"y":y}, ...]
                             B <=> (0: x = c, 1: x != c, 2: x = y, 3: x != y,
                             4: x >= y); mode 0/1: y is a constant, else a var
  ent:     [{"mode":m,"b":b,"x":x,"y":y}, ...]  B => the same, one-directional
  lex:     [{"le":0/1,"pairs":[[x,y],...]}, ...]
                             lexicographic <= / < on the pair vectors
"""
import json
import sys
import threading
import time

# ortools 9.15's python API imports pandas, which imports pyarrow when
# installed; pyarrow bundles its own libprotobuf, and the two protobuf copies'
# descriptor registrations collide (FATAL). The server never uses pyarrow, so
# block the import in this process only (pandas works without it).
sys.modules.setdefault('pyarrow', None)

from ortools.sat.python import cp_model


def build(m):
    M = cp_model.CpModel()
    X = [M.NewIntVarFromDomain(cp_model.Domain.FromIntervals([list(iv) for iv in dom]), "x%d" % i)
         for i, dom in enumerate(m["domains"])]
    for x, d, t in m.get("edges", []):
        M.Add(X[x] + d != X[t])
    for xs in m.get("halls", []):
        M.AddAllDifferent([X[x] for x in xs])
    TUP = [[X[i] for i in tup] for tup in m.get("tuples", [])]
    for i, t, v in m.get("elements", []):
        n = len(TUP[t])
        iv = M.NewIntVar(0, n - 1, "")
        M.Add(iv == X[i] - 1)           # the net's element is 1-based
        M.AddElement(iv, TUP[t], X[v])
    for x, off, a, t in m.get("chans", []):
        n = len(TUP[t])
        iv = M.NewIntVar(0, n - 1, "")
        M.Add(iv == X[x] - off - 1)
        M.AddElement(iv, TUP[t], a)
    seen = set()
    for o, s, k, t in m.get("maps", []):
        if (o, s, k, t) in seen:
            continue
        seen.add((o, s, k, t))
        M.Add(X[t] == s * X[o] + k)
    for l in m.get("lins", []):
        e = sum(a * X[x] for a, x in l["terms"]) if l["terms"] else 0
        if l["op"] == 1:
            M.Add(e >= -l["c"])
        else:
            M.Add(e == -l["c"])
    for x, y, nn in m.get("abs", []):
        M.AddAbsEquality(nn, X[x] - X[y])
    for x, y, z in m.get("divs", []):
        M.Add(X[x] - y * X[z] >= 0)
        M.Add(X[x] - y * X[z] <= abs(y) - 1)
    for x, y, z in m.get("mods", []):
        M.AddModuloEquality(X[z], X[x], y)
    for mm in m.get("mm", []):
        terms = [X[i] if i >= 0 else v for i, v in mm["terms"]]
        (M.AddMinEquality if mm["m"] else M.AddMaxEquality)(X[mm["r"]], terms)

    def bool_of(bv):
        b = M.NewBoolVar("")
        M.Add(bv == 1).OnlyEnforceIf(b)
        M.Add(bv == 0).OnlyEnforceIf(b.Not())
        return b

    for r in m.get("reif", []):
        B, x, mo = bool_of(X[r["b"]]), X[r["x"]], r["mode"]
        if mo in (0, 1):
            c = r["y"]
            if mo == 0:
                M.Add(x == c).OnlyEnforceIf(B)
                M.Add(x != c).OnlyEnforceIf(B.Not())
            else:
                M.Add(x != c).OnlyEnforceIf(B)
                M.Add(x == c).OnlyEnforceIf(B.Not())
        else:
            y = X[r["y"]]
            if mo == 2:
                M.Add(x == y).OnlyEnforceIf(B)
                M.Add(x != y).OnlyEnforceIf(B.Not())
            elif mo == 3:
                M.Add(x != y).OnlyEnforceIf(B)
                M.Add(x == y).OnlyEnforceIf(B.Not())
            else:
                M.Add(x >= y).OnlyEnforceIf(B)
                M.Add(x < y).OnlyEnforceIf(B.Not())
    for en in m.get("ent", []):
        B, x, mo = bool_of(X[en["b"]]), X[en["x"]], en["mode"]
        if mo in (0, 1):
            c = en["y"]
            (M.Add(x == c) if mo == 0 else M.Add(x != c)).OnlyEnforceIf(B)
        else:
            y = X[en["y"]]
            (M.Add(x == y) if mo == 2 else M.Add(x != y)).OnlyEnforceIf(B)
    for lx in m.get("lex", []):
        pairs, le = lx["pairs"], lx["le"]
        n = len(pairs)
        es, ss = [], []
        for x, y in pairs:
            e = M.NewBoolVar("")
            M.Add(X[x] == X[y]).OnlyEnforceIf(e)
            M.Add(X[x] != X[y]).OnlyEnforceIf(e.Not())
            s = M.NewBoolVar("")
            M.Add(X[x] < X[y]).OnlyEnforceIf(s)
            M.Add(X[x] >= X[y]).OnlyEnforceIf(s.Not())
            es.append(e)
            ss.append(s)
        for i in range(n):
            if i < n - 1 or le:
                M.AddBoolOr([ss[i], es[i]]).OnlyEnforceIf(es[:i])
            else:
                M.AddBoolOr([ss[i]]).OnlyEnforceIf(es[:i])
    obj = m.get("obj", -1)
    if obj >= 0:
        M.Minimize(X[obj])
    return M, X, obj


def solve(req):
    m = req["model"]
    M, X, obj = build(m)
    solver = cp_model.CpSolver()
    tlim = req.get("time", 0)
    if tlim:
        solver.parameters.max_time_in_seconds = tlim
    w = req.get("workers", 0)
    if w:
        solver.parameters.num_workers = w
    t0 = time.time()
    mode = req.get("mode", "solve")
    if obj >= 0:
        st = solver.Solve(M)
        wall = time.time() - t0
        if st == cp_model.OPTIMAL:
            return {"status": "ok", "obj": round(solver.ObjectiveValue()), "count": 1,
                    "sols": [[solver.Value(v) for v in X]], "wall": wall}
        if st == cp_model.FEASIBLE:
            return {"status": "feasible", "obj": 0, "count": 0, "sols": [], "wall": wall}
        if st == cp_model.INFEASIBLE:
            return {"status": "ok", "obj": 0, "count": 0, "sols": [], "wall": wall}
        return {"status": "limit", "obj": 0, "count": 0, "sols": [], "wall": wall}
    sols = []
    seen = set()
    nsol = [0]
    maxs = req.get("maxsols", 0)
    # the native search branches only on the label variables, so a solution
    # is one assignment of the label list: dedup on the label projection
    # (other net variables — reification bools of entailments, say — can be
    # free and would multiply the solutions), and with num_workers > 1 the
    # same solution can also be found more than once
    lab = m.get("label", list(range(len(X))))
    # the lazy protocol: the C side re-asks with the label projections it
    # already holds forbidden, so each ask returns only new solutions
    avoid = req.get("avoid", [])
    if avoid:
        M.AddForbiddenAssignments([X[i] for i in lab], avoid)
    # with num_workers > 1 the callback runs on several threads at once:
    # the dedup set and the counter need a lock (nsol[0] += 1 alone loses
    # increments, the check-then-add races)
    cblock = threading.Lock()
    capped = [False]

    class CB(cp_model.CpSolverSolutionCallback):
        def on_solution_callback(self):
            with cblock:
                key = tuple(self.Value(X[i]) for i in lab)
                if key in seen:
                    return
                seen.add(key)
                nsol[0] += 1
                if mode == "solve":
                    sols.append([self.Value(v) for v in X])
                if maxs and nsol[0] >= maxs:
                    capped[0] = True
                    self.StopSearch()

    def enum_once(width):
        seen.clear(); nsol[0] = 0; del sols[:]
        s = cp_model.CpSolver()
        s.parameters.num_workers = width
        if tlim:
            s.parameters.max_time_in_seconds = tlim
        st = s.SearchForAllSolutions(M, CB())
        return st, time.time() - t0

    # complete enumeration is only reliable on a single worker in ortools:
    # with several the portfolio can silently drop solutions and still
    # report OPTIMAL.  An INFEASIBLE verdict is sound at any width (and much
    # faster there -- pigeonhole proofs), but a solution set must be
    # complete, so anything else is re-enumerated on one worker.  The
    # solution cap is the exception: the C side re-asks for the rest (the
    # lazy protocol), so a capped batch returns at once -- a first-solution
    # search then costs one ask and no single-threaded re-pass.
    t0 = time.time()
    if maxs == 1:
        # the first batch of the lazy protocol: any one new solution.  The
        # solve mode (default heuristics) is far faster at finding one than
        # the enumeration mode (measured: queens-200, 2 s vs 27 s).  The
        # status is "limit" whenever a solution was found: the C side cannot
        # know whether more exist, and a full walk re-asks with the avoid
        # list, which completes the set; INFEASIBLE is unsat, sound.
        st = solver.Solve(M)
        wall = time.time() - t0
        if st in (cp_model.OPTIMAL, cp_model.FEASIBLE):
            return {"status": "limit", "obj": 0, "count": 1,
                    "sols": [[solver.Value(v) for v in X]], "wall": wall}
        if st == cp_model.INFEASIBLE:
            return {"status": "ok", "obj": 0, "count": 0, "sols": [], "wall": wall}
        return {"status": "limit", "obj": 0, "count": 0, "sols": [], "wall": wall}
    st, w1 = enum_once(w)
    if st == cp_model.INFEASIBLE:
        return {"status": "ok", "obj": 0, "count": 0, "sols": [], "wall": w1}
    if capped[0]:
        return {"status": "limit", "obj": 0, "count": nsol[0], "sols": sols, "wall": w1}
    if w > 1 or st != cp_model.OPTIMAL:
        st, w2 = enum_once(1)
        wall = w1 + w2
    else:
        wall = w1
    if st != cp_model.OPTIMAL:
        return {"status": "limit", "obj": 0, "count": nsol[0], "sols": sols, "wall": wall}
    return {"status": "ok", "obj": 0, "count": nsol[0], "sols": sols, "wall": wall}


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--server":
        while True:
            line = sys.stdin.readline()
            if not line:
                break
            line = line.strip()
            if not line:
                continue
            try:
                r = solve(json.loads(line))
            except Exception as exc:                    # any server error: the C side falls back
                r = {"status": "error", "obj": 0, "count": 0, "sols": [], "err": str(exc)}
            sys.stdout.write(json.dumps(r) + "\n")
            sys.stdout.flush()
    else:
        line = sys.stdin.readline()
        try:
            r = solve(json.loads(line))
        except Exception as exc:
            r = {"status": "error", "obj": 0, "count": 0, "sols": [], "err": str(exc)}
        sys.stdout.write(json.dumps(r) + "\n")


if __name__ == "__main__":
    main()
