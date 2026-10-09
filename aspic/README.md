# Description

This is an Answer Set Programms to Picat transpiler (source to source translator).

The transpiler is described in [a paper accepted at ICLP 2026](https://cgi.cse.unsw.edu.au/~eptcs/paper.cgi?ICLP2026.21).

In addition a source to source compiler that takes Picat code with (now a single) ASP block and converts it into a picat source that uses the constraints defined in ASP in addition to the ones defined in Picat.  

# How to use the Picat with embedded ASP transpiler (PICASP)

picat picasp.pi examples/asp_embedded_in_picat.pi #using sat(default)

or

picat cp picasp.pi examples/asp_embedded_in_picat.pi #using cp

# Pure-Picat embedded-ASP flow (no external programs)

`picasp.pi` shells out to `cpp`, a python preprocessor, `sed` and a
second `picat` process.  The same flow is also available with no
external programs at all, in two ordinary `picat` runs:

    picat aspic_prep.pi pre SRC.pi LIBDIR OUT.pi aspic_runtime_template.pi sat
    (cd LIBDIR and) picat OUT.pi

 * stage 1 (`aspic_prep.pi`, with the in-process transpiler
   `aspic_gen.pi`): substitutes `#define NAME value` lines, extracts
   every `asp ... end` block — the marker is a bare `asp` as the
   final word of a line, scanned over the comment-blanked source, so
   comment text and `asp(...)` predicate calls (PICASP library style)
   are never interpreted — transpiles
   each block in-process, splices the generated predicates and a
   constraint-library import after the last `import`, replaces each
   block by a call to its `aspic_N(ASPIC_OPT_N)` predicate, writes
   `OUT.pi` and generates `LIBDIR/aspic_runtime.pi` from the template
   (in place of the `sed` step; `ASPIC_CONSTRAINTS_LIB` becomes `sat`
   or `cp`).  `#if`/`#ifdef`/`#include` are rejected with a clear error.
 * stage 2: an ordinary Picat program (imports `aspic_runtime.`), runs
   on any Picat build — including the browser build, where
   `aspic_prep`'s `main` (fixed web-FS paths) is what the page stages.

The blocks are numbered from 1 in BOTH flows (aspic_prep and picasp): the
Nth `asp ... end` block is replaced by `aspic_N(ASPIC_OPT_N)`, and the
block's objective variable is that same `ASPIC_OPT_N` — an
`aspic_solve_sub(ASPIC_OPT_N)` call in the surrounding code must reference
exactly that N. Passing a variable of a different name gives the solver a
variable that appears in no constraint, which stops with
`free_var_not_allowed`.

The constraint library is `sat` by default; a `#constr cp` line
(outside the `asp` block) switches to the picat CP solver — the
32-bit (browser/wasm32) build needs this for large instances, since
the built-in 32-bit kissat caps its clause arena at 1 GB while the CP
solver has no such cap (e.g. N=200 queens: `sat` runs out of arena,
`cp` solves it).

`aspic_gen` seeds a fresh runtime per run (`aspic_seed_bump()` at the
head of every generated predicate), so re-running in one interpreter
does not replay stale asserted rules.

Example (8 queens, identical result to the `picasp` flow):

    picat aspic_prep.pi pre examples/asp_embedded_in_picat-queens.pi . out.pi aspic_runtime_template.pi sat
    picat out.pi

# How to use the ASP to Picat transpiler (ASPIC)

## a) Using a convenience script

picat -log aspic.pi ASPFILE1.lp ASPFILE2.lp

## b) or with explicit calls:

picat -log aspic_transpiler.pi run ASPFILE.lp 2>tmpfile.pi

picat -log tmpfile.pi 2>&1

(the "run" keyword selects the transpile mode; "cp"/"sat"/"all" etc. are
alternatives, see below)

## c) or using an alias:

It is convenient to define an alias
```
alias aspic="picat -log aspic.pi "
```
and then use: aspic ASPFILE.lp 

The examples below assume this alias. Otherwise just replace manually
"aspic" with "picat -log aspic.pi" in the examples.

# Choice between cp and sat

Picat can use multiple libraries for solving constraint models. Aspic
supports now sat (default, being usually the fastest) 
and cp (much faster for selected problems like the jobshop scheduling).

To specify one of those just use it as the first argument to aspic, before
the list of lp files.

```
aspic sat ASPFILE.lp
aspic cp ASPFILE.lp
```

# Generating all solutions:

To generate all solutions, instead of a single one, add "all" when calling ASPIC:

```
# all solutions, using sat
aspic all ASPFILE.lp

#all solutions, with explicit choice of the constraints library
aspic sat/all ASPFILE.lp
aspic cp/all ASPFILE.lp

```


# Show
The annotation #show is not yet implemented, 
instead you can use the script show, like this

aspic ASPFILE.lp | grep solution | ./show predicate

where predicate is the one you are interested in.

Complete example:
```
$ picat -log aspic.pi examples/hk_coloring.lp 2>&1|grep solution|./show color/2
color(belgium,white)
color(denmark,white)
color(france,green)
color(germany,blue)
color(luxembourg,red)
color(netherlands,green)
```


# Rule order: place atom-creating rules before choice rules

Rules are translated in program order, and every rule line's variables are
enumerated over the constants of the atoms translated SO FAR (the
"universe"). A choice rule whose line comes first therefore iterates over an
EMPTY universe: no atoms of its head predicate are created, and every later
reference to them - in other rules, in an optimization directive, in the
printed solution - sees them as simply false. The choice rule silently
becomes vacuous, with no error and no warning.

Example - the choice rule below creates nothing, and the maximum is
reported as 0:

    {pick(X) : dom(X)}.
    dom(1). dom(2). dom(3).
    val(1,1). val(2,2). val(3,10).
    #maximize { V,X : pick(X), val(X,V) }.

Moving the facts before the choice rule fixes it - the same program then
reports the maximum 13 with pick(1), pick(2) and pick(3) chosen:

    dom(1). dom(2). dom(3).
    val(1,1). val(2,2). val(3,10).
    {pick(X) : dom(X)}.

Recommendation: always place facts (or any rules that introduce new
constants) BEFORE choice rules. The regression cases test/cases/ok_maximize.lp
and test/cases/run_maximize.lp follow this order; this behavior is a known
limitation of the translation, not of #maximize. Re-verified with the
completion alive (v0.3.11): the example still reports 0 - the choice atoms
are never created during translation, so the completion cannot fire for
them; a duplicate choice rule later in the program does create them
(tested: the same program with a second {pick(X) : dom(X)} line reports
the maximum 13).


# Not implemented yet

The aggregates #sum, #max, #min and #count ARE supported in rule bodies and
constraints over numeric terms (e.g. `#sum { X : p(X) }`), since v0.3.
Aggregates over ATOMS are supported too (v0.3.10): the element before the
colon may be a predicate literal, e.g. `q :- #sum { p(X) : dom(X) } >= 1.`
means q holds iff at least one p(X) is true — the element contributes the
atom's 0/1 truth value (so #sum over atoms is a count of the true atoms
among the groundings). Negated elements are supported too (v0.3.12):
`#sum { not p(X) : dom(X) }` counts the instances where p(X) is FALSE —
the element contributes 1 - atom via the runtime's aspic_not. Negation in
the aggregate BODY works as before.
Conditional literals in rule bodies are supported (v0.3.11): `all :- p(X) :
dom(X).` means all holds iff every p(X) with dom(X) holds — the conclusion
must hold for every grounding whose condition holds (each instance is an
implication condition -> conclusion; the conjunction of all instances must
hold). The conclusion's variables are local to the conditional literal and
range over the universe; the condition is comma-separated. Separate the
conditional literal from other body elements with `;` (a following
comma-group becomes part of the condition). The negated form works too
(`s :- not p(X) : dom(X).` — some instance is violated). NOT supported:
conditional elements in head disjunctions (`p(X) : d(X).` as a head) and
head aggregates with the extra literal between tuple and condition
(`#count { X: p(X,Y): X=1..3 } = 1`).
#minimize and #maximize ARE supported (v0.3.7); since v0.3.13 several
directives are allowed and they may be freely MIXED with weak constraints:
all of them unify into ONE signed objective, minimized with
aspic_solve(...,min) — #minimize elements and weak constraints contribute
+w, #maximize elements contribute -w (clingo: a #minimize element IS a
weak constraint :~ body. [w@l,t], a #maximize element IS
:~ body. [-w@l,t] — the weight sign distinguishes them). A pure-#maximize
program keeps the positive maximized objective in ASPIC_OPT (backward
compatible with the single-directive behavior); a mixed program has the
signed combined objective. In an embedded asp block the unified objective
arrives in the block's ASPIC_OPT_N variable.
Weak constraints (`:~ Body. [Weight@Level,Terms].`) ARE supported as a soft
weighted optimum (v0.3.9): every weak constraint contributes its penalty
terms to ONE combined objective, minimized with aspic_solve(...,min); the
level after `@` is parsed (and must be an integer) but is IGNORED — all
levels merge into the single weighted sum, so multi-level lexicographic
optimization is not implemented. The bracket is optional (`:~ Body.` means
weight 1, level 1); the weight may be a constant or a variable grounded by
the body; several weak constraints per program are allowed and their
penalties add up. Mixing weak constraints with #minimize/#maximize in one
program (or one embedded block) is rejected with a diagnostic. In an
embedded asp block the combined penalty arrives in the block's ASPIC_OPT_N
variable.

