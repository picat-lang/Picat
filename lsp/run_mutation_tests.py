#!/usr/bin/env python3
"""Mutation tests for picat_syntax_check.py.

Generates a throwaway .pi variant per case, runs the checker on it,
and asserts the expected diagnostic (or cleanliness).  The variants
are written to a temp dir so the tree stays clean.  Exit 0 if every
case passes, 1 otherwise.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCR = os.path.join(HERE, "picat_syntax_check.py")
mut = tempfile.mkdtemp(prefix="picat_mut_")

# (name, source, [expected substrings] or None for "must be clean")
CASES = [
    ("extra_close", "module m.\nmain => f(X)) .\n",
     ["unbalanced ')'"]),
    ("missing_close", "module m.\nmain => f(X\n",
     ["unclosed '('"]),
    ("wrong_kind", "module m.\nmain => f([1,2)).\n",
     ["unbalanced ')'", "still-open delimiter is '['"]),
    ("close_no_opener", "module m.\nmain => f()).\n",
     ["unbalanced ')': no matching opener"]),
    ("unclosed_brace", "module m.\nmain => f([1,2\n",
     ["unclosed '['"]),
    ("unclosed_curlies", "module m.\nmain => f({1,2\n",
     ["unclosed '{'"]),
    ("unterm_string", "module m.\nmain => writef(\"abc.\n",
     ["unterminated string literal"]),
    ("unterm_bcomment", "module m.\nmain => writeln(1). /* trailing comment\n",
     ["unterminated block comment"]),
    ("unterm_atom", "module m.\nmain => g('a, h(X). \n",
     ["unterminated quoted atom"]),
    ("head_arrow", "module m.\ng(X) => X = 1.\nbar(Y) -> Y = 2.\n",
     ["clause header uses '->'"]),
    ("arrow_multiline_ok", "module m.\ng(X) => X = 1.\nbar(Y)\n    -> Y = 2.\n",
     None),  # multi-line head: valid, must NOT be flagged
    ("dcg_ok", "module m.\np(a) ---> [a|S0] :- ...\n", None),  # not used below
    ("stray comma dot", "module m.\nf(X) => g(X, .\n",
     ["stray ',' or ';'"]),
    ("stray semi dot", "module m.\nf(X) => g(X; .\n",
     ["stray ',' or ';'"]),
    ("if_no_end", "module m.\nf(X) => if (X > 0) g(X).\n",
     ["'if' block (line 2) never closed with 'end'"]),
    ("foreach_no_end", "module m.\nf(X, L) => foreach(I in L) g(I).\n",
     ["'foreach' block (line 2) never closed with 'end'"]),
    ("end_alone", "module m.\nend.\n",
     ["'end' with no matching"]),
    ("else_no_if", "module m.\nf(X) => g(X) else h(X) end.\n",
     ["'else' with no matching 'if'"]),
    ("elseif_no_if", "module m.\nf(X) => g(X) elseif (X) h(X) else 0 end.\n",
     ["'elseif' with no matching 'if'"]),
    ("if_ok", "module m.\nf(X) => if (X > 0) g(X) elseif (X < 0) h(X) "
              "else writeln(0) end.\n", None),
    ("foreach_ok", "module m.\nf(L, A) => foreach(I in L) A := A + I end.\n",
     None),
    ("while_ok", "module m.\nf(X) => while (X > 0) X := X - 1 end.\n", None),
    ("dowhile_ok", "module m.\nf(X) => do X := X - 1 while (X > 0).\n",
     None),
    ("do_ok_multiline", "module m.\nf(N) =>\n    do\n        writeln(N),\n"
                        "        N := N - 1\n    while (N > 0).\n", None),
    ("writef_under", "module m.\nf(X) => writef(\"%d %d\", X).\n",
     ["format string takes 2 argument(s), but 1 argument(s)"]),
    ("writef_over", "module m.\nf(X, Y) => writef(\"%d\", X, Y).\n",
     ["format string takes 1 argument(s), but 2 argument(s)"]),
    ("writef_fmt_ok", "module m.\nf(X, Y) => writef(\"[%8.2f] %5d %n\", "
                      "X, Y).\n", None),
    ("printf_fd_skip", "module m.\nf(Fd, V) => printf(Fd, \"v %d\", V).\n",
     None),
    ("writef_fd_skip", "module m.\nf(Fd, V) => writef(Fd, \"v %d\", V).\n",
     None),
    ("writef_var_fmt_skip", "module m.\nf(F, X) => writef(F, X, Y).\n",
     None),
    ("to_fstring_ok", "module m.\nf(X) => S = to_fstring(\"v=%d\", X), "
                      "writeln(S).\n", None),
    ("to_fstring_under", "module m.\nf(X) => S = to_fstring(\"%d %d\", X), "
                         "writeln(S).\n",
     ["to_fstring: format string takes 2 argument(s), but 1 argument(s)"]),
    ("quoted_atoms_ok", "module m.\nf(IStream, L, HTab) => "
                        "g(IStream, ['(', '(', 'X' | L], HTab).\n", None),
    ("multiline_string_ok", "module m.\n/*\n\"\"\"\ndoc \" with %s and ( [ {\n"
                            "\"\"\"\n*/\nmain => writeln(1).\n", None),
    ("comment_quotes_ok", "module m.\n% a \" quote and ( paren in a comment\n"
                          "main => writeln(1).\n", None),
    ("string_with_comment_mark_ok", "module m.\nf(X, Y) => writef("
                                    "\"100%% %d %d\", X, Y).\n", None),
    ("pigeonhole_real", "module m.\nmain =>\n    N = 4,\n    foreach(K in "
                        "1..N)  % real-life bug: extra ')'\n        "
                        "writeln(K)),\n    end.\n",
     ["unbalanced ')'"]),
]
# fix the dcg case (proper DCG syntax)
CASES = [c for c in CASES if c[0] != "dcg_ok"]
CASES.append(("dcg_ok", "module m.\np --> [a|S0].\nq --> p, r.\n", None))

fails = 0
for name, src, expected in CASES:
    p = os.path.join(mut, name.replace(" ", "_") + ".pi")
    with open(p, "w") as fh:
        fh.write(src)
    r = subprocess.run([sys.executable, SCR, p],
                       capture_output=True, text=True)
    out = r.stdout
    ok = True
    where = ""
    if expected is None:
        if out.strip():
            ok = False
            where = out.strip()
    else:
        for e in expected:
            if e not in out:
                ok = False
                where = "missing: %r\n%s" % (e, out.strip())
    status = "PASS" if ok else "FAIL"
    if not ok:
        fails += 1
    print("%-4s %-24s %s" % (status, name, where.replace("\n", " | ")))
print("FAILURES:", fails)
sys.exit(1 if fails else 0)
