# picat-fdn: transparent native multicore FD solver

The engine in `Picat-src/emu/fdn.c`, `fdn.h` and `fdn_solver.cpp` is the
stock Picat emulator plus a native C++ search engine for CP models.
Programs run unchanged: `picat prog.pi args`. Every `solve/1,2` of the `cp`
module checks the constraint network that is live at that moment:

- **Supported** (see below): the whole search runs natively on all cores.
  The solutions come back to Picat one at a time, in exactly the order
  Picat's own labeling would produce them, and `statistics(backtracks)`
  advances by exactly Picat's count.
- **Anything else:** Picat's own labeling runs, unchanged.

## Build, run, test

```sh
cd emu && make -f Makefile.linux64 -j8 picat FDN_ARCH=-mpopcnt
cd .. && emu/picat exs/fdn/mixed.pi               # transparency test
emu/picat exs/cp/queens.pi                        # any cp program
FDN=0 emu/picat exs/cp/queens.pi                  # hook off: exactly stock
```

`FDN_ARCH=-mpopcnt` is for x86_64; omit it on other platforms. `fdn_hook.pi`
must sit next to the binary (it does, in `emu/`); `FDN_HOOK=path` overrides
this.

Environment variables:

| variable | effect |
|---|---|
| `FDN=0` | do not install the hook (exactly stock Picat) |
| `FDN_THREADS=n` | threads per search (default: all cores) |
| `FDN_VERBOSE=1` | report each `solve` on stderr: native search (size) or the fallback reason |
| `FDN_SPAWN=n` | start the thread pool after n nodes (default 20000; small searches stay on the calling thread) |

## Supported propagators

| Picat source | native propagator |
|---|---|
| `X #!= Y`, `X #!= Y+C`, `abs(X-Y) #!= C` | forward checking |
| `all_different` | forward checking |
| `all_distinct` | FC + Hall check anchored at the changed variable |
| `all_distinct` on a permutation | + primal/dual channelling |
| `sum`, `#=`, `#>=`, `#=<` (linear) | bounds consistency; ARC sums reach arc consistency once ≤ 2 variables are unbound |
| `X #= Y+C`, `X #= C-Y` | value mapping (arc consistent) |

Labeling options: `[]` / `leftmost`, `ff`, `ffc`, `up`. Everything else
(other propagators such as `element`, `circuit`, `table`, `#\/`,
reification; other attributes; other labeling options; value ranges wider
than 65536) falls back to Picat's labeling.

## `exs/fdn/mixed.pi`

The transparency test: each block prints solutions / counts and the
`backtracks` statistic, which must be identical to stock Picat. Run it with
and without `FDN=0` and compare. It covers findall / count_all
enumeration, ARC and general linear constraints, binary equalities, `ffc`,
partially instantiated label lists, `element` with a constant index, and a
fallback case (`circuit`).

## Limits and known differences

- Solutions are transferred to Picat one at a time by unification, which
  runs Picat's own propagators. That keeps every non-label variable exactly
  as stock Picat leaves it, but it costs a few µs per solution.
- First-solution searches gain from native code only (up to ~8× observed);
  threads help when the solution lies right of large failing subtrees.
- Bool-sum heavy models (pigeon 0/1) are about 1.35× slower than Picat on
  one thread; the parallel speedup more than makes up for it.
- One `backtracks` quirk is not reproduced: stock Picat reports some
  retries of an exhausted `count_all` over a queens search only at the next
  `solve`; picat-fdn does not add these.
- Loading `fdn_hook.pi` at start-up shifts the internal heap and symbol
  layout. Flows that never call `labeling/2` (e.g. `sat`/`sat2` programs)
  are unaffected logically, but a solver that returns one of several valid
  solutions may return a different one (observed on
  `exs/sat/marriage_roman_sat.pi`: a different, equally valid stable
  matching). `FDN=0` removes the load entirely.
