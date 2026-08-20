#!/usr/bin/env python3
"""picat_syntax_check.py -- line-accurate diagnostics for Picat source.

The Picat parser reports many problems as a bare 'error' with no
position.  This script scans the source following the lexical rules
of the Picat 3.9 reference (% to-end-of-line and /* ... */ block
comments, possibly multi-line "... " strings, 'quoted' atoms with
backslash escapes, and ( ) [ ] { } as the four delimiters) and
reports, at the actual line and column:

  - unbalanced ( ) [ ] { }: a closer with no opener open (names the
    still-open delimiter), or an opener never closed
  - unterminated string literal / quoted atom / block comment
  - clause header that uses '->' where '=>' is meant (and the
    construct right before looks like the end of the previous rule)
  - stray ',' or ';' right before the clause-ending '.'
  - 'end' with no matching block; 'if' / 'foreach' / 'while' block
    that never gets its 'end'; 'else' / 'elseif' with no 'if'.  (A
    do-while block ends with 'while (Cond)' and needs no 'end'.)
  - writef / printf / to_fstring: number of argument-taking format
    specifiers (%c %d %e %E %f %g %G %i %o %s %u %w %x %X; '%%' and
    '%n' take none; flags / width / .precision are honoured) vs the
    number of arguments after the format -- normally a runtime
    format error, caught here early.  Only checked when the first
    argument is a string literal.  (End-of-file is accepted as an
    implicit clause terminator, as the compiler does.)

Usage:  picat_syntax_check.py FILE.pi [MORE.pi ...]
Exit:   0 if no errors, 1 if at least one error was reported.

Checker, not parser: it cannot catch every syntax error and may
occasionally flag unusual-but-valid code (an atom spelled like a
keyword in term position, or a 'do' body whose first statement is a
'while' loop, which is misread as the do-while terminator).
"""
import re
import sys

IDENT = re.compile(r"[A-Za-z_$][A-Za-z0-9_$]*")
HEAD_ARROW = re.compile(r"^\s*[A-Za-z_$][A-Za-z0-9_$]*\s*\([^()]*\)\s*->(?!>)")
STRAY_DOT = re.compile(r"[,;]\s*\.\s*$")
FORMAT_CALLS = {"writef": 16, "printf": 16, "to_fstring": 10}
NOARG_SPEC = frozenset("n")                 # %n takes no argument
OPENERS = "([{"
CLOSERS = ")]}"


class Diag:
    def __init__(self, path, line, col, msg):
        (self.path, self.line, self.col, self.msg) = \
            (path, line, col, msg)

    def sort_key(self):
        return (self.line, self.col)

    def render(self, lines):
        text = msg_safe_text(lines[self.line - 1]
                             if self.line <= len(lines) else "")
        pad = " " * max(0, self.col - 1)
        head = self.path + ":" + str(self.line) + ":" + str(self.col) \
            + ": error: " + self.msg
        return head + "\n      | " + text + "\n      | " + pad + "^"


def msg_safe_text(s):
    return s.rstrip("\r")


def lineof(s, p):
    return s.count("\n", 0, p) + 1


def colof(s, p):
    return p - (s.rfind("\n", 0, p) + 1)


def mask(raw):
    """Blank comments, string and quoted-atom contents; same length
    as raw (space = not code).  Reports unterminated constructs."""
    out = list(raw)
    diags = []
    i, n = 0, len(raw)
    while i < n:
        c = raw[i]
        if c == '"':
            j = i + 1
            while j < n:
                if raw[j] == "\\":
                    j += 2
                    continue
                if raw[j] == '"':
                    break
                j += 1
            if j >= n:
                diags.append(Diag(None, lineof(raw, i), colof(raw, i),
                                  "unterminated string literal"))
                i = n
                continue
            for k in range(i, j + 1):
                if out[k] != "\n":
                    out[k] = " "
            i = j + 1
            continue
        if c == "'":
            j = i + 1
            while j < n and raw[j] != "\n":
                if raw[j] == "\\":
                    j += 2
                    continue
                if raw[j] == "'":
                    break
                j += 1
            if j >= n or raw[j] != "'":
                diags.append(Diag(None, lineof(raw, i), colof(raw, i),
                                  "unterminated quoted atom"))
                i = n
                continue
            for k in range(i, j + 1):
                if out[k] != "\n":
                    out[k] = " "
            i = j + 1
            continue
        if c == "/" and i + 1 < n and raw[i + 1] == "*":
            j = raw.find("*/", i + 2)
            if j < 0:
                diags.append(Diag(None, lineof(raw, i), colof(raw, i),
                                  "unterminated block comment '/*'"))
                for k in range(i, n):
                    if out[k] != "\n":
                        out[k] = " "
                i = n
                continue
            for k in range(i, j + 2):
                if out[k] != "\n":
                    out[k] = " "
            i = j + 2
            continue
        if c == "%":
            j = raw.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
            continue
        i += 1
    return "".join(out), diags


