# PVM examples — achieved accelerations and run commands

fork-based OR-parallel CP search (`parsearch` branch; `emu/parvm.c`): a
Picat program wraps its existing CP search in the `bp.pvm_*` builtins
(`fork / delegate / worker_id / chunk / claim / report / collect /
solution`) and the facility distributes the search over up to 4096
fork()-ed copy-on-write workers. The only inter-process state is one
POSIX shared-memory block plus `waitpid` — see
`../README.md` for the protocol reference.

## Running

From the repository root (a `parsearch` build, `emu/picat`):

```sh
export PICATPATH=$PWD/lib2
picat exs/parallel/pvm/<model>.pi [args]        # or: emu/picat ...
```

Arguments are positional (`[N] [NT] ...`); the Ramsey/backpack
drivers read `K`/`N`/`T`/`M`/`O`/`DYN`/`NT` from the **environment**
(shown inline below); numbers are from two batches on the
shared 384-thread dual-socket EPYC node, `-O3`, this tree: a quieter one and a hot one (2026-08-28
remeasurement, node load average ~100–560), which inflated the big
runs 20–30 % and — for the N=479 first-solution case — erased the
speedup entirely (see the table: it is load-sensitive). Long runs
(ramsey) take minutes — run them under `timeout` or in the background.

## Achieved accelerations (method, mode, parameters)

| Problem / test case | Method & mode | Parameters | Serial | Parallel | Speedup |
|---|---|---|---|---|---|
| n-Queens first solution, N = 479 | mode 3 — value-chunked search | NT = 16, C = 16 (`queens_first.pi 479 16 3 16`) | 6.90–8.13 s (quiet node) / 4.78–4.86 s (hot node, 2026-08-28 remeasurement) | 5.27 s quiet (battery best cells 4.92–5.49 s) / 4.87–5.31 s hot | **1.14–1.54× on the quiet node; load-sensitive — ~1.0× (flat) on the hot node**. The parallelizable work is only the failing Q4 prefix; when the serial baseline is fast there is no gain left to take |
| same, mode 1 (C = 1 OR split) | mode 1 | NT = 8/16 (`queens_first.pi 479 16 1 1`) | — | 16.2–17.8 s quiet / 9.3 s hot | 0.51× hot (0.46–0.50× quiet) — slower than serial in both batches (fork-per-value tax) |
| n-Queens counting, N = 16 | mode 2 — static value partition, all-results | NT = 16 (= N values) (`queens_count.pi 16 16`) | 229.5 s (quiet) / 228.6 s (hot) | 15.6 s, exact 14,772,512 (both batches) | **14.7× / 14.6×** (7.2× at NT = 8, 31.7 s hot; flat at NT = 32) |
| n-Queens joint partition, 17×17 pairs | mode 2 over the joint value Q[1]+N(Q[2]−1) | 289 = 17×17 workers (`queens_count2.pi 17 289`) | — | exact 95,815,104 | full-space count, 289 live workers |
| 0/1 knapsack, N = 80 / N = 100 | mode 4, **banded** bound-lift B&B (each round = mode-2 partition of the value range) | NT = 16 (`N=80 NT=16 backpack_band.pi`) | 2543 / 8213 ms (NT = 0, the built-in `$max`; hot-node remeasurement — 2508/8187 ms quiet) | 1094 / 4047 ms (1070/4011 ms quiet) | **2.33× / 2.03×** (best of 3 each batch; 2.34×/2.04× quiet, 75-run battery; heavier earlier batch: 1.71×/1.47×). Family ceiling ≈ 2×: the closing all-empty proof round is serial. |
| (K,K)-Ramsey R(3,3) = 6 | mode 3 SAT + mode-2 UNSAT | T = 2/8/16 (`K=3 N=6 T=8 ramsey_pvm.pi`) | ~2 ms | ~2 ms | both cells proven at every T |
| **R(4,4) ≤ 18** whole-tree UNSAT (153 edge vars) | static mode-2 joint partition (M-bit edge prefixes) | T = 2^M ∈ {16,128,256} (`K=4 N=18 T=256 M=8 ramsey_m2.pi`) | no verdict in 7200 s serial; kissat 4.0.4 via satext: no verdict > 3 h | 1844.5 / 176.9 / 118.3 s (quiet batches); 141.3 s at T = 256 (hot node) | 16× workers → 15.6× wall; >60× the 2 h serial floor |
| same | **dynamic mode-2 chunks** (`DYN=1`: worker pool claiming leaves from the shared `pvm_claim` cursor) | 384 workers, 2^14 leaves (`K=4 N=18 T=384 M=14 DYN=1 ramsey_m2.pi`) | as above | quiet: **53.3 s** @ 384w/2^14, 63.3 @ 256w/2^14, 69.6 / 59.3 @ 256w / 384w × 2^12, 58.3 @ 2^15 (knee); hot: 61.5 / 70.4 / 69.4 / 67.8 s (same cells) | **2.10×** vs the 112.1 s static M=8 of the quiet batch; **2.30×** vs the 141.3 s static of the hot batch (61.5 s, best of 3) |

