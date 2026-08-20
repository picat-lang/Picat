# Experimental branch — what is new versus `main`

This branch (`experimental`) adds features on top of `main`. Each area
has its own README where noted.

## 1. Picat in a browser — new folder `webasm/`

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

## 2. Concurrency for native Picat — new modules `par`, `thread`, `pp`

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

## 3. External SAT solvers behind `import sat.` (satext)

`solve`/`solve_all` can now be served by an external SAT solver
(kissat, CaDiCaL, MiniSat, or any DIMACS/IPASIR solver — the protocol
is detected automatically). Models come back exactly as with the
built-in solver, so `solve_all`, `findall` and blocking of enumerated
solutions work unchanged. How to use it:

- program option: `solve([$solver(Spec)], Vars)`, or call
  `bp.c_satext_set_solver(Spec)` (e.g. in `main`, before solving)
- environment: `SATEXT_SOLVER="kissat -t 4"` — whitespace-separated
  argv (first token = executable, resolved via PATH, the rest are
  arbitrary solver arguments); a `@file` token makes Picat generate a
  DIMACS file and pass its path; `nil`/`false` selects the built-in
  solver; `SATEXT_SOLVER="kissat|cryptominisat"` runs a first-wins
  portfolio over the listed solvers
- `SATEXT_NO_FALLBACK=1` stops the chain to the built-in solver when
  the external solver answers `unknown`
- low level: `import satext.` — `satext.solve(Spec, Clauses, Status,
  Model)`, `satext.cnf_info`, `satext.write_dimacs`

Usage notes: the comments of `lib/sat.pi` / `lib2/satext.pi` and
`exs/satext/bench_satext.sh`; examples: `exs/satext/`.

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
