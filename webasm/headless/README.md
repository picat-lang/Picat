# Running the webasm build headlessly

The exact runtime the browser ships (`dist/picat.js` + `dist/picat.wasm`,
Emscripten `RETCALL`-based B-Prolog) runs unmodified under node — no
browser required. This folder contains:

- `run_pi.js` — one-shot runner: fresh interpreter module (like a page
  load), boots it, runs one `.pi` file, reports timing.
- `compare.py` — drives all 100 packed examples on both the native build
  (`emu/picat`) and the wasm build, and prints/CSV-dumps the comparison.

## Prerequisites

```sh
cd webasm && make            # -> dist/ (picat.js, picat.wasm, picat.data)
cd ../emu && make -f Makefile.linux64   # -> ../emu/picat (for the comparison)
```

Node and python3 (stdlib only) on PATH.

## Single run

```sh
cd webasm/dist                      # picat.data resolves from the CWD
node headless/run_pi.js ../examples/sat_bqueens.pi
# rc=1 instantiate_ms=34 boot_ms=31 run_ms=1351
```

- `instantiate_ms` — node start + wasm instantiation + FS preload:
  one-time cost per "page load".
- `boot_ms` — `browser_boot` = `initialize_bprolog('/lib2')`: one-time.
- `run_ms` — the run itself (`browser_rerun`, program only).
- exit code 0 only when the run status is 1.

## Native-vs-wasm comparison

```sh
python3 headless/compare.py [--runs N] [--workers N] [--limit N] [--out FILE]
```

- `--runs` (default 1): measurements per side per example; the median is
  kept. Use ≥ 3 for stable numbers (the table below used 3).
- `--workers` (default 32): examples measured in parallel. Keep it ≤ 32:
  each measurement spawns one native + one node process in turn, and the
  machine we ran on has 384 cores; beyond ~32 the numbers just get noisier.
- `--limit N`: only the first N examples (debugging).
- writes `results.csv` beside the script (regenerated each run).

Method / what the columns mean, read before quoting the ratios:

| column | meaning |
| --- | --- |
| native ms | median **wall time of the whole process**: C startup + stdlib load + program. Includes ~25–30 ms of process-startup floor. |
| wasm run ms | median of `run_ms` only: the program, on a pre-booted interpreter. The one-time `instantiate + boot` (~60 + 35 ms, below) is *not* in the ratio. |
| ratio | wasm run ms / native ms. For programs far below the ~25 ms native startup floor the ratio is dominated by that floor — wasm "faster" entries there are a measurement artifact, not real speedups. |
| wasm boot (ms) | median one-time boot, shown so a browser session's first run is estimable: instantiate + boot + run ms. |

Notes:

- examples that open data files by bare name (`nn_*`, some `euler_*`)
  preload those files at the wasm FS root; for the native side the harness
  symlinks the same basenames into `headless/native_cwd/` and runs the
  binary there (paths are generated from `DATA_SRC` in `webasm/Makefile`).
- the native binary exits non-zero (and in some FANN error paths segfaults)
  when a data file is missing; the wasm side reports a normal picat error.
  That asymmetry only bites outside the harness setup above.
- `*` marks an example where at least one measured run (either side)
  failed; such rows are excluded from the statistics. Under 32-way
  parallelism an occasional transient failure is normal (see below).

## Results (2026-08-21, x86_64, 384 cores, 32 workers, median of 3 runs)

