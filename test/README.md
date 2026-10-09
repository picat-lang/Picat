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
| any other C change (`emu/*.c`) | `exs/deviation_check.sh`, `webasm/headless/compare.py` (after rebuilding `dist/`) |
| shell tools (`tools/`) | `tools/test_picat_run.sh` |