def count_specifiers(fmt):
    """Argument-taking specifiers in a format string.
    Spec: %[flags][width][.precision]c d e E f g G i o s u w x X.
    '%%' and '%n' take no argument."""
    n, i, m = 0, 0, len(fmt)
    while i < m:
        if fmt[i] != "%":
            i += 1
            continue
        if i + 1 >= m:
            break
        if fmt[i + 1] == "%":
            i += 2
            continue
        k = i + 1
        while k < m and fmt[k] in " -+#0":
            k += 1
        while k < m and fmt[k].isdigit():
            k += 1
        if k < m and fmt[k] == ".":
            k += 1
            while k < m and fmt[k].isdigit():
                k += 1
        if k < m and fmt[k] not in NOARG_SPEC:
            n += 1
        i = k + 1
    return n


def arg_count(body_code):
    """Top-level argument count of a call body (masked body)."""
    if not body_code.strip():
        return 0
    depth, commas = 0, 0
    for c in body_code:
        if c in OPENERS:
            depth += 1
        elif c in CLOSERS:
            depth -= 1
        elif c == "," and depth <= 0:
            commas += 1
    return commas + 1


def matching_close(code, open_idx):
    depth = 0
    for j in range(open_idx, len(code)):
        c = code[j]
        if c in OPENERS:
            depth += 1
        elif c in CLOSERS:
            depth -= 1
            if depth == 0:
                return j
    return None