Everything else in the following list is NOT supported. None of it is
accepted silently: unknown or malformed input is reported as an error
(notparsed, "ASP syntax error!"), so a program either transpiles or stops
with a diagnostic that names the offending line.

- priority levels/weights (`@l`) in #minimize/#maximize; multi-level
  lexicographic weak-constraint optimization (levels are parsed but merged)
- conditional elements in head disjunctions (`p(X) : d(X).` as a head) and
  head aggregates with the extra literal (`#count { X: p(X,Y): X=1..3 } = 1`)
- a #minimize/#maximize whose { ... } set spans several lines (must be on
  one line)
- disjunctive heads (`a ; b :- c.`)
- strong negation (`-p(X)`)
- conditional literals (`H : B`) outside aggregates/cardinality constraints
- reals/floats; strings; quoted atoms
- mod, **, abs and other arithmetic functions; the <> comparison operator
- block comments `/* ... */` (only `%` line comments)
- #if/#ifdef/#include/#program/#external/#heuristic/#project/#script/#base

#show is also not implemented, but it is TOLERATED: the line is ignored and
a `% aspic: WARNING: ...` comment is emitted, and the script "show" (see
above) can be used to filter the printed solutions instead.

# Diagnostics

- a `#` directive other than #const, #minimize, #maximize, #show, #hide
  stops the transpilation with "unsupported ASP directive: <line>"
