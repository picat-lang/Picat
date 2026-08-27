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
(shown inline below). Numbers below are one batch each on the
shared 384-thread dual-socket EPYC node, `-O3`, this tree; wall times
of shared-machine runs vary a few percent per batch (ranges noted).
Long runs (ramsey) take minutes — run them under `timeout` or in the
background.

## Achieved accelerations (method, mode, parameters)

| Problem / test case | Method & mode | Parameters | Serial | Parallel | Speedup |
|---|---|---|---|---|---|
| n-Queens first solution, N = 479 | mode 3 — value-chunked search | NT = 16, C = 16 (`queens_first.pi 479 16 3 16`) | 8.13 s (best of 3; 27-run battery: 6.90–7.03 s) | 5.27 s (battery best cells 4.92–5.49 s) | **up to 1.54×** across the battery 1.14–1.40× |
| same, mode 1 (C = 1 OR split) | mode 1 | NT = 8/16 (`queens_first.pi 479 16 1 1`) | — | 16.2–17.8 s | 0.46–0.50× (fork-per-value tax) |
| n-Queens counting, N = 16 | mode 2 — static value partition, all-results | NT = 16 (= N values) (`queens_count.pi 16 16`) | 229.5 s | 15.6 s (exact 14,772,512) | **14.7×** (3.7× at NT = 8; flat at NT = 32) |
| n-Queens joint partition, 17×17 pairs | mode 2 over the joint value Q[1]+N(Q[2]−1) | 289 = 17×17 workers (`queens_count2.pi 17 289`) | — | exact 95,815,104 | full-space count, 289 live workers |
| 0/1 knapsack, N = 80 / N = 100 | mode 4, **banded** bound-lift B&B (each round = mode-2 partition of the value range) | NT = 16 (`N=80 NT=16 backpack_band.pi`) | 2508 / 8187 ms (NT = 0, the built-in `$max`) | 1070 / 4011 ms | **2.34× / 2.04×** (best of 3, 75-run final battery; heavier earlier batch: 1.71×/1.47×). Family ceiling ≈ 2×: the closing all-empty proof round is serial. |
| (K,K)-Ramsey R(3,3) = 6 | mode 3 SAT + mode-2 UNSAT | T = 2/8/16 (`K=3 N=6 T=8 ramsey_pvm.pi`) | ~2 ms | ~2 ms | both cells proven at every T |
| **R(4,4) ≤ 18** whole-tree UNSAT (153 edge vars) | static mode-2 joint partition (M-bit edge prefixes) | T = 2^M ∈ {16,128,256} (`K=4 N=18 T=256 M=8 ramsey_m2.pi`) | no verdict in 7200 s serial; kissat 4.0.4 via satext: no verdict > 3 h | 1844.5 / 176.9 / 118.3 s | 16× workers → 15.6× wall; >60× the 2 h serial floor |
| same | **dynamic mode-2 chunks** (`DYN=1`: worker pool claiming leaves from the shared `pvm_claim` cursor) | 384 workers, 2^14 leaves (`K=4 N=18 T=384 M=14 DYN=1 ramsey_m2.pi`) | as above | **53.3 s**; 63.3 s @ 256w/2^14; 69.6 / 59.3 s @ 256w, 384w × 2^12; 58.3 s @ 2^15 (knee) | **2.10×** vs the 112.1 s static M=8 wall of the same batch (2.22× vs a prior batch's 118.3 s) |

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
would need 16,384 workers (one per leaf), above the 4096 cap.

## Reproducing each row

```sh
# 1. first solution, N=479 (serial baseline vs best mode-3 cell)
picat exs/parallel/pvm/queens_first.pi 479                # serial, ~7-8 s
picat exs/parallel/pvm/queens_first.pi 479 16 3 16        # mode 3, NT=16, C=16, PIN=0: ~5 s
picat exs/parallel/pvm/queens_first.pi 479 16 1 1         # mode 1 (C=1 OR split): ~16 s (slower)

# 2. counting, N=16 (near-linear to NT = N; exact OEIS A000170 total)
picat exs/parallel/pvm/queens_count.pi 16 16              # ~15.6 s -> 14772512
picat exs/parallel/pvm/queens_count.pi 16 8               # ~62 s
picat exs/parallel/pvm/queens_count.pi 16 0               # serial: ~230 s
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
K=4 N=18 T=16  M=4  picat exs/parallel/pvm/ramsey_m2.pi   # static: ~31 min
K=4 N=18 T=128 M=7  picat exs/parallel/pvm/ramsey_m2.pi   # static: ~3 min
K=4 N=18 T=256 M=8  picat exs/parallel/pvm/ramsey_m2.pi   # static: ~2 min (batch 2: 112.1 s)
# dynamic chunks: T = pool size, 2^M leaves claimed from the shared cursor
K=4 N=18 T=256 M=12 DYN=1 picat exs/parallel/pvm/ramsey_m2.pi   # ~70 s
K=4 N=18 T=384 M=12 DYN=1 picat exs/parallel/pvm/ramsey_m2.pi   # ~59 s
K=4 N=18 T=256 M=14 DYN=1 picat exs/parallel/pvm/ramsey_m2.pi   # ~63 s
K=4 N=18 T=384 M=14 DYN=1 picat exs/parallel/pvm/ramsey_m2.pi   # ~53 s  <- best
K=4 N=18 T=384 M=15 DYN=1 picat exs/parallel/pvm/ramsey_m2.pi   # ~58 s (the knee)
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
