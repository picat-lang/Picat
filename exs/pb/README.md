# pb -- pseudo-Boolean constraints over 0/1 variables

`import pb.` lets a solver see pseudo-Boolean constraints in their
NATIVE form: the OPB text is emitted directly from the store, so an
external pseudo-Boolean solver (roundingsat, env `PBSOL`) sees the
linear structure -- the pigeon-hole class, where clausal encodings are
exponential for CDCL solvers, falls in milliseconds.

## Introduction and requirements

**roundingsat** is the external solver the pb module talks to.  It is
an open-source pseudo-Boolean solver built on **cutting planes**
(Gomory/MIR cuts and cardinality resolution), developed by the MIAO
research group; the official repository is
`gitlab.com/MIAOresearch/software/roundingsat` (build with CMake, a
C++17 compiler).  It reads the OPB format natively -- which is exactly
the form the pb module emits -- and its cutting-plane engine is what
makes the counting-argument UNSAT class (the pigeon-hole principle)
fall in milliseconds where clausal encodings are exponential for CDCL
solvers.  Measured on the instances in this directory: PHP(10) UNSAT
in ~17ms on the OPB form vs ~1580ms for kissat on the clausal encoding,
and at N=12 the CNF path takes >300s where the OPB form stays ~10ms.

Requirements per example:

| example | needs |
|---|---|
| the solver-agnostic ones (`subset_sum`, `pigeons`, `cnf`, `knapsack`, `strict`, `mv`) | nothing beyond Picat (built-in cp/sat) |
| the pb path of any of them | `PBSOL=<path>/roundingsat PICATPATH=<picat>/lib2` |
| `ramsey.pi`, the benchmarks (`php_bench.pi`, `parity.pi`) | roundingsat via `PBSOL` AND `kissat` on `PATH` for the CNF comparison side |
| `pb_test.pi` | roundingsat via `PBSOL` |
| the five families (`php_card`, `pmodd`, `mchess`, `evencol`, `subsetcard`) | nothing beyond Picat (built-in cp/sat); the pb side needs `PBSOL`; deterministic generators (seeded LCG), sized by the command line |

`PBSOL` defaults to `"roundingsat"` PATH-resolved; the satext runner
resolves it like an executable name.

## Where pb has an advantage

The speed advantage is specific to the counting/cardinality class --
where the pseudo-Boolean form shows a solver the counting structure
natively and a clausal encoding of the same program is exponential for
CDCL solvers:

| example | pb advantage? |
|---|---|
| `php_matrix.pi` / `php_bench.pi` | YES, measured: the counting/cardinality class -- PHP(10) UNSAT in ~17ms on the OPB form vs ~1580ms for kissat on the clausal encoding; at N=12 the CNF path takes >300s where the OPB form stays ~10ms |
| `parity.pi` | no solve-time gap (both instant -- the standard CNF encodings bake the counting in), but a STRUCTURAL advantage: one OPB line vs 24 MB of CNF at k=100 (the quadratic encoding blow-up) |
| `ramsey.pi` | no -- pb LOSES on the SAT-search side (K=4, N=15: pb ~1.7s vs ~73ms for kissat on the CNF; K=4, N=17: ~11.5s vs ~204ms) |
| `php.pi` | no -- the strict-chain (sorted) formulation makes it easy for everyone; cp's eager propagation answers instantly |
| the small demos (`subset_sum`, `pigeons`, `cnf`, `knapsack`, `strict`, `mv`) | correctness demos, never benchmarked -- `pigeons.pi` is the same counting class as `php_matrix.pi` in principle |

So: the solve-time advantage is specific to the cardinality/counting
class; the only other pb win is encoding compactness, not speed.

## The five pb-advantage families, tested against the built-ins

The families where roundingsat beats kissat by orders of magnitude on
the DIRECT clausal encoding (see the pb-advantage study) were built as
solver-agnostic examples and measured against the built-ins with
`test/pb_families_bench.sh` (import pb / import cp / import sat, the
import line being the only difference):

| file | family | UNSAT reason |
|---|---|---|
| `php_card.pi` | pigeon-hole, the cardinality form (one 0/1 var per pigeon-hole pair, `>= 1` per pigeon row, `<= 1` per hole column) | pigeons > holes |
| `pmodd.pi` | perfect matching on K_{2n+1} (one var per edge, one equation per vertex) | odd vertex count, every edge counted twice -- needs the divide rule |
| `mchess.pi` | mutilated chessboard, even n (one var per domino placement, one equation per remaining cell; the four dominoes touching the removed corners are forced off) | black and white cell counts differ |
| `evencol.pi` | even colouring on a random 4-regular graph with one edge subdivided (odd edge count, all degrees even; seeded LCG generator) | half of an odd number of edges |
| `subsetcard.pi` | subset cardinality: n x n pattern with exactly 4 ones per row/column plus one extra (seeded permutation and its cyclic shifts); rows `>= half rounded up`, columns `<= half rounded down` | rows ask for 2n+1, columns allow 2n |

