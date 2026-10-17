# fp -- function values, lambdas, and composition for Picat

The `fp` module ([lib2/fp.pi](../../lib2/fp.pi)) brings functional
programming constructs to Picat: first-class function values,
anonymous functions (lambdas) with closures and recursion, function
composition, and pipelines -- with **no compiler changes**, built
entirely on Picat's own function-terms.

This directory contains [elegant.pi](elegant.pi), which computes
sqrt(2) by Newton's method, differentiates x^2, integrates x^2, and
maps a composition pipeline over a list -- all with lambdas.

## Introduction and requirements

**Function-terms** are the foundation.  In Picat, `$f` is the atom
naming the function `f/1`, and `'*'(2)` as a term is the function
`'*'/2` with its first argument pre-applied.  A term like
`$'*'(2)` can be *applied*: writing `F(X)` where `F` is bound to
`$'*'(2)` calls `'*'(2, X)`.  Picat applies such a value by
**appending** the remaining arguments to the term.  The whole `fp`
module is built on this one rule.

**The `fp` module is not preloaded.**  Run with `PICATPATH` including
`lib2`:

    cd Picat-src
    PICATPATH=.:./lib2 emu/picat exs/fp/elegant.pi

or from anywhere with the binary path made absolute.  Every example
file starts with `import fp.`

## How to build a lambda

A lambda is written `$lambda([X1,...,Xk], Body)` and is an anonymous
**function value**:

    import fp.
    F = $lambda([X], X + 1),
    println(F(4))                        % 5

Three things to know:

1. **The `$` is required.**  Picat evaluates function-call arguments
   eagerly, so the unquoted `lambda([X], X + 1)` would try to evaluate
   the body and fail with `Free variable in expression`.  The `$`
   keeps the body as a *term*, which the module evaluates at call
   time (the same trick the `udf` module uses for user functions in
   constraints).
2. **The parameter list is explicit**: `[X1,...,Xk]`, up to 6
   parameters.  Applying a lambda of k parameters to n arguments
   throws `$lambda_arity_mismatch(k, n)` unless k = n.
3. **A lambda value cannot be called immediately**: `$lambda(...)(4)`
   is a syntax error.  Bind first, then call (`F = $lambda(...), F(4)`).

The **body** is an expression over the parameters:

- variables, numbers, atoms, strings, lists;
- arithmetic `+ - * / // div mod rem **` and unary `-`;
- the comparisons `> < >= =< == !=`, returning `true`/`false` (in
  Picat itself comparisons are goals, not expressions -- here they
  evaluate to values);
- the built-in `cond(Boolean, TrueExp, FalseExp)`, short-circuiting:
  only the chosen branch is evaluated, so
  `cond(X == 0, 0, 100 / X)` is safe at `X = 0`;
- **calls of named functions** -- any loaded function, built-ins
  included (`abs(X)`, `sum(L)`, `length(L)`) -- and of other lambda
  values;
- other lambdas, returned as values (that is how closures are built).

A body needing a variable that is neither a parameter nor a captured
value throws `$lambda_free_variable(V)`.

## The functions used in the example

**`lambda(VList, Exp, A1,...,An)`** -- the callable form of a lambda.
Applying `F(A1,...,An)` to `F = $lambda(VList, Exp)` appends the
arguments to the term and calls `lambda(VList, Exp, A1,...,An)`,
which binds the VList and evaluates the body.  (The doc-only,
unimplemented compiler `lambda` of overview.tex is *implemented* here.)

**`cond(Boolean, TrueExp, FalseExp)`** -- Picat's built-in conditional
function; `fp` evaluates it short-circuiting at any nesting depth
(inside branches, arithmetic, lists, call arguments, comparisons).
It is also the expression-to-goal bridge: its Boolean argument is
called *as a goal*.

**`callv(F, X) = F(X)`** -- applies a function value to one argument,
from inside a lambda body.  Needed because the compiler evaluates the
arguments of a variable-functor call eagerly (`N * F(X)` cannot be
quoted), but the named call `callv(F, X)` can.  WARNING: do not use
`callv` for self-reference through the variable the lambda is bound
to -- that makes the term circular and crashes the interpreter.

**`selfapply(F, X[, Y[, Z[, W[, V]]]]) = F(F, ...)`** -- the recursion
helper.  A lambda is anonymous and cannot call itself by name, so it
takes **itself as its first parameter** and `selfapply` passes it
back:

    FAC = $lambda([Me, N], cond(N =< 1, 1, N * selfapply(Me, N - 1))),
    FAC(FAC, 5)                          % 120