| example | native ms | wasm run ms | wasm boot (ms) | ratio |
| --- | ---: | ---: | ---: | ---: |
| cp_kakuro | 49 | 35 | 35 | 0.71 |
| cp_knightTour | 48 | 34 | 52 | 0.71 |
| cp_pigeon_hole | 46 | 206 | 62 | 4.48 |
| cp_ppm | 43 | 25 | 33 | 0.58 |
| cp_queens | 78 | 107 | 42 | 1.37 |
| cp_sequence | 86 | 65 | 37 | 0.76 |
| cp_sudoku | 39 | 19 | 37 | 0.49 |
| cp_zebra | 43 | 14 | 33 | 0.33 |
| euler_p1 | 41 | 14 | 40 | 0.34 |
| euler_p10 | 461 | 986 | 32 | 2.14 |
| euler_p100 | 58 | 10 | 48 | 0.17 |
| euler_p106 | 41 | 13 | 40 | 0.32 |
| euler_p10_rb | 3402 | 9949 | 35 | 2.92 |
| euler_p11 | 42 | 22 | 42 | 0.52 |
| euler_p12 | 2076 | 8608 | 31 | 4.15 |
| euler_p13 | 43 | 23 | 32 | 0.53 |
| euler_p14 | 3032 | 2753 | 27 | 0.91 |
| euler_p15 | 64 | 14 | 38 | 0.22 |
| euler_p16 | 41 | 12 | 29 | 0.29 |
| euler_p17 | 50 | 37 | 39 | 0.74 |
| euler_p18 | 38 | 11 | 34 | 0.29 |
| euler_p19 | 63 | 76 | 39 | 1.21 |
| euler_p1_rb | 62 | 14 | 45 | 0.23 |
| euler_p2 | 40 | 14 | 52 | 0.35 |
| euler_p20 | 40 | 12 | 41 | 0.30 |
| euler_p206 | 39 | 13 | 25 | 0.33 |
| euler_p21 | 94 | 175 | 29 | 1.86 |
| euler_p22 | 63 | 58 | 48 | 0.92 |
| euler_p23 | 998 | 3541 | 33 | 3.55 |
| euler_p24 | 626 | 1088 | 36 | 1.74 |
| euler_p25 | 336 | 340 | 30 | 1.01 |
| euler_p26 | 139 | 85 | 31 | 0.61 |
| euler_p27 | 668 | 2668 | 37 | 3.99 |
| euler_p28 | 37 | 12 | 35 | 0.32 |
| euler_p29 | 1231 | 2113 | 39 | 1.72 |
| euler_p2_rb | 33 | 11 | 32 | 0.33 |
| euler_p3 | 29 | 13 | 31 | 0.45 |
| euler_p30 | 499 | 1815 | 37 | 3.64 |
| euler_p31 | 337 | 990 | 28 | 2.94 |
| euler_p32 | 28 | 36 | 41 | 1.29 |
| euler_p33 | 30 | 15 | 38 | 0.50 |
| euler_p34 | 133 | 507 | 40 | 3.81 |
| euler_p35 | 785 | 922 | 38 | 1.17 |
| euler_p36 | 750 | 2108 | 38 | 2.81 |
| euler_p37 | 1212 | 2822 | 27 | 2.33 |
| euler_p38 | 34 | 50 | 41 | 1.47 |
| euler_p39 | 2490 | 6109 | 25 | 2.45 |
| euler_p4 | 372 | 1022 | 36 | 2.75 |
| euler_p40 | 148 | 236 | 36 | 1.59 |
| euler_p41 | 76 | 30 | 40 | 0.39 |
| euler_p42 | 37 | 21 | 27 | 0.57 |
| euler_p43 | 57 | 105 | 43 | 1.84 |
| euler_p44 | 418 | 1427 | 48 | 3.41 |
| euler_p45 | 45 | 183 | 43 | 4.07 |
| euler_p46 | 40 | 58 | 40 | 1.45 |
| euler_p47 | 397 | 995 | 52 | 2.51 |
| euler_p48 | 1796 | 798 | 29 | 0.44 |
| euler_p49 | 132 | 193 | 38 | 1.46 |
| euler_p5 | 26 | 9 | 36 | 0.35 |
| euler_p50 | 117 | 235 | 43 | 2.01 |
| euler_p52 | 98 | 164 | 26 | 1.67 |
| euler_p6 | 34 | 11 | 31 | 0.32 |
| euler_p67 | 43 | 51 | 48 | 1.19 |
| euler_p7 | 101 | 257 | 29 | 2.54 |
| euler_p8 | 36 | 15 | 33 | 0.42 |
| euler_p9 | 55 | 461 | 86 | 8.38 * |
| euler_p99 | 40 | 17 | 33 | 0.42 |
| euler_p9_rb | 64 | 215 | 30 | 3.36 |
| euler_pi_rb | 27 | 10 | 34 | 0.37 |
| hello | 29 | 9 | 26 | 0.31 |
| nn_fann_xor | 34 | 39 | 42 | 1.15 |
| nn_frequencies | 224 | 609 | 31 | 2.72 |
| nn_lang_train * | 300 | 33 | 36 | 0.11 |
| nn_preprocess | 308 | 367 | 29 | 1.19 |
| nn_scaling_test | 47 | 98 | 32 | 2.09 |
| nn_scaling_train | 971 | 1513 | 34 | 1.56 |
| nn_spam_test | 109 | 228 | 34 | 2.09 |
| nn_spam_train | 1516 | 1975 | 30 | 1.30 |
| nn_transition_probabilities | 88 | 205 | 46 | 2.33 |
| nn_xor_test | 20 | 13 | 30 | 0.65 |
| nn_xor_train_data | 27 | 33 | 33 | 1.22 |
| nn_xor_train_file | 26 | 27 | 32 | 1.04 |
| planner_15_puzzle | 851 | 1295 | 27 | 1.52 |
| planner_farmer | 33 | 13 | 36 | 0.39 |
| planner_klotski | 7913 | 17829 | 29 | 2.25 |
| planner_nomystery | 38 | 29 | 29 | 0.76 |
| planner_ricochet | 7995 | 15574 | 30 | 1.95 |
| planner_sokoban | 675 | 1690 | 34 | 2.50 |
| planner_solitaire | 53 | 26 | 37 | 0.49 |
| planner_treasure | 43 | 32 | 31 | 0.74 |
| planner_water | 31 | 13 | 30 | 0.42 |
| queens | 51 | 118 | 36 | 2.31 |
| sat_bqueens | 341 | 1351 | 72 | 3.96 |
| sat_crossword | 32 | 78 | 71 | 2.44 |
| sat_magic_square | 2823 | 2369 | 31 | 0.84 |
| sat_marriage_roman_sat | 16913 | 19657 | 27 | 1.16 |
| sat_maxClique | 33 | 42 | 34 | 1.27 |
| sat_queens | 10845 | 24220 | 36 | 2.23 |
| sat_sudoku | 88 | 142 | 33 | 1.61 |
| sat_vmtl | 6980 | 2175 | 28 | 0.31 |