Reading: counting is the near-linear regime (limited by partition
granularity); first-solution is capped by the serial "failing prefix
then one success path" structure; the R(4,4) dynamic gain combines a
finer partition (which does less total work — splitting the hardest
112 s 8-bit chunk into its 16 twelve-bit leaves gives 1.5–6.0 s leaves
summing to 56.7 s) with near-perfect claim balance (all 384 workers
finish within [51.8, 53.2] s, σ = 0.31 s; wall = ideal balance + 4%).
The static M=8 skew is small (256 chunk costs: median 89.2 s, top-5
share 2.7% of the ~20.5 k s total), so the cursor's skew-removal term
pays here mostly by making the depth cheap — a static 14-bit grid
would need 16,384 workers (one per leaf), above the 4096 cap. A
full-batch remeasurement on the hot node (2026-08-28) reproduced the
verdicts and the relative ordering: 2.30× there versus 2.10× quiet.

## Reproducing each row

```sh
# 1. first solution, N=479 (serial baseline vs best mode-3 cell)
picat exs/parallel/pvm/queens_first.pi 479                # serial: ~4.8 s hot / 7-8 s quiet node
picat exs/parallel/pvm/queens_first.pi 479 16 3 16        # mode 3, NT=16, C=16, PIN=0: ~4.9-5.5 s
picat exs/parallel/pvm/queens_first.pi 479 16 1 1         # mode 1 (C=1 OR split): ~9.3 s hot (slower than serial)

# 2. counting, N=16 (near-linear to NT = N; exact OEIS A000170 total)
picat exs/parallel/pvm/queens_count.pi 16 16              # ~15.6 s -> 14772512
picat exs/parallel/pvm/queens_count.pi 16 8               # ~32 s hot (62 s in the first batch)
picat exs/parallel/pvm/queens_count.pi 16 0               # serial: ~229 s
# joint partition over the first two queens (up to N*N workers):
picat exs/parallel/pvm/queens_count2.pi 17 289            # -> 95815104
picat exs/parallel/pvm/queens_count2.pi 8 35              # worked example -> b = 92

# 3. knapsack (env-driven; NT=0 = serial built-in $max reference)
NT=0  N=100 picat exs/parallel/pvm/backpack_band.pi       # serial reference
NT=16 N=100 picat exs/parallel/pvm/backpack_band.pi       # banded mode-4 rounds: ~2.04x at N=100
NT=16 N=80  picat exs/parallel/pvm/backpack_band.pi       # ~2.34x at N=80
NT=16 N=100 picat exs/parallel/pvm/backpack_pvm.pi        # same problem, mode-1 lifting protocol

# 4. (K,K)-Ramsey cells (env K =, N =, T = worker count)
K=3 N=6 T=8  picat exs/parallel/pvm/ramsey_pvm.pi         # UNSAT ~2 ms; with K=3 N=5 SAT -> R(3,3)=6
K=4 N=17 T=8 picat exs/parallel/pvm/ramsey_pvm.pi         # SAT (~0.1-0.5 s): R(4,4) >= 18
K=4 N=16 T=8 picat exs/parallel/pvm/ramsey_pvm.pi         # SAT: the 8-regular critical graph

# 5. R(4,4) <= 18 whole-tree UNSAT proof (env K= N= T= M=; ~2-32 min)
K=4 N=18 T=16  M=4  picat exs/parallel/pvm/ramsey_m2.pi   # static: ~31 min (quiet node)
K=4 N=18 T=128 M=7  picat exs/parallel/pvm/ramsey_m2.pi   # static: ~3 min quiet
K=4 N=18 T=256 M=8  picat exs/parallel/pvm/ramsey_m2.pi   # static: 112.1 s quiet / 141.3 s hot node
# dynamic chunks: T = pool size, 2^M leaves claimed from the shared cursor
K=4 N=18 T=256 M=12 DYN=1 picat exs/parallel/pvm/ramsey_m2.pi   # ~60-70 s
K=4 N=18 T=384 M=12 DYN=1 picat exs/parallel/pvm/ramsey_m2.pi   # ~59-69 s
K=4 N=18 T=256 M=14 DYN=1 picat exs/parallel/pvm/ramsey_m2.pi   # ~63-71 s
K=4 N=18 T=384 M=14 DYN=1 picat exs/parallel/pvm/ramsey_m2.pi   # ~53-64 s  <- best
K=4 N=18 T=384 M=15 DYN=1 picat exs/parallel/pvm/ramsey_m2.pi   # ~58-68 s (the knee)
# window offset O (patterns O+1..O+T) for probing sub-regions, e.g. the
# 16 twelve-bit leaves of 8-bit pattern 170 (the batch's hardest chunk):
K=4 N=18 T=16 M=12 O=2720 picat exs/parallel/pvm/ramsey_m2.pi   # max leaf ~6 s

# 6. report-codec capability check (any ground term crossing the boundary)
picat exs/parallel/pvm/term_report.pi                     # -> 1 / <term> / pass
```

