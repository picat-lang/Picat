# picat-run — running Picat files as executable scripts

`tools/picat-run` starts a Picat program like a Python or shell script:

```
./prog arg1 arg2
```

with a first line such as

```
#!/usr/bin/env picat-run
```

and without any change to Picat's parser. This document describes the
tool, why it is needed, how it works, how to install and test it, and
what it does not do.

Throughout: *picat* is the Picat interpreter; *PICATPATH* is the
colon-separated list of directories picat searches for modules
(`import m.` loads `m.pi` from there); *the fdn build* is the
experimental multicore FD solver build of picat in `emu/` (see
`exs/fd_native_mt/README.md`).

## 1. Why `#!/usr/bin/env picat` does not work

Two independent obstacles, both checked with the release 3.9#12 binary:

1. **The parser rejects the `#!` line.** A file starting with
   `#!/usr/bin/env picat` fails with

   ```
   *** SYNTAX ERROR *** (1-2) wrong rule.
   #!/u <<HERE>> sr/bin/env picat
   ```

   The tokenizer reads `#!` as the start of an operator such as `#!=`
   (`emu/token.c`). Unlike `%`, a `#!` line is not a comment.

2. **Picat only loads files named `*.pi`.** Each of these fails with
   `existence_error`:

   - `picat noext` (a file without the extension),
   - `picat /dev/stdin < prog.pi`,
   - `picat <(cat prog.pi)`.

   So `env` cannot hand picat an extensionless script, and a filter
   cannot pipe a cleaned-up text into it.

`env -S` only splits the `#!` line into arguments; neither obstacle goes
away. A pure `env -S sh -c '...'` one-liner could do what the wrapper
does, but it would be long, hard to read and close to the kernel's `#!`
length limit (256 bytes on Linux >= 5.1).

## 2. The tool

`tools/picat-run` is a POSIX shell script. `picat-run script [args...]`:

1. copies the script into a fresh `mktemp -d` directory as
   `<basename without .pi>.pi`, so the extension rule is met and a
   `module m.` declaration still matches the file name when the script
   is `m.pi`;
2. turns line 1 `#!...` into the comment `%#!...` with `sed`. The line
   is kept, so error positions `(line-line)` refer to the original
   file;
3. prepends the script's own directory to `PICATPATH`, so `import m.`
   finds a module `m.pi` next to the script whatever the current
   directory is;
4. runs `${PICAT:-picat} <copy> "$@"`. Picat calls `main(Args)` with
   the arguments, or `main/0` if there is no `main/1`. The exit code is
   picat's own (0, or 1 on error/failure);
5. deletes the temporary directory on exit (`trap ... EXIT HUP INT
   TERM`).

The full source is the file `tools/picat-run` itself (16 lines).

### Installing

```
install -m 755 tools/picat-run ~/.local/bin/   # any directory on PATH
# picat itself must be on PATH too, or: export PICAT=/path/to/picat
chmod +x myscript                              # the script with the #! line
./myscript a "b c"
```

`PICAT` selects the interpreter, e.g.
`PICAT=$PWD/emu/picat ./myscript` for the fdn build; the default is
`picat` on `PATH`.

### Testing

`tools/test_picat_run.sh` writes the example scripts into a temporary
directory and runs four cases:

| case | expected |
|---|---|
| `hello a "b c"` (extensionless, `main(Args)`, run directly) | `args = [a,b c]` on stdout, exit code 0 |
| `bad.pi` (a syntax error on line 3) | the error position `(3-4)`, i.e. the original line numbers; exit code nonzero |
| `sub/useit` run from its parent directory, `import helper.` with `helper.pi` next to the script | `from helper` on stdout, exit code 0 |
| an FD program (queens-12) with the fdn build | `STAT runtime_ms=` on stdout, exit code 0 |

Run:

```
tools/test_picat_run.sh          # picat on PATH; the fdn case runs if emu/picat is built
tools/test_picat_run.sh --fdn    # require the fdn case
PICAT=/path/to/picat tools/test_picat_run.sh
```

It prints PASS or FAIL per case and exits 0 only if all cases pass.

## 3. Caveats

- Not tested:
  - scripts that call `compile/1` or `load/1` on paths relative to
    their own location. Picat's current directory is the caller's, as
    for any script.
  - BSD/macOS `mktemp` and `sed` (both are called in their portable
    form).
- The temporary copy lives in `$TMPDIR` (default `/tmp`) while the
  script runs.
- A plain `picat script` without the wrapper still fails on the `#!`
  line. Scripts meant for both uses can omit the line and be run as
  `picat-run script.pi` or `picat script.pi`.

## 4. Alternative without a `#!` line: binfmt_misc

The kernel can run every executable `*.pi` file through the wrapper;
the script then needs no first line at all. This needs root and changes
the whole system. Not tested:

```
echo ':picat:E::pi::/usr/local/bin/picat-run:' > /proc/sys/fs/binfmt_misc/register
```

(`E` = match by extension.) The registration is lost on reboot unless
it is put in `/etc/binfmt.d/picat.conf` (same string, systemd-binfmt).

## 5. What was deliberately not done

Making picat's tokenizer skip a `#!` first line would be a two-line
patch in `emu/token.c`, but it was deliberately not done:

- it would not reach the goal on its own: picat would still only load
  `*.pi` files, so `#!/usr/bin/env picat` (an extensionless script)
  would still fail with `existence_error` — the wrapper is needed
  regardless;
- it changes the interpreter's core scanner in two modes (file input
  and string input) plus the UTF-8 entry, with subtleties (skip only at
  the file start, keep the line count, decide whether string-compiled
  sources get the same treatment) that are easy to get subtly wrong;
- the wrapper solves both obstacles with no change to the interpreter.