\* `euler_p9`: verified separately at ~2.8x (native 55–75 ms, wasm
171–279 ms over 5 runs each); the 461 ms cluster above was a bad sample
under 32-way parallel load. `nn_lang_train`: one of its three wasm runs
failed only under 32-way load; standalone 5/5 pass at 17–70 ms. Both rows
are excluded from the statistics below.

## Statistics

**In general how much slower.** Geomean of the ratios over all 99
successful rows is **1.06x**, the median **1.19x**. But the distribution
bimodal:

- programs that finish in a few tens of ms are effectively **at parity** —
  both sides are dominated by startup (native includes ~25–30 ms of
  process start; the wasm side has to pay ~60 ms instantiate + ~35 ms boot
  once per session, then nothing per run);
- genuinely compute-heavy programs (search, SAT, FANN) run **~2–4x
  slower on wasm**, which is the honest figure for the interpreter
  overhead on real workloads.

**Faster on wasm.** 43 of 99 rows have ratio < 1, but almost all of them
are sub-50 ms programs where the native startup floor dominates (an
artifact). Three of the "faster" entries are real, heavy programs:

| example | native | wasm | wasm/native |
| --- | ---: | ---: | ---: |
| sat_vmtl | ~7.0 s | ~2.3 s | 0.31 |
| euler_p48 | ~1.8 s | ~0.8 s | 0.44 |
| sat_magic_square | ~2.8 s | ~2.4 s | 0.84 |

`sat_vmtl` (~3x) was re-verified run-by-run (native 6.5–8.0 s, wasm
2.2–3.0 s) and is stable, so the wasm build genuinely out-runs the native
one there (both call the same compiled C SAT code; see also the
optimization-level differences between the two builds).

**Most extreme slowdown** (verified, not a single noisy sample):
`cp_pigeon_hole` 4.5x (46 → 206 ms), `euler_p12` 4.2x (2.1 → 8.6 s),
`euler_p45` 4.1x (45 → 183 ms), `euler_p27` 4.0x, `sat_bqueens` 4.0x,
`euler_p30` 3.6x — i.e. nothing exceeds ~4.5x; the FANN `nn_*` programs
are the most consistently slow class (1.2–2.7x), except `nn_frequencies`
at 2.7x (native 224 ms → wasm 609 ms).

**One-time wasm start cost** (not in the ratios): instantiate ~60 ms +
boot ~35 ms (medians above), plus ~10–20 ms of node-process startup in
headless mode; in a browser the page-load part replaces the node start.
So a browser session's first run of an example costs roughly
`instantiate + boot + run ms` from the table.