## Parameter reference (per sample)

| file | interface | meaning |
|---|---|---|
| `queens_first.pi` | args `[N] [NT] [MODE] [C] [PIN]` (defaults `10 0 3 1 0`) | N board size; NT worker budget (0 = serial); MODE 1 = C=1 OR split, 3 = value chunks; C chunk size (mode 3); PIN fixes Q[1] (0 = free) |
| `queens_count.pi` | args `[N] [NT]` (defaults 10, serial) | all-results count over the N values of Q[1]; sweet spot NT = N |
| `queens_count2.pi` | args `[N] [NT]` (defaults 10, serial) | count over the joint value Q[1]+N(Q[2]−1) in 1..N·N; up to N·N workers useful |
| `ramsey_pvm.pi` | env `K= N= T=` (defaults 3, 6, 0=serial) | mode-3 first solution of the (K,K)-Ramsey graph on N vertices; SAT prints + root-verifies the graph, UNSAT proves R(K,K) ≤ N |
| `ramsey_m2.pi` | env `K= N= T= M= O= DYN=` (defaults 3, 6/N-for-K=3, 0, 0, 0, 0) | mode-2 joint partition of the first M edge variables; static: T = 2^M workers, one 2^M-bit pattern each (window O+1..O+T, O ≥ 0); dynamic (DYN=1): T workers claim all 2^M patterns (window O+1..O+2^M) from the shared `pvm_claim` cursor, instance built once per worker, per-claim prefix units backtracked; every reported leaf must arrive (length check) and every worker must be alive at collection |
| `backpack_pvm.pi` | env `N= NT=` (defaults 20, 0=serial `$max`) | mode-4 B&B: each iteration a re-armed mode-1 session racing `model + P #>= B+1`; first reporter lifts the bound by value; the exhausting session proves optimality |
| `backpack_band.pi` | env `N= NT=` (defaults 20, 0) | mode-4 variant: each lift round is one re-armed mode-2 session (`pvm_fork_lb`) partitioning B+1..U into NT value bands; an all-empty round proves B optimal; NT=1 degenerates to `backpack_pvm`'s loop |
| `term_report.pi` | none | worker reports a nested mixed ground term (ints/atoms/bool/float/bigint/lists/compounds/array); root re-materializes and verifies field by field |

Every negative verdict above runs on a live session: `pvm_collect`
refuses the result (hard `run_time_error`) if any worker died, so an
"UNSAT" / "optimal" line is trusted only when every worker reported.
Full validation matrix in the engineering report
(`docs/fokus_report.tex`, §Verification).