def check(path):
    with open(path, "rb") as fh:
        raw = fh.read().decode("utf-8", "replace")
    lines = raw.split("\n")
    code, diags = mask(raw)
    for d in diags:
        d.path = path
    clines = code.split("\n")
    diag = diags.append

    def err(line, col, msg):
        diag(Diag(path, line, col, msg))

    # per-line delimiter depth and clause-terminator marks ----------------
    line_end_depth = [0] * (len(clines) + 1)
    depth = 0
    for idx, cl in enumerate(clines, 1):
        for c in cl:
            if c in OPENERS:
                depth += 1
            elif c in CLOSERS:
                depth = max(0, depth - 1)
        line_end_depth[idx] = depth
    line_end_dot = [0] * (len(clines) + 1)
    for idx in range(1, len(clines) + 1):
        rl = clines[idx - 1]
        line_end_dot[idx] = (rl.rstrip().endswith(".") and
                             line_end_depth[idx] == 0)

    # main scan: delimiters, statement keywords, format-call sites ------
    stack = []        # (char, line, col)
    ops = []          # (kind, line, col)
    fscalls = []      # (name, open_idx)
    i, n = 0, len(code)
    line = 1
    while i < n:
        c = code[i]
        if c == "\n":
            line += 1
            i += 1
            continue
        if c in OPENERS:
            stack.append((c, line, colof(code, i)))
            i += 1
            continue
        if c in CLOSERS:
            want = {")": "(", "]": "[", "}": "{"}[c]
            if not stack:
                err(line, colof(code, i),
                    "unbalanced '%s': no matching opener is open at this "
                    "point" % c)
            else:
                kind, ol, oc = stack.pop()
                if kind != want:
                    err(line, colof(code, i),
                        "unbalanced '%s': the still-open delimiter is "
                        "'%s' (line %d, column %d)" % (c, kind, ol, oc))
            i += 1
            continue
        if c.isalpha() or c in "_$":
            m = IDENT.match(code, i)
            if m is None:
                i += 1
                continue
            name = m.group(0)
            col = colof(code, i)
            prev = i - 1
            while prev >= 0 and code[prev] in " \t\r":
                prev -= 1
            # start-of-line (or right after , ; > : ? on the same line)
            if prev < 0 or code[prev] in "\n,;>:?":
                boundary = True
            else:
                boundary = False
            if name in ("foreach", "while", "if", "do"):
                if name == "while" and ops and ops[-1][0] == "do":
                    ops.pop()          # do-while terminator (also mid-line:
                # a 'while ... end' cannot legally precede the 'do' body)
                elif boundary:
                    ops.append((name, line, col))
            elif name == "end":
                # 'end' is reserved, so it never occurs elsewhere
                if ops:
                    ops.pop()
                else:
                    err(line, col,
                        "'end' with no matching 'if' / 'foreach' /"
                        " 'while' / 'do' block")
            elif name in ("else", "elseif"):
                # reserved words too; they may sit mid-line after a goal
                if not ops or ops[-1][0] != "if":
                    err(line, col,
                        "'%s' with no matching 'if'" % name)
            elif name in FORMAT_CALLS:
                k = m.end()
                while k < n:
                    while k < n and code[k] in " \t\r\n":
                        k += 1
                    if code[k: k + 2] == "/*":
                        k2 = code.find("*/", k + 2)
                        k = n if k2 < 0 else k2 + 2
                        continue
                    if code[k] == "%":
                        k = code.find("\n", k)
                        k = n if k < 0 else k + 1
                        continue
                    break
                if k < n and code[k] == "(":
                    fscalls.append((name, k))
            i = m.end()
            continue
        i += 1

    # format-call argument checks ----------------------------------------
    for name, op in fscalls:
        close = matching_close(code, op)
        if close is None:
            continue                          # imbalance already reported
        body_code = code[op + 1: close]
        body_raw = raw[op + 1: close]
        if not body_code.strip():
            continue
        # First token of the argument list, read from the RAW body
        # (in the masked body the string contents are blanked out).
        j, L = 0, len(body_raw)
        first_is_str = False
        while j < L:
            c = body_raw[j]
            if c in " \t\r\n":
                j += 1
                continue
            if body_raw[j: j + 2] == "/*":
                k2 = body_raw.find("*/", j + 2)
                j = L if k2 < 0 else k2 + 2
                continue
            if c == "%":
                nl = body_raw.find("\n", j)
                j = L if nl < 0 else nl + 1
                continue
            if c == "'":
                j += 1
                while j < L and body_raw[j] != "\n":
                    if body_raw[j] == "\\":
                        j += 2
                        continue
                    if body_raw[j] == "'":
                        break
                    j += 1
                j += 1
                continue
            first_is_str = (c == '"')
            break
        fmt = None
        if first_is_str:
            q = j + 1
            while q < L:
                if body_raw[q] == "\\":
                    q += 2
                    continue
                if body_raw[q] == '"':
                    break
                q += 1
            if q < L:
                fmt = body_raw[j + 1: q]
        args = arg_count(body_code)
        if fmt is not None:
            need = count_specifiers(fmt)
            if args != 1 + need:
                err(lineof(raw, op), colof(raw, op) + 1,
                    "%s: format string takes %d argument(s), but %d "
                    "argument(s) follow the format string"
                    % (name, need, args - 1))

    # line-level checks ----------------------------------------------------
    for ln in range(1, len(lines) + 1):
        cl = clines[ln - 1]
        if not cl.strip():
            continue
        if HEAD_ARROW.match(cl):
            l = ln - 1
            while l >= 1 and not clines[l - 1].strip():
                l -= 1
            gate = (l == 0) or line_end_dot[l]
            if gate:
                err(ln, cl.find("->") + 1,
                    "clause header uses '->'; did you mean '=>' ?")
        tail = cl.rstrip()
        md = STRAY_DOT.search(tail)
        if md:
            err(ln, md.start() + 1,
                "stray ',' or ';' right before the clause-ending '.'")

    # (picat accepts end-of-file as an implicit clause terminator, so
    #  there is no "last line must end with '.'" check)

    for kind, ol, oc in stack:
        want = {"(": ")", "[": "]", "{": "}"}[kind]
        err(ol, oc,
            "unclosed '%s': never closed before the end of the file "
            "(matching '%s' missing)" % (kind, want))
    for kind, ol, oc in ops:
        err(ol, oc,
            "'%s' block (line %d) never closed with 'end'" % (kind, ol))

    diags.sort(key=lambda d: (d.line, d.col))
    seen, uniq = set(), []
    for d in diags:
        k = (d.line, d.col, d.msg)
        if k not in seen:
            seen.add(k)
            uniq.append(d)
    return uniq, lines


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: picat_syntax_check.py FILE.pi [MORE.pi ...]")
        sys.exit(2)
    any_err = False
    for p in sys.argv[1:]:
        d, lines = check(p)
        for x in d:
            print(x.render(lines))
        if d:
            any_err = True
    sys.exit(1 if any_err else 0)
