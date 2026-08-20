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
make            # build dist/ (first run stages + compiles ~100 files)
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

* the **editor** (left) holds the source; **Run** (or Ctrl/Cmd-Enter)
  executes exactly what is in the editor,
* the **example** select loads a preloaded program into the editor
  (it does not run it),
* the **File** menu opens a local `.pi` file into the editor, or saves
  the editor contents as a download,
* the **terminal** (right) shows the program's stdout/stderr.

How a Run works: the page writes the editor text to the virtual file
system as `/user_code.pi` (rewriting a `module foo.` line to
`module user_code.`, since picat requires the module name to match the
file name), and calls `browser_rerun()`, which makes the interpreter
compile/load that file and call its `main` via `$bp_first_call`.
The runtime is bootstrapped once (`browser_boot("-p /lib2")`) on the
first Run. Uncaught picat errors make the interpreter call
`exit(1)`; the build links with `-sNO_EXIT_RUNTIME`, so the runtime
survives and the next Run works again.

To add an example, drop a `.pi` file into `examples/` (module name must
match the file name) and rebuild; the select is populated from the
preloaded directory at page load.

Verified headlessly under node (same calls the page makes): `hello.pi`
and `queens.pi` (8-queens, 92 solutions) both produce the same output
as the native interpreter, and the runtime survives a program with a
syntax error and keeps running afterwards.

## What is built and what is excluded

The source list is the native object list (`emu/common.mak`), minus pieces
that cannot work in a browser or that are not in the standard build:

| excluded | why |
| --- | --- |
| `thread.c` | pthreads; `Cboot_thread()` is stubbed in `browser_stub.c` (thread() predicates are simply not registered) |
| `satext.c`, `satshim.c`, `sapi.c`, `kissat/` | external-solver support (fork/exec, shared memory) |
| `kissat_picat.c`, built without `-DSAT` | SAT built-ins degrade to the "sat_not_supported" stubs |
| `glpk_bp.c`, `scip_picat.c`, `qc.c`, `kissat24_picat.c`, `sat_bp.c`, `plc_java.c`, `maple_interface.cpp` | LP/Java back-ends, not part of the standard build at all |
| `jmp_table.c`, `assert.c`, `expand.c`, `load_inst.c`, `temp.c`, `satshim.c` | `#include`d helpers or dead sources (not separate objects natively; `jmp_table.c` is still *staged*, since `toam.c` includes it) |

Notes:

* Compiles as **wasm32** (no `-DM64BITS`), with the same defines as the
  native linux64 build otherwise (`-DGC -DGCC -DPICAT -Dunix -DPOSIX
  -DFANN_NO_DLL -DFANN`), plus the fann sources (`emu/fann/`, with
  `fann_interface.cpp` via `em++`).
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

* no SAT/kissat built-ins, no concurrent predicates (`thread/...`),
  no socket file descriptors,
* memory grows on demand (`ALLOW_MEMORY_GROWTH`), initial 16 MB,
  growth capped at 2 GB (wasm32).
