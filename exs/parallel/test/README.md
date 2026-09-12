# exs/parallel/test

Reproducers and regression tests for the parblock **race family** and
the `race_bug` fix (first-wins mode 2, `bp.pvm_fork_rw`).

## Background (`race_bug`)

The original mode-1 portfolio race (`race_begin/race_cl/race_end` and
`race_res`) forked at choice points inside a `bp.pvm_delegate` window
-- an OR-parallel **CP-search** substrate. For a race of independent
deterministic function candidates that substrate re-ran the *first*
candidate in every forked process, so:

- only the first clause/candidate ever ran (fast never started),
- the winner was nondeterministic in NT and carried ~2x overhead,
- a first candidate that could not complete blocked the others (hang).

The fix reimplemented the race family on **first-wins mode 2**
(`bp.pvm_fork_rw`): the fixed-chunk split forks the candidates up
front and runs them concurrently; the first `pvm_report` wins by CAS
on `found`; `pvm_collect` waits for the winner's marker, SIGKILLs the
still-running losers and returns early. Plain mode-2 count-all
(`bp.pvm_fork(NT, 2, M)`, the `par_run`/`par_all`/counting family) is
untouched.

## Files

| file | what it checks |
|------|----------------|
| `race_par_block.pi` | block race (`race_begin/race_cl/race_end`): slow first, fast second. `NT=0/1` serial written-order (slow wins); `NT>=2` **fast wins and the race terminates early**. |
| `race_par_res.pi` | functional `race_res`: same contract. |
| `race_early_kill.pi` | the "hang" case: a never-finishing first candidate + a quick one; quick must win and the loser must be killed (the race returns in ~quick's time, not ~never's). |
| `tsp_race.pi` | **TSP portfolio race** on first-wins mode 2: a deterministic 40-city instance raced by randomized strategies (random multi-restart 2-opt, insertion-start 2-opt, heavy random 2-opt) with per-run random seeds + seed jitter. Checks serial determinism (NT=0/1 = written order), correctness (winner's length equals a serial recompute), **nondeterminism** (>= 2 distinct winners over 12 races at NT=4 -- proves all candidates truly run concurrently, so the race_bug "only the first clause ran" class cannot pass), and no exceptions. |
| `tsp_opts_race.pi` | **Solver-option portfolio**: the same small TSP modelled with `import cp` (`circuit` + `element` + `sum`, solved with `$min`). Each parallel candidate is an option set -- a single labeling option (`ff`, `ffc`, `ffd`, `leftmost`, ...) or a **random combination of two or more options** drawn from a 10-option pool. No artificial pacing -- the solve times differ and that is the point: the first to prove the optimum wins. Checks serial determinism (NT=0/1 = written order), correctness (winner = optimum), race health (no exceptions), and reports each winning option-set plus a per-option win tally (which options help the most). |
| `test_race.sh` | runs the above at the relevant NT values, asserts `PASS` + no segfault / no `uncaught exception`, and re-checks a count-all mode-2 example (`queens_count 10 4 = 724`) is unaffected. |

## Run

Requires the rebuilt engine (with `bp.pvm_fork_rw`):

```
cd <Picat-src>
cd emu && make -f Makefile.linux64 picat && cd ..
bash exs/parallel/test/test_race.sh
```

The race tests compare concurrent finish times (which option wins, the
nondeterminism tally), so `test_race.sh` pins every run -- and, through
the inherited affinity mask, every forked worker -- to **cores 0..89**
with `taskset`.  On a many-core machine without the pinning the timing
results are not reliable (CPU migration / contention shifts the winner).

Each `.pi` is self-checking (prints `PASS: ...` or `FAIL: ...` and
exits non-zero on failure), so they can also be run individually, e.g.:

```
taskset -c 0-89 env PICATPATH=lib2 emu/picat exs/parallel/test/race_par_block.pi 2
```