**Times** (the in-process `solve` call in ms, printed by the programs
themselves via `bp.c_wall_ms`, from the 2026-10-15 sweep with
`test/pb_families_bench.sh` on the reference machine, 60 s timeout;
the pb column includes the OPB write and the roundingsat spawn,
roughly 8-10 ms of fixed overhead):

| sample | sat (ms) | cp (ms) | pb (ms) |
|---|---|---|---|
| `php_card.pi 8` | 10 | 8 | 10 |
| `php_card.pi 10` | 11 | 9 | 11 |
| `php_card.pi 12` | 9 | 11 | 12 |
| `pmodd.pi 7` | 10 | 10 | 31 |
| `pmodd.pi 8` | 9 | 10 | 38 |
| `pmodd.pi 9` | 10 | 11 | 81 |
| `mchess.pi 10` | 10 | 11 | 12 |
| `mchess.pi 14` | 12 | 11 | 15 |
| `mchess.pi 16` | 12 | 14 | 17 |
| `evencol.pi 20` | 10 | 11 | 13 |
| `evencol.pi 40` | 8 | 12 | 27 |
| `evencol.pi 60` | 10 | 11 | 16 |
| `subsetcard.pi 10` | 10 | 8 | 11 |
| `subsetcard.pi 20` | 9 | 9 | 13 |
| `subsetcard.pi 30` | 10 | 10 | 13 |

**Finding**: all five answer UNSAT in milliseconds under ALL THREE
solvers at every tested size (php_card up to N=30: ~50 ms under the
built-in sat and cp).  The built-in sat and cp paths do NOT use the
direct clausal encoding -- their lowering of the cardinality/linear
constraints carries the counting in auxiliary structure or in
propagation, exactly as the `parity.pi` finding predicts for
polynomial-size encodings.  The exponential separation documented in
the pb-advantage study is against the DIRECT clausal encoding
(`php_bench.pi`: kissat 1.8 s at N=10, >300 s at N=12, vs ~17 ms on
the OPB form).  The pb module's value on this class remains the
native proof form (one linear constraint, no auxiliary vars) and the
encoding-size gap on the weighted class.

## The solver-agnostic program

The same program body runs unchanged under `import pb`, `import cp`
and `import sat`; only the import line decides the solver:

    import pb.                   % or import cp / import sat

    main =>
        X = new_array(4),
        X :: 0..1,                        % the declaration
        X[1] + X[2] #>= 1,                % at least one of X1, X2
        X[1] + X[2] + X[3] + X[4] #= 2,   % exactly two 1s in total
        solve(X),                         % X now holds the 0/1 values
        printf("model %w%n", [X[I] : I in 1..4])

Running it prints `model [1,1,0,0]` on pb, `model [0,1,0,1]` on cp --
different models, both satisfying the constraints.

## The calls

### the # syntax (main-process only)

| call | meaning |
|---|---|
| `X :: 0..1` | the DECLARATION: a plain var is indexed on first sight (bound to a `pbv(I)` term); single vars, lists and arrays accepted |
| `X :: 1..3` | a domain LIST (the `..` materializes to one): encoded one-hot (one OPB var per value plus the exactly-one), the linear form stays native; non-contiguous value sets (`X :: [1, 5, 9]`) come for free; sets and `[]` rejected (`$pb_dom`) |
| `L #>= R` / `L #=< R` / `L #= R` | sum constraints over a linear term: `+ -`, `*` with a constant factor, `sum(ListOrArray)` over linear terms (the knapsack idiom) |
| `L #< R` / `L #> R` | strict relations over sums, variables on both sides (exact via integrality: `L < R` ⇔ `L - R <= -1`) |
| `L #\= R`, `#<=>`, `#=>` | rejected: no OPB encoding for reification (`$pb_no_reif`) |
| `X[i] * Y[j]`, `/`, undeclared vars | rejected (`$pb_nonlinear` / `$pb_var`) |
| an unresolvable external solver | `pb_no_solver` before any solve: a `PBSOL` path that does not exist, or a bare name not found on PATH, makes pb complain (`*** pb_no_solver: ...`) and exit picat with rc=1 -- a missing solver would otherwise fail the solve silently, which the `(solve -> SAT ; UNSAT)` pattern reads as UNSAT |
| `solve(V)` | the UNIFORM solve: a goal over an ARRAY of declared vars; each element is overwritten with its 0/1 value -- the same call and reading pattern as the built-in cp and sat solve; fails on unsat exactly like the built-ins |
| `pb.solve()` | solves the registered store, returns the 0/1 model list |
| `String = pb.to_opb()` | the current store's OPB text |

