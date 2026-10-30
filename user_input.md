# User input log

Each entry is the user's message, rewritten to be standalone and context independent.

## Entry 1 (Thu Oct 22 2026)

Clone the Picat repository from https://github.com/picat-lang/Picat.git into the current
working directory (/home/jovyan/snapshotable/aspicudf), then create a new purely local
git branch named `aspicudf` based on the remote `experimental` branch (with upstream
tracking removed) and check it out.

## Entry 2 (Thu Oct 22 2026)

Read, to build a full picture of the two feature areas this project combines:
1. the main README.md (the experimental-branch overview, incl. sections 5 `udf` and the aspic-related context),
2. the aspic README (aspic/README.md — the ASP-to-Picat transpiler and the PICASP embedded-ASP flow),
3. the udf README (doc/udf.tex — user-defined functions in constraints),
4. the udf examples (exs/cp/udf_test.pi, udf_testsum.pi, udf_functions.pi, udf_ops_schedule.pi,
   exs/sat/udf_functions.pi, exs/mip/udf_functions.pi, exs/smt/udf_functions.pi, exs/fp/udf_lambda.pi,
   plus the operator shim files lib2/udf_ops*.pi and the check test/udf_check.sh),
5. the aspic implementation files (aspic/aspic.pi, aspic/picasp.pi, aspic/aspic_prep.pi,
   aspic/aspic_gen.pi, aspic/aspic_transpiler.pi, aspic/aspic_runtime_template.pi,
   aspic/picasp_preprocessor.py, aspic/show).

## Entry 3 (Thu Oct 22 2026)

Convert the backpack ASP example (aspic/examples/backpack.lp) with the aspic
transpiler (into the scratch/backpack working folder, since the flow writes t.pi
and aspic_runtime.pi into the CWD), read the transpiled program that the
conversion produced, and observe how awkward supplementary (auxiliary) variables
are introduced by the generated code to work around Picat's limitation that
regular user-defined Picat functions cannot be used inside constraint
expressions.

## Entry 4 (Thu Oct 22 2026)

