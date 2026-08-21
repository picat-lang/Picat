#!/usr/bin/env python3
"""
aspic_preprocessor.py

Preprocesses Picat source files to replace ASP blocks (asp ... end) with
calls to generated predicates (aspic_1, aspic_2, …).

Each ASP block's content is written to a temporary file and passed to the
external script ``aspic_sub <ID> <tmp_file>``, which should output valid
Picat code defining a predicate ``aspic_<ID>/0``.

The generated predicate definitions are inserted after the (possibly
multiple) ``import`` lines at the top of the source.  The original ``asp
… end`` blocks are replaced by ``aspic_<ID>()`` calls, leaving all
surrounding punctuation (commas, etc.) intact.

Usage
-----
    python aspic_preprocessor.py <input_file> [output_file]

If ``output_file`` is omitted the result is written to stdout.
"""

import re
import subprocess
import sys
import tempfile
import os

# ---------------------------------------------------------------------------
# Core preprocessing logic
# ---------------------------------------------------------------------------

# ``asp`` and ``end`` are matched as whole words (lower-case), non-greedy
# content in between (including newlines).
_ASP_BLOCK_RE = re.compile(r'\basp\b(.*?)\bend\b', re.DOTALL)


def _find_last_import_line(lines: list[str]) -> int:
    """Return the index of the last ``import …`` line, or ``-1``."""
    last = -1
    for i, line in enumerate(lines):
        if re.match(r'^\s*import\s+', line):
            last = i
    return last

def call_name(counter):
    ax=next(counter)
    return f'aspic_{ax}(ASPIC_OPT_{ax})'    

def preprocess(solvertype:str,source_code: str) -> str:
    """
    Convert ASP blocks to predicate calls and inject generated definitions.

    Returns the transformed source code.  If no ``asp … end`` blocks are
    found the original source is returned unchanged.
    """

    # 1. Locate every asp … end block
    matches = list(_ASP_BLOCK_RE.finditer(source_code))
    if not matches:
        return source_code

    # 2. For each block, write content to a temp file and call aspic_sub
    generated_snippets: list[str] = []
    for idx, match in enumerate(matches, start=1):
        asp_content = match.group(1)  # text between asp and end (excluded)

        with tempfile.NamedTemporaryFile(
            mode='w', suffix='.asp', delete=False, encoding='utf-8'
        ) as tmp:
            tmp.write(asp_content)
            tmp_path = tmp.name

        try:
#            print(["picat","-log", "aspic_transpiler.pi", solvertype+"/sub", str(idx), tmp_path])
            result = subprocess.run(
                ["picat","-log", "aspic_transpiler.pi", solvertype+"/sub", str(idx), tmp_path],
                #capture_output=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                text=True,
                check=True,
            )
            # Strip trailing whitespace but preserve the code itself
            generated_snippets.append(result.stderr.rstrip('\n\r'))
        except FileNotFoundError:
            print(
                "Error: external script 'aspic_sub' not found in PATH.",
                file=sys.stderr,
            )
            sys.exit(1)
        except subprocess.CalledProcessError as exc:
            print(
                f"Error: aspic_sub failed for block {idx} (exit code {exc.returncode}).\n"
                f"stderr: {exc.stderr}",
                file=sys.stderr,
            )
            sys.exit(1)
        finally:
            os.unlink(tmp_path)

    # 3. Replace every matched block with the corresponding predicate call
    counter = iter(range(1, len(matches) + 1))
    source_with_calls = _ASP_BLOCK_RE.sub(
        lambda _m: call_name(counter), source_code
    )

    # 4. Insert generated code after the last import line
    lines = source_with_calls.split('\n')
    insert_pos = _find_last_import_line(lines) + 1  # +1 so it's after imports

    # Build the block to insert (all snippets joined with newlines)
    insertion = '\n'.join(generated_snippets)
    insertion="import "+solvertype+".\n"+insertion
    # Insert – if there is existing text we need to make sure we don't lose it
    new_lines = lines[:insert_pos] + [insertion] + lines[insert_pos:]

    return '\n'.join(new_lines)


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

def main() -> None:
    if len(sys.argv) < 3:
        print(
            "Usage: python aspic_preprocessor.py <solvertype> <input_file> [output_file]\nwhere solvertype is asp or cp",
            file=sys.stderr,
        )
        sys.exit(1)

    solvertype = sys.argv[1]
    input_path = sys.argv[2]

    try:
        with open(input_path, 'r', encoding='utf-8') as fh:
            source_code = fh.read()
    except FileNotFoundError:
        print(f"Error: input file '{input_path}' not found.", file=sys.stderr)
        sys.exit(1)
    except OSError as exc:
        print(f"Error reading '{input_path}': {exc}", file=sys.stderr)
        sys.exit(1)

    processed = preprocess(solvertype,source_code)

    if len(sys.argv) >= 4:
        output_path = sys.argv[3]
        try:
            with open(output_path, 'w', encoding='utf-8') as fh:
                fh.write(processed)
        except OSError as exc:
            print(f"Error writing '{output_path}': {exc}", file=sys.stderr)
            sys.exit(1)
    else:
        sys.stdout.write(processed)


if __name__ == '__main__':
    main()
