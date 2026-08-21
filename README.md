# Experimental branch — what is new versus `main`

This branch (`experimental`) adds features on top of `main`. Each area
has its own README where noted.

## 1. External SAT solvers and the SAT solver portfolio (satext)

`solve`/`solve_all` can now be served by an external SAT solver
(kissat, CaDiCaL, MiniSat, CryptoMiniSat, or any DIMACS/IPASIR solver —
the input protocol is picked from the solver name, probed once and
cached for unknown names). Models come back exactly as with the
built-in solver, so `solve_all`, `findall` and blocking of enumerated
solutions work unchanged. Several solvers may be named at once: they
are **raced** on the same CNF and the first decisive answer wins (the
rest are killed) — the portfolio is capped at 8 solvers per solve. The
built-in solver is still fed, so a failed/`unknown` external run falls
back to it (see `SATEXT_NO_FALLBACK` below).

How to select a solver:

- program option: `solve([$solver(Spec)], Vars)`
- from code: `bp.c_satext_set_solver(Spec)` (e.g. in `main`, before
  the constraints/solve)
- environment: `SATEXT_SOLVER=...` (below)
- low level: `import satext.` (needs `PICATPATH=<picat>/lib2`) —
  `satext.solve(Spec, Clauses, Status, Model)`, `satext.cnf_info`,
  `satext.write_dimacs`; and `bp.c_satext_last_status(St)` reports how
  the last `solve` resolved (1 SAT / 2 UNSAT / 0 unknown-or-abandoned).

`Spec` is an atom or string (`"kissat"`, a path, ...; PATH-resolved
when it has no `/`), a list of atoms/strings forming the solver's argv
(a `@file` token is replaced by a generated CNF file, for solvers that
read the input file as an argument), or a **list of such argv lists**
to race them as a first-wins portfolio. `nil`/`false` selects the
built-in solver again.

Environment variables (all read by the satext layer):

- `SATEXT_SOLVER` — solver selection for the standard `import sat`
  flow. One solver: whitespace-separated argv — first token =
  executable (name or path), the rest = extra arguments, e.g.
  `SATEXT_SOLVER="kissat -t 4"`. A portfolio: `|` separates several
  such argv strings, e.g. `SATEXT_SOLVER="kissat|cryptominisat"`; the
  solvers are raced and the first decisive answer wins (up to 8).
  `nil`/`false` selects the built-in solver.
- `SATEXT_PRT_MIN` — estimated CNF size in bytes below which a
  portfolio collapses to its first solver (default 64 KiB).
- `SATEXT_PRT_BUDGET_MS` — portfolio wall budget in ms per solve
  (default 60000; `0` = no budget). On expiry the race is killed and
  the built-in solver answers (unless `SATEXT_NO_FALLBACK` is set).
- `SATEXT_NO_FALLBACK` — non-empty: when the external solver(s) answer
  `unknown` (e.g. the wall budget elapsed) the built-in solver is
  **not** run and the solve fails instead of answering from it; the
  default is for the built-in solver to answer. It fails *without a
  verdict* — that is not an `unsat` result (`c_satext_last_status`
  reports 0).
- `SATEXT_PRT_STATS` — non-empty: print a per-solve line to stderr
  with each racer's wall time and the winner.
- `SATEXT_SHIM` — path of the `satshim` helper, used to hand a large
  formula to a solver via a file descriptor; default: next to the
  `picat` binary.
- `SATEXT_SHIM_MIN` — estimated CNF size in bytes above which the shim
  path is used instead of a pipe (default 4 MiB).
- `SATEXT_TMPDIR` — directory for generated CNF files (default:
  `/dev/shm`, else `/tmp`).

Usage notes: the comments of `lib/sat.pi` / `lib2/satext.pi` and
`exs/satext/bench_satext.sh`; examples: `exs/satext/`.

## 2. Picat in a browser — new folder `webasm/`

A wasm32 (emscripten) build of the interpreter plus an
editor-and-terminal web page: any Picat program runs in a browser tab,
repeatedly, in one runtime. `import cp.` (CLP/FD) and `import sat.`
(with the built-in kissat solver) work; what does not: external SAT
solvers (fork/exec), `mip`/`smt` (external binaries), `thread`
(pthreads). 100 example programs (packed from `exs/`) are preloaded.

```sh
cd webasm && make && make serve   # needs emsdk + a native build (lib2)
# -> http://localhost:8000 : editor left, terminal right, example select
```

Details (build, run loop, exclusions, example set):
[webasm/README.md](webasm/README.md)

## 3. Concurrency for native Picat — new modules `par`, `thread`, `pp`

