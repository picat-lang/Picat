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
