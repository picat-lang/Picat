# WebAssembly build of Picat

Builds the full Picat engine (incl. CP, SAT/kissat, espresso, FANN) to wasm32
with Emscripten. The bytecode image (`picat_bc.h`) is stored as portable `int`
arrays, so the 32-bit (non-`M64BITS`) build loads it unchanged.

## Build

```sh
cd emu
# one-time: generate emconfig.py, llvm-shim/, and a writable EM_CACHE
nix shell nixpkgs#emscripten --command ./setup-emscripten-nix.sh

EM_CONFIG=$PWD/emconfig.py EM_CACHE=$HOME/.emscripten_cache_picat \
  nix shell nixpkgs#emscripten --command make -f Makefile.emscripten -j10
```

(With a non-nix emsdk install, plain `make -f Makefile.emscripten` should
work and none of the workarounds below are needed.)

Outputs:
- `picat.js` / `picat.wasm` — embeddable build (web, worker, node; MEMFS,
  exports `FS` and `callMain`)
- `picat-node.js` / `picat-node.wasm` — node CLI build (`NODERAWFS`, real
  filesystem): `node picat-node.js ../exs/cp/queens.pi`

## nixpkgs-emscripten workarounds (what setup-emscripten-nix.sh does)

1. **`/tmp` symlink breaks system-lib rebuilds (macOS).** The nixpkgs wrapper
   puts `EM_CACHE` under `/tmp`, which is a symlink to `/private/tmp`; emcc's
   relative source paths (`../../../../nix/store/...`) then resolve to
   `/private/nix/...` and clang fails. Fix: seed a cache under `$HOME` (copy
   the wrapper's `/tmp/...-emscripten-*_cache`) and pass `EM_CACHE`.

2. **emcc/LLD version mismatch.** nixpkgs pairs emcc 4.0.21 with LLD 21, which
   doesn't know `--no-stack-first` (emcc expects LLVM 22). `llvm-shim/` is a
   symlink farm of the real LLVM bin dir where `wasm-ld` is a wrapper that
   drops that flag; `emconfig.py` points `LLVM_ROOT` at it. Re-run the setup
   script if the nix store paths change.

3. `-sAUTO_NATIVE_LIBRARIES=0` skips libGL/libal/libhtml5 (not needed, and
   their rebuild also trips issue 1).

## Source change

`file.c` `get_socket_fd()`: musl's `FILE` is opaque, so the glibc
`fdes->_fileno` access is replaced with `fileno()` under `__EMSCRIPTEN__`.

## Notes

- pthreads are linked as Emscripten's single-threaded stubs; Picat timers
  (`event.c`) won't spawn real threads.
- Verified: REPL arithmetic, `cp` 8/100-queens, `sat` (kissat) solving,
  running `.pi` files from disk via the node build.
