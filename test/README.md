# test/ — regression sweeps for the experimental build

Scripts that test this build for regressions after changes. There are
three groups: the `exs` samples (in `exs/` and `exs/fd_native_mt/`), the
Hakan Kjellerstrand example tree (`test/`, this folder), and the webasm
build headless (`webasm/headless/`).

Throughout: *this build* is the experimental picat (`emu/picat` after
`cd emu && make -f Makefile.linux64`); *the stock binary* is a release
picat binary (Picat 3.9#12), e.g. `STOCK_PICAT=/path/to/picat`; the *Hakan
tree* is a directory of `.pi` example programs, expected layout:
`*.pi` files at the root plus `ppl/` and `nn_hakank/` subdirectories
(2579 programs in the reference tree).

## 1. The exs sample suites (repo)

- `exs/deviation_check.sh [family ...]` — every `exs` sample (families
  default: cp sat mip smt planner euler debug) on this build and on the
  stock binary; reports `DIVERGES (exp fails)` — the regression class —
  plus stock-fails and output-differs cases. Library modules (no main)
  are skipped. Env: `STOCK_PICAT`, `SATEXT_COMPARE=1` (compare with the
  external-solver portfolio active on this build only).
- `exs/fd_native_mt/dev_check_312.sh [family ...]` — the fdn variant:
  every sample of the chosen families (default: cp debug euler
  fd_native_mt) on this build and on the stock binary, each side pinned
  to a dedicated group of free physical cores of one NUMA node; compares
  the normalized outputs, expects both sides to run and return 0, and
  reports the acceleration. Env: `STOCK_PICAT`, `GROUP` (cores per side,
  default 16), `TIMEOUT` (default 300 s).
- `exs/fd_native_mt/ext_check.sh` — the external CP-SAT check
  (`FDN_EXTSOLVER=cp-sat`): every program of that folder must print the
  same output with and without the external server; the comparison is
  semantic (lines sorted, times and backtracks masked). Repo-only, no
  Hakan tree needed.
- `pb_families_bench.sh` — the five pb-advantage families
  (`exs/pb/php_card`, `pmodd`, `mchess`, `evencol`, `subsetcard`),
  each under all three solvers (import pb / cp / sat, swapped into
  temp copies): expects UNSAT everywhere, reports the wall time per
  run. Env: `PBSOL` (the roundingsat binary), `BOOST_LIB` (its boost
  runtime dir, exported as `LD_LIBRARY_PATH`), `FDN`, `TIMEOUT`
  (default 60 s), `PICAT`, `WORK` (the scratch dir for the swapped
  copies, default `scratch/pbfam`). See `exs/pb/README.md` for the
  finding.

## 2. The Hakan tree sweeps (this folder)

All four sweep every `.pi` of the Hakan tree (root, `ppl/`,
`nn_hakank/`) and never touch the original tree: each side works on a
fresh writable copy with any `.qi` bytecode deleted first, so no build
ever shares compiled bytecode. The `pb` examples (`exs/pb`, which need
the `pb` module and an external PB solver) are swept too, single-sided
where the stock binary has no `pb`. Each script takes its trees and
binaries from environment variables with defaults for the reference
workspace; override `HAKAN`/`TARGET` (the Hakan tree), `STOCK_PICAT`,
`PBSOL` (the PB solver binary), `PICAT`/`BIN` (this build), `TIMEOUT`,
`TIMEOUT_PB`.

- `divergence_check.sh [light|detailed]` — sequential sweep, this build
  vs the stock binary, Hakan tree only. `light` (default) compares
  return codes; `detailed` also compares the outputs with
  timing/solver noise stripped. Verdicts: `DIVERGES (exp rc=N, stock
  rc=0)` is the regression class; `same (both timeout)` means raise
  `TIMEOUT` and re-run.
- `divergence_reverify.sh TIMEOUT <example.pi> ...` — re-verify the
  flaky candidates of a parallel sweep sequentially with a longer
  timeout (120 s) and an output comparison: the documented follow-up
  for timeout-based `DIVERGES` verdicts. Takes the example paths as
  printed by the sweep (`root/x.pi` = the tree root, `ppl/x.pi` = a
  subdirectory). Each side works on a fresh writable copy with `.qi`
  deleted (a unique per-invocation workdir under the scratch area, so
  no run shares bytecode and NFS stale-file locks cannot build up).
  Verdicts: `same`, `exp-slower` (the stock finished, this build timed
  out), `exp-fails`, `stock-fails`, `stock-timeout`, `both-fail`. Env:
  `EXP`, `STOCK_PICAT`, `HAKAN`, `WORK`.
- `divergence_parallel.sh [light|detailed] [JOBS]` — the same sweep
  through GNU parallel (`JOBS` in flight, default 32), plus the `pb`
  examples single-sided on this build. Needs GNU parallel and ≥90
  physical cores for the core pinning.
- `fdn_sweep.sh [JOBS]` — the fdn transparency sweep: every Hakan
  example (plus `pb`) runs twice on the SAME binary, with the fdn hook
  (default) and with `FDN=0` (exactly the stock behavior); compares
  return codes and normalized outputs. Needs GNU parallel.
- `fdn_exs.sh [JOBS]` — the same fdn transparency sweep over all `exs`
  examples instead of the Hakan tree (the `pb` and `satext` examples run
  with their environments). Needs GNU parallel.
- `fdn_speedup.sh [JOBS]` — the FDN=1 vs FDN=0 wall-time benchmark over
  every example that imports cp or cp2 (exs/ root+cp+cp2 and the Hakan
  tree root+ppl+nn_hakank). Each side runs twice, the minimum kept (the
  first run's one-time .qi compile does not pollute the timing). Reports
  per-example times, the speedup (t_FDN0/t_FDN1) and flags every example
  where FDN=1 is slower. `FDN_THREADS=N` (in the environment) bounds the
  fdn's worker pool per process for batch runs; `TIMEOUT=N` (default 60)
  bounds one run. Needs GNU parallel.

Every script exits 0 only when there is no divergence (for the
transparency sweeps: no `RC-DIFFERS` and no `OUTPUT-DIFFERS`).

### Last verified results (reference workspace)

- Hakan tree, divergence sweep (detailed): no `DIVERGES`, the flaky
  candidates were re-verified sequentially with a 120 s timeout.
- Hakan tree, fdn transparency sweep: 3132/3132 identical
  (fdn-on vs `FDN=0`).
- exs, fdn transparency sweep: all families identical.

## 3. The webasm build headless (repo)

- `webasm/headless/run_pi.js <example.pi>` — one-shot runner: boots a
  fresh interpreter module (like a page load) and runs one `.pi` file
  under node, with timing.
- `webasm/headless/compare.py` — drives all 100 packed examples on both
  the native build (`emu/picat`) and the wasm build, prints the
  comparison and dumps a CSV (`webasm/headless/results.csv`).

Prerequisites: `cd webasm && make` (→ `dist/`: picat.js, picat.wasm,
picat.data) and `cd emu && make -f Makefile.linux64`; node and python3
(stdlib only) on `PATH`. The wasm build must be rebuilt after C changes
or the comparison tests a stale `dist/`.

## 4. Which suite to run after what

| change | suites to run |
|---|---|
| fdn C code (`emu/fdn.c`, `fdn.h`) | `exs/fd_native_mt/dev_check_312.sh`, `test/fdn_sweep.sh`, `test/fdn_exs.sh`, `exs/fd_native_mt/ext_check.sh` |
| external-solver code (`fdn_solver.cpp`, `fdn_cpsat.py`, `fdn_hook.pi`) | `exs/fd_native_mt/ext_check.sh`, `test/fdn_exs.sh` |
| the reified library (`lib/global_reif.pi`) | `test/reif_globals_check.sh` (the truth-value equivalence + faithfulness of the reified globals), the 4 `exs/*/reif_*` examples; the wasm preload needs a relink (`webasm/dist` carries a copy) |
| the pb library (`lib2/pb.pi`, `emu/satext.c`) | `test/pb_families_bench.sh` (the five families under pb/cp/sat), `exs/pb/pb_test.pi` (the battery), `exs/pb/php_bench.pi` + `exs/pb/parity.pi` (the head-to-heads) |
| the fp library (`lib2/fp.pi`) | `test/fp_check.sh` (composition over function-terms, mapped in one pass), `exs/fp/elegant.pi` (the worked example: Newton's method, a derivative operator, an integral, a pipeline) |
| any other C change (`emu/*.c`) | `exs/deviation_check.sh`, `webasm/headless/compare.py` (after rebuilding `dist/`) |
| shell tools (`tools/`) | `tools/test_picat_run.sh` |

## 5. Verification probes in this folder

- `udfexp/` — 26 `.pi` experiments (exp1-17, plus probes) that pin down
  the udf `$`-term semantics of `lib2/udf.pi`: one `$` per expression
  suffices (an unmarked nested call inside a `$`-term behaves exactly
  like the marked form), a `$`-term cannot follow an operator (exp13),
  the multi-call form is one `$`-term `$(f+g)`, 0-arity functions are
  impossible, and the interaction of `$when`/`$nwhen` with `#=`. Each
  file is standalone: run with `PICATPATH=.:./lib2 emu/picat
  test/udfexp/expN.pi` and check the printed claim. `exp14`/`exp15`
  back up the udf doc corrections.
- `wasmshow/show.js` — a stdout-passing variant of
  `webasm/headless/run_asp_pi.js` for the aspic (embedded `asp...end`)
  two-shot flow on the webasm runtime under node; that variant runs the
  generated program with its stdout swallowed, this one shows the
  solution line. Usage: `node test/wasmshow/show.js <file.pi>`.