**`comp(F, G, X) = F(G(X))`** -- function composition,
right-to-left.  The value form `comp(F, G)` returns the composite
function itself, so `map(comp($succ, $sq), L)` maps the composition
in one pass, with no intermediate list.  Built-ins compose too
(`comp($'*'(2), $sq)`).

**`pipe([F1,...,Fn], X)`** -- left-to-right application:
`pipe([F1,F2,F3], X) = F3(F2(F1(X)))`.  The value form
`pipe([F1,...,Fn])` returns the pipeline as a function value, bound
once and mapped:

    P = pipe([SQ, $lambda([X], X + 1), $'*'(3)]),
    map(P, [1,2,3])                      % [6,15,30]

**`compose([F1,...,Fn], X)`** -- n-ary composition,
right-to-left (`compose([], X) = X`); the value form `compose(Fs)`
returns the composite.

**`collect(F, From, To, P) = [F(I) : I in From..To, P(I)]`** -- the
comprehension surrogate.  Comprehension syntax cannot appear in a
lambda body (it is syntax, not a term), so a body maps and filters a
range through this helper instead; the filter `P` is a function value
returning true/false, typically a capturing lambda:

    S = $lambda([X], sum(collect($sq, 1, X, $lambda([I], I*I < X))))

**`truth(Goal) = cond(Goal, true, false)`** -- the
expression-to-goal bridge: the built-in `cond` calls its Boolean
argument as a goal, so `truth` turns any goal into a true/false
expression, usable in a lambda body.

**`filterf(P, L) = [X : X in L, P(X)]`** -- filter for function
values.  The built-ins that take a predicate call it as a *goal*,
which a function-returning lambda cannot serve; `filterf` applies
`P` as a function and keeps the elements returning true.

**`map(F, L)`**, **`sum(L)`**, **`abs(X)`** -- Picat's own built-ins,
called from lambda bodies by name; `map` accepts any function value
(lambda included) as its first argument.

## Closures

A lambda **returned from a lambda captures the outer parameter
values**.  The example's derivative operator is exactly this:

    D = $lambda([F], $lambda([X], (callv(F, X + 1.0e-6) - callv(F, X)) / 1.0e-6))

`D(SQ)` returns a lambda whose body calls the captured `SQ`; nested
capture works too (`make_adder` style):

    MK = $lambda([X], lambda([Y], Y + X)),
    F = MK(3),
    F(4)                                 % 7

## Running the example and the test suite

    cd Picat-src
    PICATPATH=.:./lib2 emu/picat exs/fp/elegant.pi

Expected output:

    sqrt(2)      = 1.41421356237469
    d/dx x^2 @3  = 6.000001000927568
    int x^2 0..2 = 2.664666999999823
    3*(x^2+1)    = [6,15,30]

The module's self-checking test suite (41+ checks) is
[test/fp_check.sh](../../test/fp_check.sh):

    sh test/fp_check.sh

## Performance

The lambdas are an interpreter over terms, so they are **much slower
than compiled Picat functions** — measure before using them in hot
loops (benchmarks, `statistics(runtime, ...)`):

| benchmark | compiled | lambda | ratio |
|---|---|---|---|
| 20k calls of `sq(X) = X*X` | 1 ms | 11 ms | ~10x |
| `sumto(300)` x20 (300-step recursion) | <1 ms | 13 ms | per-step ~2 us |
| `map(F, 1..2000)` x10 | 28 ms | 13 ms | ~2x FASTER |

The map result shows why: a compiled function value passed to `map`
(`$sq`) pays a runtime module search per element, while the lambda's
`lambda/3` is found once and the interpreter walk is cheap for a
small body.  For raw calls the ratio is 10-40x (every named call
inside a body goes through `call_by_arity`, i.e. another runtime
search).

**Known heap-pressure hang (upstream interpreter, not fp):** a long
run of function-term calls (tens of thousands) followed by a loop of
compiled recursions and then a recursive lambda call can hang the
interpreter (the heap sits at ~80 of 128 MB, gc has run once).  Keep
long-running lambda work in separate runs, or call the compiled
function instead in hot loops.

## Known limitations

- Bodies are expressions only: no multi-goal bodies, no `if/then/else`
  statements, no `&&`/`||`, no backtracking lambdas.
- No pattern-matching parameters (the VList is plain variables).
- `catch` inside a body does not short-circuit (the arguments are
  evaluated eagerly by the general call path).
- The `$` at the definition site cannot be avoided: true unquoted
  lambda syntax would need compiler support (the compiler's own
  `lambda` hook is bytecode-only and unreachable).