Question: does udf.define still have issues with reusing variables between
multiple rules? (Answered empirically with scratch/udfexp/exp7.pi, 16 checks:
no - per-rule copy_term isolation makes same-name reuse safe; the historical
== gotcha concerns udf.pi's own walker clauses only, fixed at lib2/udf.pi:325.)

## Entry 5 (Thu Oct 22 2026)

Proceed with the rewrite of the aspic translator(s) so that translated programs
use UDFs and are easier to read (continuing Entry 3's task; the validated
design: the objective/aggregate as ONE udf term $suml([$when(C,V),...]) posted
through a shimmed '#=', minimal 2-aux-var plain chains for rule-body
aggregates, aspic_instances enumeration instead of universe(), runtime-wrapper
plumbing aspic_flatten/aspic_udf_init so generated code needs no
"import udf.").

## Entry 6 (Thu Oct 22 2026)

"What did we do so far?" - provide an anchored summary of the session state
(the objective, the completed work, the active work, and the next move).

## Entry 7 (Thu Oct 22 2026)

"stay in your folder; proceed" - continue the aspic_gen.pi mirror / battery
work from the aspic/ working directory (do not cd out to other project
folders for the debugging steps).

## Entry 8 (Thu Oct 22 2026)

Check all aspic examples (aspic/examples/) by comparing the answers produced
with the current (modified, udf-rewrite v0.4.0) transpiler against the ones
produced with the unmodified upstream Picat experimental transpiler
(the baseline v0.3.27 sources at HEAD), verifying the rewrite did not change
the semantics.

## Entry 9 (Thu Oct 22 2026)

What about the speed - is there any slowdown of the converted programs with
the udf-rewrite (v0.4.0) compared to the unmodified baseline (v0.3.27)?

## Entry 10 (Thu Oct 22 2026)

Commit the current project state (the v0.4.0 aspic rewrite, the battery and
test-script fixes, the comparison/speed scripts, and the project docs) as a
single commit on the aspicudf branch.

## Entry 12 (Thu Oct 22 2026)

"pay attention, aspic_runtime gets regenerated" - reminder that the
transpiler regenerates aspic_runtime.pi (via sed from aspic_runtime_template.pi
in its CWD) on every transpile run; the comparison scripts must copy the
version-matched template into the transpile working dir first.

## Entry 13 (Thu Oct 22 2026)

List the limitations of the udf feature (lib2/udf.pi) and how each affects
aspic (the translated programs / the transpiler design).

## Entry 14 (Thu Oct 22 2026)

Question: what are the limitations of udf and how do they affect aspic?
(Answered with the structured list below, from doc/udf.tex's Limitations
subsubsection, the lib2/udf.pi docstring, and this project's experiments
scratch/udfexp/exp1-exp13.)

## Entry 15 (Thu Oct 22 2026)

Claim to verify: you never need to use nested $ or more than one $ per
expression - $f(g(X)) should be the same as $f($g(X)) (the inner $ is
redundant, g is not evaluated automatically inside a $-term). Verify
empirically and update the limitation list accordingly.

## Entry 16 (Thu Oct 22 2026)

"also $f+$g is never needed, this is $(f+g)" - refinement of the single-
dollar claim: the multi-call form is ONE $-term wrapping the whole
expression with the unmarked calls inside ($(f+g), $(f(1)+g(1))), never
two $-terms joined by an operator.  Verified with exp15 (also: a 0-arity
udf function cannot be defined - $f throws struct_expected).

## Entry 17 (Thu Oct 22 2026)

"don't keep defining shims, they are all in cp2 and sat2 libs, just import
those instead of cp and sat" + "they are in lib2, pay attention to picat's
-path arg" - the generated programs import sat2/cp2 (the lib2 shim
libraries) instead of sat/cp; the per-program '#=' shim lines and the
runtime's aspic_flatten wrapper are removed; lib2 must be on the module
search path (PICATPATH or picat's -path argument).

## Entry 18 (Thu Oct 22 2026)

"show me how an aspic optimisation problem translates" then "commit" - the
backpack.lp translation (v0.4.1: import sat2, the $suml objective, the
minimal two-locals-per-instance constraint chain) was walked through and
verified (maximization(29), sel(1), sel(3)); the verification experiments in
scratch/ and the walkthrough files are committed.

## Entry 19 (Fri Oct 23 2026)

"why is suml necessary?" / "why not just sum?" / "not = but #=" / "after
solve, the value is in var" - experiments exp16a-f showed the #= operator
passes its operands unevaluated, so the builtin sum() folds the expanded
when() contributions and $suml is NOT necessary (the earlier 0 results were
a test bug: max(X) without the $ is evaluated instead of passed as the
$max(X) option). v0.4.2: the generated objective is
ASPIC_OPT#=sum([X : X in C1++C2]); the $suml registration and the expansion
depth raise are dropped from aspic_udf_init; battery 88/0, baseline
comparison 31 matched / 0 differ.

## Entry 20 (Fri Oct 23 2026)

"commit" - the working tree was already clean (v0.4.2 committed as 2aca7ce);
$suml appears in no live source or test script, only in stale scratch
copies and one historical handover note.

## Entry 21 (Fri Oct 23 2026)

"commit" - the headless wasm verification (x_asp_queens.pi works on the
rebuilt dist, rc=0, valid 8-queens board) was already done; the
stdout-passing variant of the headless harness (scratch/wasmshow/show.js)
is committed.

## Entry 22 (Fri Oct 23 2026)

"in hk_jobs the transformation of plain facts to function configurations
returning 1 misses the ones with options (semicolon). e.g. person, job.
examine how male and female work and these other ones miss" then "yes" -
examination: collecthead_choice marked ALL the cardinality set's arguments
as atom-generating, so the enumeration domain (person/job) lost the
fact-function classification and became fd variables. Fix (v0.4.3):
collecthead_choice marks only the set element (Es[1]); person/job now get
person(roberta)=1./person(_)=0. and the choice rules enumerate over
aspic_instances instead of universe(). Battery 88/0, baseline comparison
31 matched / 0 differ.
## Entry 23 (Wed Oct 28 2026)

"pull" - already up to date; only origin/main advanced (5d61e6d..be45127).

## Entry 24 (Wed Oct 28 2026)

"why is the scratch in the repo? move any tests that you need from it to tests and remove it from the repo, it should be in gitignore" - the parallel aspic/udf session had committed scratch/. Moved the reusable tests to test/ (udfexp/ 26 probes, wasmshow/show.js), git rm'd the one-off artifacts (backpack/, bisect/, oldemb/, queens_ladder/, the gen dumps), added scratch/ to .gitignore, documented the moved items in test/README.md (udfexp needs PICATPATH=.:./lib2). Committed 3d89ef9 + 74bf81f, pushed.

## Entry 25 (Wed Oct 28 2026)

"pull" - already up to date; only origin/main advanced (be45127..5bedd89).

## Entry 26 (Wed Oct 28 2026)

"pull the main branch in a temp dir and read about anonymous functions and predicates" - cloned main (4.0b13, 5bedd89) to scratch/mainpull; read doc/higher_order.tex (322 lines, a proposed chapter for the 4.0 guide), the grammar in syntax_grammar.tex, and updates.txt. Anonymous functions ([A1..An] -> Res, Body) and predicates ([A1..An] => Body) are a 4.0b12 feature; the chapter header claims parser-only (no compiler/runtime).

## Entry 27 (Wed Oct 28 2026)

"could we benefit from this new syntax instead of our lambdas?" - answered no: main's anonymous forms were believed parser-only, our branch lacks the parser, and udf.pi's constraint integration (solver-variable sharing, expansion across the four solvers) is out of scope for host closures; fp.pi's $lambda would migrate only after a 4.0 merge.

## Entry 28 (Wed Oct 28 2026)

"are you sure it doesn't run yet with the picat from main?" - challenged and REFUTED: built main's binary from scratch/mainpull/emu (zero errors, one pre-existing upstream warning in init.c) and verified all doc examples run: lambdas, anonymous predicates, capture-at-creation (15/25), currying (15), pattern params (a), foreach bodies (6), 0-arity (42), findall generators ([a,b,c]). The chapter header is STALE. Constraint test: main rejects a lambda after #= (syntax error) while udf's $apply($lambda(...)) works (X=10), so udf.pi stays; fp.pi is now replaceable in principle after a 4.0 merge (native lambdas verified working on main).
## Entry 29 (Wed Oct 28 2026)

"our experimental picat should already be based on 4.08b" - confirmed: the fork point is 81f4c83, two commits after the 4.0b8 bump (944c61f, Oct 15 2026); emu/build.h says VERSION "4.0.0". My earlier "3.9 lineage" claim was wrong (over-inferred from the missing updates.txt and the Picat 3.9#12 stock comparison binary). The lambda conclusion stands with the precise reason: anonymous defs landed in 4.0b12 (Oct 24), four betas after our fork. The branch-to-main gap is 4.0b9-4.0b13.
## Entry 30 (Wed Oct 28 2026)

"can you write in this syntax our lambda examples from exs (not the udf ones)" - wrote exs/fp/elegant_native.pi: exs/fp/elegant.pi rewritten in Picat 4.0's native anonymous-function syntax, no fp import, runs on a 4.0b12+ binary (not our 4.0b8-forked emu/picat). Found a real limitation by isolation: native closures CANNOT BE SELF-APPLIED - passing a closure as its own argument (F(F,...), the anonymous-recursion pattern) dies with "Undefined procedure: lam.../N" (the closure term is extended with itself, a circular term - the same class the fp README warns about for callv). The recursive lambdas were restructured as iterations (foreach + := + convergence guard). Output matches elegant.pi (sqrt(2)=1.41421356237469, d/dx=6.000001000927568, pipeline=[6,15,30]; the integral differs in the last digits: 2.664667000000001 vs 2.664666999999823, the summation order differs). exs/fp/README.md updated. Files uncommitted (commit on request).
## Entry 31 (Fri Oct 30 2026)

"try in a new local branch initiated from experimental to integrate all new changes from the main branch" - created local branch integrate-main from experimental (2c2bd2b) and merged origin/main (a137b2a, past 4.0b13). 11 conflicts resolved: 5 emu C-interface files (mode-only changes on our side) took main's deletion; lib2/common_constr.pi + pb.pi kept ours (main deleted lib2/, ~90 unchanged lib2 files staged for deletion - upstream conversion tools); README.md kept ours (the experimental-branch README); emu/clpfd.c kept our ~800-line dense-cache block; emu/file.c combined main's NULL-checks + next_file_index() pre-allocation with our PICATPATH fallback and atomic .qi writes (two upstream leak fixes added on merged code: qi buffer free on index failure, release_file_index on fopen failure); emu/init.c kept our experimental banner (base note updated to 4.0b13). Three pre-existing parvm.c warnings surfaced by the full rebuild and fixed: fprintf %d casts (pvm.mode/pvm.armed/pvm_last_deleg_status), write return captured in the diag dump, and a REAL BUG in pvm_segv_dump: for (k=k0; k<0; k--) loops forever (decrementing while negative) - fixed to k++ (probe a few words below si_addr as intended). Rebuilt zero-warning (fdn linked). All suites green: pigeon_hole 720, fp_check, udf_check, aspic battery 88, reif, ext_check 5/5, deviation_check 0 DIVERGES. Native lambdas now PARSE on the merged binary (lammain1=7, elegant_native.pi all 4 outputs); a lambda after #= still rejected (main's limitation).
## Entry 32 (Fri Oct 30 2026)

"check also hakan and full examples list for divergences - make scripts for this, or are they already there and did you check them?" - the scripts were already there: exs/deviation_check.sh (exs, ran pre-question: 0 DIVERGES) and test/divergence_parallel.sh (Hakan tree). Ran the Hakan sweep (3137 examples, 08:12-10:44): 27 DIVERGES + 2 RC-DIFFERS, all timeout-based (30s default) or upstream. Wrote test/divergence_reverify.sh (the documented 120s sequential re-verification; fixed two script bugs en route: the sweep's root/ prefix means the tree root, and NFS .nfs* stale-file locks - unique per-invocation workdir now). Re-verified all 17 exp-timeout candidates at 120s: 6 same (no regression), 7 exp-slower, 2 both-fail, 2 _bv. Classification: 7 exp-slower are UPSTREAM (FDN=0 also times out; main's 4.0b13 binary also times out - 4.0b13 slower than 3.9#12 on going_to_church, how_many_routes, instant_insanity, movement_puzzle, no_three_in_line, tantalizer_45, utils_me_test); the 2 _bv (coins_grid_bv, send_most_money_bv) are an UPSTREAM 4.0 regression (sat_solve_minimize_bv_sub/6 undefined - fails on all 4.0 binaries incl. main, passes on stock 3.9#12); 4 stock-fails cases are 4.0 improvements (stock fails, we pass); 6 stock-timeouts are improvements. ZERO regressions from our merge. Harness gotcha found: FDN_*/SATEXT_* env leaks distort runs (unset before sweeps). README updated with the reverify script.
