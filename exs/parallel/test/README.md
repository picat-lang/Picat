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
| `test_race.sh` | runs all three at the relevant NT values, asserts `PASS` + no segfault / no `uncaught exception`, and re-checks a count-all mode-2 example (`queens_count 10 4 = 724`) is unaffected. |

## Run

Requires the rebuilt engine (with `bp.pvm_fork_rw`):

```
cd <Picat-src>
cd emu && make -f Makefile.linux64 picat && cd ..
bash exs/parallel/test/test_race.sh
```

Each `.pi` is self-checking (prints `PASS: ...` or `FAIL: ...` and
exits non-zero on failure), so they can also be run individually, e.g.:

```
PICATPATH=lib2 emu/picat exs/parallel/test/race_par_block.pi 2
```
