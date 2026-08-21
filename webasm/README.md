# webasm -- Picat in the browser (WebAssembly)

Builds a wasm32, browser-runnable version of the Picat interpreter with
emscripten.  The design keeps this build **unmingled** with the rest of the
tree:

1. the C sources are *copied* into `build/stage/`,
2. `patches/*.patch` (lexicographic order) are applied **to that copy only**
   (`patch -p1 -d build/stage`) -- `emu/` is never modified,
3. `emcc` compiles the staged copy and links `dist/picat.js` +
   `dist/picat.wasm` (+ `dist/picat.data`, the preloaded filesystem).

## Build and run

```sh
make            # build dist/ (first run stages + compiles ~165 files)
make serve      # serve dist/ on http://localhost:8000  -> open index.html
make clean
```

Prerequisites:

* an emsdk activation (`EMSDK`, default `~/emsdk`; `emcc` must be in
  `$EMSDK/upstream/emscripten/`),
* the precompiled runtime libraries in `../lib2` (from the normal native
  build) -- they are preloaded into the wasm filesystem at `/lib2`,
* `examples/` (preloaded at `/examples`).

 The page is a split editor/terminal:

* the **editor** (left) holds the source; its toolbar carries the
  **File** menu at the left edge and **Run** (or Ctrl/Cmd-Enter) at the
  right corner,
* the **example** select (top right) loads a preloaded program into the
  editor (it does not run it),
* the **File** menu opens a local `.pi` file into the editor, or saves
  the editor contents as a download,
* the **terminal** (right) shows the program's stdout/stderr.

How a Run works: the page writes the editor text to the virtual file
system as `/user_code.pi` and calls `browser_rerun()`, which makes the
interpreter compile/load that file and call its `main` via
`$bp_first_call`. About the module line: a program may omit it
entirely; if it has one, the page retargets the name to `user_code`,
since picat requires it to match the file name. The page never
*adds* a module line: that is not merely unnecessary, for the planner
library it is harmful (`plan`/`plan_unbounded` resolve `final//action/`
in the global module, so a program with a module name breaks).
The run itself is a synchronous wasm call that blocks the main thread,
so it is started on the frame after the click: the Run button greys
out and reads "Running…" and the status bar shows `running…` for the
whole duration (with no repaint possible in between, an immediately
started run would only show its starting state in its last instant).
The runtime is bootstrapped once (`browser_boot("-p /lib2")`) on the
first Run. Uncaught picat errors make the interpreter call
`exit(1)`; the build links with `-sNO_EXIT_RUNTIME`, so the runtime
survives and the next Run works again.

Long heavy runs (the large SAT examples) can exhaust the interpreter:
after about three consecutive solves, the next `browser_rerun()` ends
immediately with status 0 (or traps) before doing any work. This is a
limitation of repeated `$bp_first_call` inside one interpreter (state
left by a heavy solve is not fully reclaimed between runs), not a
program error. A run can only be exhausted after earlier runs
succeeded on the same module, so the page treats a sub-50 ms failure
after a prior success as exhaustion: it re-creates the whole module
(a fresh `initialize_bprolog` and a fresh emulated filesystem
re-created from the preload image — a clean "restart picat" that keeps
all the example data files) and retries the same run once. The
terminal then shows `[interpreter exhausted — resetting and
retrying…]` and the finish line gains an
`(after interpreter reset)` note.

The 100 packed examples (`examples/`) came from the repository's
`exs/` collection, renamed with their folder as prefix
(`cp_kakuro.pi`, `sat_bqueens.pi`, `planner_sokoban.pi`,
`euler_p1.pi`, `nn_spam_test.pi`, ...). Every candidate was tested on
the wasm runtime; what did not make it: `mip/`, `smt/`, `parallel/`
and `satext/` (back-ends the browser build excludes), the interactive
and missing-data `nn` programs, `sat/numberlink_b.pi` (too slow on
wasm) and `euler/p108.pi` (times out even natively). The data files of
the file-reading examples are listed in `DATA_SRC` in the Makefile and
preloaded at the root of the virtual FS, because the programs open
them with relative names and the wasm CWD is `/`.

