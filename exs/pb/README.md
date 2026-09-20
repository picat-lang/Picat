# pb -- pseudo-Boolean constraints over 0/1 variables

`import pb.` lets a solver see pseudo-Boolean constraints in their
NATIVE form: the OPB text is emitted directly from the store, so an
external pseudo-Boolean solver (roundingsat, env `PBSOL`) sees the
linear structure -- the pigeon-hole class, where clausal encodings are
exponential for CDCL solvers, falls in milliseconds.

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
| `X :: 0..1` | the DECLARATION: a plain var is indexed on first sight (bound to a `pbv(I)` term); single vars, lists and arrays accepted; the 0/1 domain itself is implicit, any other domain throws `$pb_dom` |
| `L #>= R` / `L #=< R` / `L #= R` | sum constraints over a linear term: `+ -`, `*` with a constant factor, `sum(ListOrArray)` over linear terms (the knapsack idiom) |
| `L #< R` / `L #> R` | strict relations over sums, variables on both sides (exact via integrality: `L < R` ⇔ `L - R <= -1`) |
| `L #\= R`, `#<=>`, `#=>` | rejected: no OPB encoding for reification (`$pb_no_reif`) |
| `X[i] * Y[j]`, `/`, undeclared vars | rejected (`$pb_nonlinear` / `$pb_var`) |
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
| `pb_test.pi` | the battery: 23 checks over both APIs (the pigeon-hole UNSAT headline, models verified against their constraints, store hygiene, the byte-exact OPB text, the rejection tags) |

## Running

    # pb (an external pseudo-Boolean solver, env PBSOL)
    PBSOL=<path>/roundingsat PICATPATH=<picat>/lib2 emu/picat subset_sum.pi

    # cp or sat (built in): swap the import line, drop the env
    emu/picat subset_sum.pi

Temp OPB files go to the first of `/dev/shm`, `/tmp`, `./scratch` that
exists and is writable at runtime, and are removed after each solve.