The store accumulates; `pb.reset()` starts a fresh problem.  A missing
store behaves as a fresh one, so after `import pb.` the first
declaration starts at index 1.  Every var index is checked, so a stale
var term fails loudly (`$pb_var`) instead of poisoning the formula.
Main-process only: a forked child inherits a clone, its posts never
propagate back.

### the functional API (explicit store terms, multiple stores)

| call | meaning |
|---|---|
| `B = pb.new(NVars)` | a store; boolean vars numbered 1..NVars |
| `B1 = pb.card(B, Rel, K, Idx)` | cardinality: all-1 coefficients over the indices |
| `B1 = pb.lin(B, Rel, K, Terms)` | linear: `Terms = [[C, I], ...]` |
| `Model = pb.solve_store(B)` | solves that store, throws `$pb_unsat` on unsat/unknown |
| `S = pb.solve_res(B)` | `S = [sat, Model] \| unsat \| unknown` |
| `NewVars = pb.add_vars(K)` | extends the current store; existing indices stay valid |

## The examples

| file | what it shows |
|---|---|
| `subset_sum.pi` | choose some of `[3, 34, 4, 12, 5, 2]` summing to 9 -- one linear equality, prints the picked numbers |
| `pigeons.pi` | 3 pigeons, 3 holes: each pigeon in exactly one hole, no hole holds two -- a permutation via cardinality constraints |
| `cnf.pi` | a CNF formula `(a \/ b) /\ (-a \/ c) /\ (-b \/ -c)` in its native 0/1 linear form: a negated literal `1 - x` contributes `-x` and shifts the bound |
| `knapsack.pi` | the standard knapsack-style idiom: `sum([Nums[I] * V[I] : I in 1..6]) #= 9` -- a sum over a comprehension of products |
| `strict.pi` | strict relations with variables on both sides (`A[1] #< A[2]`, `B[1]+B[2] #< B[3]+B[4]`) -- the integrality shift applied to the whole sum |
| `mv.pi` | multi-valued domains: `A :: 1..3` with `A[1] #< A[2]` and a weighted sum -- one-hot encoded in pb, native domains in cp/sat |
| `ramsey.pi` | (K,K)-Ramsey graphs sized by env (`K=4 N=15 ...`): 0/1 per edge, cardinality constraints per K-subset, degree-order symmetry breaking -- SAT/UNSAT branches with independent verification; under pb a fail is UNSAT |
| `parity.pi` | the modular subset-sum UNSAT benchmark: all weights even, the target odd -- UNSAT by pure rounding; pb answers in one MIR cut, but the standard CNF encodings make it propagation-easy too -- the separation is the quadratic encoding blow-up (24 MB of CNF vs one OPB line at k=100) |
| `php_bench.pi` | the pigeon-hole head-to-head: the pb cardinality encoding vs the direct clausal encoding solved with kissat -- the measured ~90x speed advantage (N=10: pb 17ms vs 1580ms), and at N=12 the CNF path takes >300s where pb stays ~10ms |
| `php.pi` | the simple pigeon-hole test, sized by the command line (`picat php.pi <holes> <pigeons>`), no timing -- the solver-agnostic body with the SAT/UNSAT branches, P hole variables with the domain 1..H and the all-different as the strict chain |
| `php_card.pi`, `pmodd.pi`, `mchess.pi`, `evencol.pi`, `subsetcard.pi` | the five pb-advantage families as solver-agnostic examples with deterministic generators (see the section above) |
| `php_matrix.pi` | the same test in the boolean-matrix formulation (one 0/1 var per pigeon-hole pair, the row/column cardinality constraints) -- the index-arithmetic formulation where the pb speed advantage is measured |
| `pb_test.pi` | the battery: 30 checks over both APIs (the pigeon-hole UNSAT headline, models verified against their constraints, store hygiene, the byte-exact OPB text, the rejection tags, the one-hot encoding) |

## Running

    # pb (an external pseudo-Boolean solver, env PBSOL)
    PBSOL=<path>/roundingsat PICATPATH=<picat>/lib2 emu/picat subset_sum.pi

    # cp or sat (built in): swap the import line, drop the env
    emu/picat subset_sum.pi

Temp OPB files go to the first of `/dev/shm`, `/tmp`, `./scratch` that
exists and is writable at runtime, and are removed after each solve.