Verified headlessly under node (same calls the page makes): all 100
preloaded examples run to a clean end (status 1) and match the native
output; the runtime survives a program with a syntax error and keeps
running afterwards. Repeating the heavy SAT examples (e.g. running
`sat_bqueens.pi` 12 times in a row) ends all runs clean, with the
interpreter transparently restarted every three runs; small-SAT and
planner runs repeat 5-10 times in a single module without a restart.

## What is built and what is excluded

The source list is the native object list (`emu/common.mak`), minus pieces
that cannot work in a browser or that are not in the standard build:

| excluded | why |
| --- | --- |
| `thread.c` | pthreads; `Cboot_thread()` is stubbed in `browser_stub.c` (thread() predicates are simply not registered) |
| `satext.c`, `satshim.c`, `sapi.c` | external-solver support (fork/exec, shared memory); the ~12 symbols `kissat_picat.c` references are stubbed in `browser_stub.c` (`satext_ext_prepare() -> 0`, so the built-in solver path is taken) |
| `glpk_bp.c`, `scip_picat.c`, `qc.c`, `kissat24_picat.c`, `sat_bp.c`, `plc_java.c`, `maple_interface.cpp` | LP/Java back-ends, not part of the standard build at all |
| `jmp_table.c`, `assert.c`, `expand.c`, `load_inst.c`, `temp.c`, `satshim.c` | `#include`d helpers or dead sources (not separate objects natively; `jmp_table.c` is still *staged*, since `toam.c` includes it) |

Notes:

* Compiles as **wasm32** (no `-DM64BITS`), with the same defines as the
  native linux64 build otherwise (`-DGC -DGCC -DPICAT -DSAT -Dunix
  -DPOSIX -DFANN_NO_DLL -DFANN`), plus the fann sources (`emu/fann/`,
  with `fann_interface.cpp` via `em++`).
* **SAT is built in**: `-DSAT` registers the `c_sat_*` built-ins and
  links the built-in kissat solver (`kissat/src/kis_*.c`, the native
  `KISSAT_OBJ` list, `-DNEMBEDDED` so its `main()` is compiled out).
  `import sat.` therefore works; external solvers (satext) do not, and
  are silently off.  kissat's headers use `UINT_MAX`/`INT_MIN` without
  including `<stdint.h>`/`<limits.h>` (glibc drags that in implicitly),
  so the Makefile force-includes the emscripten sysroot `limits.h` by
  absolute path (a bare `-include limits.h` would resolve to kissat's
  own `limits.h` through the `-I` search path).
* Linking uses emscripten's libc++ (`-lc++ -lc++abi`).
* `get_socket_fd` is neutered (patch 0002) because emscripten's `FILE`
  does not expose glibc's `_fileno`.
* `currentTime()` uses `time_t` (patch 0001) because emscripten's `time_t`
  is 64-bit.

## Patches

`patches/` currently contains:

* `0001-cpreds-time_t.patch` -- `long t` -> `time_t t` in `currentTime()`,
* `0002-file-socket-fd.patch` -- `get_socket_fd` reports "not supported".

Add a new patch with `--- emu/<file>` / `+++ emu/<file>` unified-diff
headers (the leading `emu/` component is stripped with `-p1`), then run
`make clean && make` (the stage is rebuilt from scratch whenever the
inputs to it change).

## Limitations

* no *external* SAT solvers (satext, fork/exec), no MIP/SMT/LP
  back-ends (they need external binaries), no concurrent predicates
  (`thread/...`), no socket file descriptors,
* memory grows on demand (`ALLOW_MEMORY_GROWTH`), initial 16 MB,
  growth capped at 2 GB (wasm32).
