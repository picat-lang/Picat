#!/usr/bin/env bash
# Generate the env needed to build Picat with the nixpkgs emscripten package.
# Run inside `nix shell nixpkgs#emscripten`, from the emu/ directory:
#
#   nix shell nixpkgs#emscripten --command ./setup-emscripten-nix.sh
#
# Produces:
#   - $HOME/.emscripten_cache_picat  (writable EM_CACHE; a cache under /tmp
#     breaks emcc's relative source paths on macOS, where /tmp is a symlink
#     to /private/tmp)
#   - llvm-shim/                     (LLVM bin dir with a wasm-ld wrapper that
#     drops --no-stack-first; nixpkgs pairs emcc 4.0.x with LLD 21, which
#     rejects that flag)
#   - emconfig.py                    (EM_CONFIG pointing LLVM_ROOT at the shim)
#
# Then build with:
#   EM_CONFIG=$PWD/emconfig.py EM_CACHE=$HOME/.emscripten_cache_picat \
#     nix shell nixpkgs#emscripten --command make -f Makefile.emscripten -j10
set -euo pipefail
cd "$(dirname "$0")"

EMCC=$(command -v emcc) || { echo "emcc not found — run inside: nix shell nixpkgs#emscripten" >&2; exit 1; }
EMROOT=$(realpath "$(dirname "$EMCC")/..")/share/emscripten
LLVM_BIN=$(sed -n "s/^LLVM_ROOT = '\(.*\)'$/\1/p" "$EMROOT/.emscripten")

# 1. Writable EM_CACHE under $HOME, seeded from the package's prebuilt cache.
CACHE="$HOME/.emscripten_cache_picat"
if [ ! -d "$CACHE" ]; then
    cp -R "$EMROOT/cache" "$CACHE"
    chmod -R u+w "$CACHE"
fi

# 2. wasm-ld shim that filters --no-stack-first.
rm -rf llvm-shim
mkdir llvm-shim
for f in "$LLVM_BIN"/*; do
    ln -s "$f" llvm-shim/
done
rm llvm-shim/wasm-ld
cat > llvm-shim/wasm-ld <<EOF
#!/bin/sh
n=\$#
i=0
while [ \$i -lt \$n ]; do
    a=\$1; shift
    [ "\$a" = "--no-stack-first" ] || set -- "\$@" "\$a"
    i=\$((i+1))
done
exec "$LLVM_BIN/wasm-ld" "\$@"
EOF
chmod +x llvm-shim/wasm-ld

# 3. EM_CONFIG with LLVM_ROOT pointing at the shim.
sed "s|^LLVM_ROOT = .*|LLVM_ROOT = '$PWD/llvm-shim'|" "$EMROOT/.emscripten" > emconfig.py

echo "ok: EM_CACHE=$CACHE EM_CONFIG=$PWD/emconfig.py"