- a #const value that is not an integer, or an unparseable or multi-line
  #minimize/#maximize each stop with a named diagnostic
- a weak constraint that does not parse (missing bracket, non-integer
  level) stops with "invalid weak constraint (expected :~ Body.
  [Weight@Level,Terms].): <line>"
- constructs that the parser does not know are reported through the
  standard notparsed mechanism; the transpiler never crashes on them
- default negation is recognized as `not ` (space or tab) and as `not(`
  (parenthesized body); `~` is accepted directly

# Tests

test/test_aspic_syntax.sh runs the regression battery: the syntax cases in
test/cases/ (ok_* must parse; err_* must fail with a clean diagnostic and
never crash; run_* are transpiled and executed end-to-end), a parse check
over all examples/*.lp, and an end-to-end run of the embedded-ASP flow:

    bash test/test_aspic_syntax.sh

Exit status 0 iff all tests pass. The battery's run steps invoke picat with
-s 1234567890 - that is the STACK SIZE (not a random seed): a large stack
is needed so that the transpiled and executed programs do not exhaust it
(a tiny stack, e.g. -s 42, makes picat segfault - deterministic, not
random).

# Requirements

- you do have Picat installed (tested with 3.9)

- for the optional script "show", one needs to have python3 installed 

- sed needs to be in the path

- cpp (the C preprocessor) needs to be in the path

- so far only tested on Linux 

# Version history

v0.3.13:

- Weak constraints and #minimize/#maximize directives unify into ONE signed
  objective: #minimize elements and weak constraints contribute +w,
  #maximize elements contribute -w (clingo: a #minimize element IS a weak
  constraint :~ body. [w@l,t], a #maximize element IS :~ body. [-w@l,t] -
  the weight sign distinguishes them). The two restrictions are lifted:
  several #minimize/#maximize directives per program are allowed, and they
  may be freely mixed with weak constraints - all their penalties sum into
  one ASPIC_OPT objective, minimized (aspic_solve(...,min)). Backward
  compatible: an all-positive objective (only #minimize and weak) keeps
  Dir=min with the plain joined objective; an all-negative objective (only
  #maximize directives) keeps the positive maximized objective in ASPIC_OPT
  (aspic_solve(...,max), aspic_solve_dir(max) for embedded blocks) - the
  existing fixtures are unchanged. A mixed-sign objective is the signed
  join with a positive term first (the picat tokenizer rejects the
  generated sequence #=- with or without a following space, so the minus
  never follows the = directly). Implemented in both parsers
  (aspic_transpiler.pi and the aspic_gen.pi copy): one Terms accumulator of
  (Sign,SumString) pairs replaces the separate Mini string and Weak list;
  the duplicate and mixing diagnostics are removed. New tests:
  run_mixed_minmax.lp (#maximize + #minimize, the unified objective
  optimization(-13): q costs 1 so it is left out, the picks are maximized),
  run_weak_max_mix.lp (:~ + #maximize, optimization(-13)), 
  ok_multiple_minimize.lp (two #minimize directives, parse); the former
  err_weak_mixed.lp, err_weak_mixed2.lp, err_duplicate_minimize.lp,
  err_dup_min_max.lp became valid programs. 85 checks pass.

v0.3.12:
- Negated aggregate elements are supported: `#sum { not p(X) : dom(X) }`
  counts the instances where p(X) is FALSE - the element contributes
  1 - atom via the runtime's aspic_not, so the aggregate sums 1 for every
  grounding whose condition holds and whose atom is false. Previously such
  aggregates stopped with notparsed. Implemented with a new nva grammar
  clause (~atom, then the atom) producing an \$aspic_not-wrapped element,
  in both parsers (aspic_transpiler.pi and the aspic_gen.pi copy); the
  translation needed no change (the element lands in the generated
  ASPIC_AGVAL1= expression, which evaluates to 1 - atom). Semantics
  verified both ways: with :- not q. forcing q, the model contains q and
  not all p chosen (at least one p is false); with p(1),p(2) facts, q does
  not hold. New tests: run_aggregate_neg.lp, run_aggregate_neg_all.lp; the
  former err_aggregate_neg.lp became the run cases. 86 checks pass.

v0.3.11:

- Conditional literals in rule bodies are supported: `all :- p(X) : dom(X).`
  means all holds iff every p(X) with dom(X) holds (each instance is an
  implication condition -> conclusion; the conjunction of all instances must
  hold - clingo's universal reading of body conditional literals). The
  conclusion's variables are local to the conditional literal and range over
  the universe inside the generated comprehension; the negated form works
  (s :- not p(X) : dom(X). - some instance is violated). Implemented with a
  p(logic) grammar clause producing a \$condlit AST term and a
  replacesetmini clause translating it to a conjunction comprehension with
  cond(condition,conclusion,1) per instance (1 when the condition is false -
  vacuously true), combined with aspic_conj (#/\); getvars/getdetvars stop
  at \$condlit so the local variables do not become rule instances. Both
  parsers (aspic_transpiler.pi and the aspic_gen.pi copy).
- FIXED a significant pre-existing bug found while verifying: the Clark
  completion in the LP flow read the raw map names
  (get_heap_map(aspic_atoms)) instead of the seed-scoped
  (get_heap_map(aspic_m(aspic_atoms))) ones the atoms actually live in, so
  the ENTIRE completion was dead code - it never fired for any construct.
  With the fix the completion fires and gives clingo-like supportedness:
  atoms that appear only in bodies and are defined by no rule are now false
  (previously they were free fd variables defaulting to true with rand).
  All 80 existing checks still pass with the completion alive.
- New tests: ok_condlit.lp (parse), run_condlit.lp (the universal forces all
  three p via :- not all.), run_condlit_neg.lp (the negated form: s holds,
  the instance X=2 is violated), run_condlit_unsat.lp (UNSAT when p(2) is
  forbidden). The completion was then verified for the EMBEDDED flow too
  (run_emb_comp.pi: s = 0, p(2) = 0 forced false by the completion, q = 1 -
  the rule direction works), and the first-position choice rule behavior
  re-checked with the completion alive: the README example still reports
  maximization(0) - unchanged (the choice atoms are never created during
  translation, so the completion cannot fire for them); a duplicate choice
  rule later in the program does create them (the same program with a
  second {pick(X) : dom(X)} line reports the maximum 13). 85 checks pass.

v0.3.10:

- Aggregates over atoms are supported: the element before the colon in
  #sum/#count/#max/#min may now be a predicate literal, e.g.
  `q :- #sum { p(X) : dom(X) } >= 1.` - the element contributes the atom's
  0/1 truth value (a sum over atoms is a count of the true atoms among the
  groundings). Previously such aggregates stopped with notparsed (the
  element grammar only accepted numbers, variables and parenthesized
  arithmetic). Implemented with a new element parser nva in both parsers
  (aspic_transpiler.pi and the aspic_gen.pi copy) that tries the atom
  interpretation FIRST - the numeric parser has a fallback that would
  otherwise swallow a lowercase identifier as a #const reference before
  the atom branch is reached - and used only for the aggregate valset, so
  the cardinality bounds and the #minimize elements are unchanged. The
  translation needed no change: the element lands in the generated
  ASPIC_AGVAL1= expression, which evaluates to the atom's fd value via the
  transpiler-generated atom function (or aspic_var for a ground atom).
  Semantics verified both ways: with :- not q. the model contains q and at
  least one p; with :- q. it contains neither. New tests: ok_aggregate_atoms.lp,
  run_aggregate_atoms.lp, err_aggregate_neg.lp (negated elements stay an
  error); the former err_aggregate_atoms.lp became the ok/run cases. 79
  checks pass.

v0.3.9:

- Weak constraints (`:~ Body. [Weight@Level,Terms].`) are supported as a
  soft weighted optimum: every weak constraint contributes its penalty
  terms (weight x grounding) to ONE combined objective ASPIC_OPT, minimized
  with aspic_solve(...,min) - the same encoding as #minimize, but with any
  number of directives allowed and their penalties summed. The level after
  `@` must be an integer but is ignored (all levels merge into the single
  weighted sum; lexicographic per-level optimization is not implemented).
  The bracket is optional (weight 1, level 1 by default); the weight may be
  a constant or a variable grounded by the body; default negation in the
  weak body works (the same not-hack as in rule bodies). Mixing weak
  constraints with #minimize/#maximize in one program or block is rejected
  with a diagnostic. Implemented in both parsers (aspic_transpiler.pi and
  the aspic_gen.pi copy); the runtime is unchanged (the existing min path
  is reused). New tests: ok_weak.lp, ok_weak_multi.lp, ok_weak_not.lp,
  err_weak_mixed.lp, err_weak_mixed2.lp, err_weak_invalid.lp, run_weak.lp
  (soft weighted optimum 2 on the forced-pick weighted example),
  run_emb_weak.pi (embedded block, optimum 1). The former error case
  err_weak_constraint.lp became the ok_weak.lp case. 77 checks pass.

v0.3.8:

- aspic_prep numbers the asp blocks from 1, matching the picasp flow
  (previously 0-based): the Nth block is `aspic_N(ASPIC_OPT_N)` in both
  flows. The shipped example asp_embedded_in_picat.pi, written for
  picasp's ASPIC_OPT_1, now also runs through aspic_prep (it previously
  failed there with free_var_not_allowed, because the spliced block bound
  ASPIC_OPT_0 while the example referenced ASPIC_OPT_1 - a variable in no
  constraint). The block-numbering contract is documented in the
  Pure-Picat embedded-ASP flow section.

v0.3.7:

- #maximize is supported (it was previously reported as an error, and
  before that silently ignored): `#maximize { Weight,Terms : Body }.` on
  one line, one optimization directive per program. The generated LP-flow
  program solves with $max and reports $maximization(<value>); in an
  embedded asp block the generated predicate emits aspic_solve_dir(max) so
  aspic_solve_sub maximizes the block's ASPIC_OPT_N objective.
- #minimize regression: unchanged semantics ($min, $optimization(<value>)).

v0.3.6:

- Unknown # directives (#maximize, #include, #heuristic, ...) are now
  reported as errors instead of being silently ignored (previously
  #maximize was silently dropped and the program solved WITHOUT the
  objective). #show/#hide are tolerated, ignored with a warning comment;
  the show script remains the workaround.
- Only one #minimize was allowed in this version; duplicates were reported.
  Several directives (and mixing with weak constraints) are allowed since
  v0.3.13. #const values must be integers; violations are reported with a
  clear error.
- Default negation "not(" (parenthesized body) and "not" followed by a tab
  are now recognized; previously "not(q)" was silently read as the positive
  atom not(q) with wrong semantics. Parenthesized body expressions
  (`~(X)`, `(p;q),r`) are supported.
- Constructs that used to crash the transpiler (bare "-" terms, e.g. the
  strong negation -p(X); non-integer #const values; multi-line #minimize)
  now give clean syntax errors instead of aborting mid-transpile.
- Tab and CR characters in whitespace-only lines and CRLF line endings no
  longer break the parse.
- Regression tests: test/test_aspic_syntax.sh.

v0.3.5:

- Support for adding ASP facts with embedded picat, e.g. for adding info transformed from input files.

v0.3.4:

- Acceleration of the grounding at the expense of the readibility of the
  generated Picat code.

v0.3.3:

- Friendlier value display for the fd variables 

- Picat embedded code can be used in facts as well

- New examples: towers of Hanoi, global constraints using embedded Picat
  code.


v0.3.2:

- Fixes: proper handling of table-like facts e.g. p(1,2;3,8) ; added missing
  aggregators; turned off the debug info

v0.3.1:

- Faster parsing of ASP code

v0.3:

- Aggregators (#min #max #sum #count) are permitted in constraints

v0.2:

- Initial optimization support addded (#minimize)

- One can choose between sat (default) and cp

- It is possible now to call aspic with multiple ASP source files