- `import par.` — data-parallel aggregates over lists/arrays of
  64-bit integers (wrapping mod 2^64): `par_sum(X)=S`, `par_prod`,
  `par_min`, `par_max`, `par_scan(X)=R` (prefix sums),
  `par_scale(X,S)=R` (parallel map), `par_fib_fast(N)=R`,
  `wall_ms()=T`.
- `import thread.` — real OS threads running registered C worker tasks
  (`sum_range`, `prod_range`, `bump`, `sleep_ms`):
  `T = new_thread(Task,Args); T.start(); join(T); R = result(T);`,
  plus `new_mutex`/`acquire_mutex`/`release_mutex`, semaphores,
  counters, `this_thread()`.
- `import pp.` — a clean functional front end over the two
  (`psum`/`pprod`/`pmin`/`pmax`/`pscan`/`pscale`/`pfib`) with sequential
  baselines (`fib_linear`, `fib_doubling`) for timing comparisons.

The modules live in `lib2/`, so run with:
`PICATPATH=<picat>/lib2 picat yourfile.pi`.
Examples and the benchmark runner `bench_parallel.sh`:
[exs/parallel/README.md](exs/parallel/README.md)

## 4. Picat syntax checker — new folder `lsp/`

`lsp/picat_syntax_check.py FILE.pi [MORE ...]` reports
line/column-accurate syntax diagnostics (unbalanced delimiters,
unterminated strings/quoted atoms/comments, line-spanning string
literals, ...) where the Picat parser would only say `error` — useful
for editor/LSP integration. Self-test:
`python3 lsp/run_mutation_tests.py`.

## 5. Smaller changes

- `.gitignore`: editor backups (`*~`) and `.snapshots/`
- internal: 64-bit bigint constructors (`emu/bigint.c`); the SAT
  interface (`emu/kissat_picat.c`, `emu/cpreds.c`, `emu/common.mak`)
  adjusted to carry the satext layer

__Current version 3.9#11.__

Picat is a simple, and yet powerful, logic-based
multi-paradigm programming language aimed for
general-purpose applications. Picat is a rule-based
language, in which predicates, functions, and actors are
defined with pattern-matching rules. Picat incorporates
many declarative language features for better productivity
of software development, including explicit
non-determinism, explicit unification, functions, list
comprehensions, constraints, and tabling. Picat also
provides imperative language constructs, such as
assignments and loops, for programming everyday things. The
Picat implementation, which is based on a well-designed
virtual machine and incorporates a memory manager that
garbage-collects and expands the stacks and data areas when
needed, is efficient and scalable. Picat can be used for
not only symbolic computations, which is a traditional
application domain of declarative languages, but also for
scripting and modeling tasks. 

Compared with functional and scripting languages, the
support of explicit unification, explicit non-determinism,
tabling, and constraints makes Picat more suitable for
symbolic computations. Compared with Prolog, Picat is
arguably more expressive and scalable: it is not rare to
find problems for which Picat requires an order of
magnitude fewer lines of code to describe than Prolog and
Picat can be significantly faster than Prolog because
pattern-matching facilitates indexing of rules. 

The Picat system is written in both C and Picat. The
package has the following folders and files:
- Picat/README  --   this file
- Picat/doc     --   documentation
- Picat/exs     --   program examples
- Picat/emu     --   the C/C++ source code of the engine
- Picat/lib     --   library modules
- Picat/lib2  --   library modules, not preloaded

The folder "Picat/emu" contains the C source code needed to
make Picat's standalone executable. It also contains the C
source code of Espresso, SAT solvers (maple and lingeling),
which are used by Picat. This folder also contains make files
for different platforms.

The folder "Picat/lib" contains library modules. There are
three types of library modules:

pre-loaded and pre-imported: 
---------------------------
The symbols defined in this type of modules are directly
available to applications, and it's unnecessary to import
them. This type includes: "basic.pi", "io.pi", "math.pi",
and "sys.pi".

pre-loaded but not pre-imported: 
-------------------------------
These modules are included in the executable. However,
applications need to import them in order to access the
functions, predicates, and constraints defined in the
modules. No setting of the environment variable PICATPATH is
required. This type includes: "cp.pi", "mip.pi", "nn.pi",
"os.pi", "planner.pi", "sat.pi", "smt.pi", and "util.pi".

not pre-loaded, and not pre-imported: 
-------------------------------------
These modules, which are mainly developed by third parties,
are not included in the executable. In order to import any
of these modules, applications must set the environment
variable PICATPATH to include the folder, in which the module
resides, or start picat with the option "-path" set.

Please contact:
- picat@picat-lang.org
- picat-lang@googlegroups.com 
