# handover.md

Everything one needs to take over this project. Updated before and after every
phase.

## Goal

The project (branch `aspicudf` of a clone of
https://github.com/picat-lang/Picat.git) combines two experimental features of
the Picat `experimental` branch and makes them work together:

1. **aspic** — an ASP (Answer Set Programming) to Picat transpiler
   (`aspic/`), which translates `.lp` programs into Picat programs that use
   the cp/sat constraint solvers.
2. **udf** — user-defined functions in constraints (`lib2/udf.pi`), which lets
   a registered Picat function be used inside constraint expressions
   (`$f(X) #= 6`) by expanding it into primitive arithmetic.

**Current task (COMPLETE):** the aspic translator(s) were rewritten so the
translated programs use the udf machinery and are easier to read. The old
generated code introduced awkward supplementary variables (`ASPIC_OPTCOND1/2/3`,
`ASPIC_AGVAL1/2` per aggregate element, hand-rolled `aspic_sum` chains,
universe-wide enumeration); the new code replaces those with one udf term per
aggregate/objective posted through a shimmed `'#='`.

## What is implemented (post-rewrite, aspic v0.4.0)

- `aspic/aspic_transpiler.pi` (parser + emitter; rewritten emitter):
  - valset / aggregate clauses emit **one udf term** per aggregate:
    - head valsets: `[$when(WC, EV) : <institer iters>, WC = <conj>]`
    - rule-body aggregates (inside `aspic_sum`): the minimal two-locals-per-
      instance chain `[W3 : X in aspic_instances(...), WC = <cond>, W3 #= cond(WC, EV, 0)]`
  - objectives (`#minimize`/`#maximize`/weak) are assembled as ONE
    `$suml([X : X in C1 ++ C2])` (a `$`-term cannot follow `+` in Picat, so
    the element lists are concatenated with `++` inside a comprehension);
    all-negative → Dir=max; mixed-sign → the -1 terms' `$when` becomes
    `$nwhen` (negated contribution), Dir=min
  - enumerations use `aspic_instances(P,N,I)` (the binding atom's instances)
    instead of `universe()`
  - the generated program defines no scaffolding predicates and no shims: it
    imports **sat2/cp2** (the lib2 shim libraries, whose `'#=` expands udf
    terms through `uf_flatten`), gets
    `main=>aspic_udf_init(),ASPIC_SEARCH:=<solver>,...` and no shim line
    (v0.4.1 removed the emitted `'#='` shim and the runtime's
    `aspic_flatten` wrapper — the sat2/cp2 libs provide the shims; they
    live in lib2, so the runs need lib2 on the module path: `PICATPATH` or
    picat's `-path` argument)
- `aspic/aspic_gen.pi` — the in-process twin, kept in sync (same clause
  bodies; island predicate head `aspic_<ID>(ASPIC_OPT)=>aspic_udf_init(),...`).
- `aspic/aspic_prep.pi` — the embedded flow splices `import sat2.`/`import
  cp2.` after the imports of the calling module (no shim line since v0.4.1).
- `aspic/aspic_runtime_template.pi` — the runtime defines the udf layer:
  `import udf.` and the idempotent `aspic_udf_init` registering
  `$when(C,V)=$(cond(C,V,0))`, `$nwhen(C,V)=$(cond(C,0-V,0))`,
  `$suml([])=0`, `$suml([H|T])=$(H+suml(T))`, and raising the expansion
  depth limit (`udf.set_max_depth(1000)`).  Generated code never references
  `udf` directly.
- Battery `aspic/test/test_aspic_syntax.sh`: **88 passed, 0 failed**
  (as of Thu Oct 22 2026). Two harness fixes were needed for v0.4.0:
  - `PICATPATH="$ASPIC_DIR/../lib2..."` (lib2 is one level ABOVE the repo's
    aspic/ folder; the old path pointed at a nonexistent `aspic/lib2`)
  - the awk that extracts the generated program from the transpiler's stderr
    must also match `import aspic_runtime.` (v0.4.0 emits it first).

## What works

- All 88 battery checks (syntax cases, parse check over examples/*.lp, the
  LP run_* end-to-end cases incl. mixed min/max objectives, and the
  embedded-flow run_*.pi cases).
- The backpack example converts with the new emitter: correct `maximization(29)`,
  `sel(1)`, `sel(3)` (scratch/backpack/t.pi shows the new readable shape).
- The embedded flow (`aspic_prep.pi`) works; earlier "UNSAT" symptoms in the
  embedded cases were TEST-HARNESS artifacts only (stale generated `out.pi` /
  `.qi` files in the CWD from relative-path runs, plus a wrong relative
  `PICATPATH` from a subfolder). With a clean CWD the store dumps correctly
  and the battery is green.
- **Queens scaling ladder (test/queens_ladder.sh)**: queens5.lp (the header's
  reference times, with completion on AMD EPYC 9654: 128 -> 0.9 s,
  256 -> 3.2 s, 512 -> 26.2 s, 768 -> 89.3 s, 1024 -> 944.6 s) copied to
  n = 8..768 and timed with both versions (5-minute per-run cap; the 1024
  point exceeds it).  No slowdown: ratios 95-102% over the ladder
  (n=768: 90.1 s new vs 94.5 s old, 95%; n=128-512 within 1-2%).  Gotcha the
  ladder needed: the transpiler writes aspic_runtime.pi with
  "sed ... aspic_runtime_template.pi > aspic_runtime.pi" reading the
  template from its CWD, so the version-matched template must be copied into
  the run dir before the transpile.
- **Baseline comparison (test/compare_baseline.sh)**: every shipped example
  run with the current sources vs the unmodified git HEAD sources —
  31 matched, 0 differed, 4 skipped (the skips behave identically on both
  sides: contrad, fd_global, money, read_data fail the same way). The
  optimization objectives all match (backpack 29, jssp2 1, tsp 11). For the
  satisfaction examples with several solutions the SPECIFIC solution may
  differ run-to-run (the satext portfolio races its solvers), which is why
  the script compares objectives/solution-existence, not solutions. One fix
  came out of it: `aspic_udf_init` now raises the udf expansion depth limit
  (`udf.set_max_depth(1000)`) — tsp's 36-term `$suml` objective exceeded the
  default limit of 100 (`udf_expansion_too_deep`).
- **Speed comparison (test/speed_baseline.sh)**: median of 3 runs per
  example, both versions — TOTAL 101% (no systematic slowdown). The large
  searches are unchanged (vl_schur 97%, hanoi 98%, queens5 97%,
  hk_queens 100%); backpack is 2.2x FASTER (45%); the one reproducible
  slowdown is ts_queens_adapted ~+8% (74.6s vs 69s, re-verified with fresh
  runs — the expanded `$when/$suml` formulation searches slightly longer
  than the old flat chain); the ms-scale ratios on the tiny examples are
  startup noise.

## What does not work / limitations

- Running a converted program still needs the repo's `lib2` on `PICATPATH`
  (`aspic_runtime.pi` imports `udf`). Documented in `aspic/README.md`.
- aspic language limitations unchanged (see `aspic/README.md` "Not implemented
  yet").
- Not yet done: a README pass documenting the v0.4.0 output shape, and
  `sources.md` (code/info sources) — both created this phase.
- Optional, not run this phase: `aspic/test/clingo_examples.py` (needs the
  clingo python module).

## Gotchas learned this phase (worth keeping)

- **One `$` per expression suffices** (scratch/udfexp/exp14.pi, exp15.pi;
  10 checks): a `$`-term is a term constructor whose arguments are never
  evaluated automatically, so an UNMARKED nested call inside a `$`-term
  behaves exactly like the marked form — `$f(g(X))` is the same term as
  `$f($g(X))`, and the walker expands the unmarked nested calls as it
  encounters them.  A `$`-term still cannot FOLLOW an operator
  (`$f(1)+$g(1)` is a syntax error), so the multi-call form is ONE
  `$`-term wrapping the whole expression with the unmarked calls inside:
  `$(f+g)`, `$(f(1)+g(1))` — never `$f+$g`.  A 0-arity udf function cannot
  be defined at all (`$f` throws `struct_expected` at compile time), so
  registered functions take at least one argument.  The docstring of
  lib2/udf.pi and doc/udf.tex were corrected accordingly (they used to
  require the `$` on nested calls).
- In this Picat build, defining additional PUBLIC functions in the runtime
  template (e.g. `aspic_flatten`/`aspic_udf_init`) does NOT affect the
  embedded flow — earlier suspicion was wrong; the failures were harness
  artifacts.
- The battery's tmp dirs (`test/tmp_XXXXXX`) are removed by its trap; do not
  run manual conversions with a RELATIVE output path while inside the repo —
  they leave stale `out.pi`/`.qi` files in the CWD that poison later manual
  runs. Always use a `scratch/<dir>` CWD or an absolute output path.
- `git checkout HEAD -- <files>` reverted uncommitted work once; it was
  recovered from the dropped-stash commit via `git fsck --lost-found`
  (dangling commit 4189872).

## Files

- aspic/: aspic.pi, aspic_transpiler.pi (rewritten emitter), aspic_gen.pi
  (twin, in sync), aspic_prep.pi (shim splicing), aspic_runtime_template.pi
  (udf plumbing), picasp.pi, picasp_preprocessor.py, show, examples/, test/,
  README.md (v0.4.0 section).
- lib2/: udf.pi, udf_ops.pi, udf_ops_sat.pi, udf_ops_mip.pi, udf_ops_smt.pi.
- test/: test_aspic_syntax.sh (fixed PICATPATH + awk), compare_baseline.sh
  (answers vs the git HEAD sources), speed_baseline.sh (times vs the git
  HEAD sources), queens_ladder.sh (the n-queens scaling times vs the git
  HEAD sources), udf_check.sh, udf_check.pi, clingo_examples.py.
- scratch/backpack/: the backpack conversion (t.pi = the new v0.4.0 output).
- scratch/udfexp/exp1–exp13.pi: the construct experiments that validated the
  design (cond in #=, $-term syntax, multi-rule define isolation).
- sources.md: the references this work is based on.

## How to build/run/test

- Picat experimental binary at /home/jovyan/bin/picat (4.0b8, commit 9796071).
- Convert+run an ASP example:
  `cd scratch/<dir> && picat -log aspic.pi file.lp 2>&1`
  (needs aspic.pi, aspic_transpiler.pi, aspic_runtime_template.pi copied
  there), with `PICATPATH` including the repo's `lib2`.
- Battery: `bash test/test_aspic_syntax.sh` from `aspic/` (sets its own
  PICATPATH; exit 0 iff all 88 checks pass).
- udf layer check: `bash test/udf_check.sh` from the repo root.
