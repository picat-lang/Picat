#!/bin/bash
# make_release_tgz.sh -- build a self-contained linux64 release tgz of the
# experimental Picat branch.
#
# What it does:
#   1. builds the picat binary STATICALLY (runs on any x86-64 Linux,
#      new or old, regardless of the installed glibc/libstdc++; the code
#      and -O3 flags are identical to the normal dynamic build, so it is
#      no slower on newer machines)
#   2. stages the binary, the fdn_hook.pi file that must sit next to it,
#      the runtime modules of lib2/, a `picat` wrapper that sets
#      PICATPATH relative to itself, and a release README
#   3. packs everything as picat-experimental-linux64-<commit>.tgz in the
#      repo root
#   4. verifies the tgz by unpacking it into a scratch dir and running
#      checks against the unpacked copy
#   5. rebuilds the normal DYNAMIC binary so the working tree build
#      artifact is unchanged afterwards
#
# Usage: bash tools/make_release_tgz.sh
# Output: picat-experimental-linux64-<commit>.tgz next to this repo's root.

set -e

REPO=$(cd "$(dirname "$0")/.." && pwd)
EMU="$REPO/emu"
COMMIT=$(git -C "$REPO" rev-parse --short HEAD)
STAGE="$REPO/scratch/reldir"
PKGDIR="picat-experimental-linux64-$COMMIT"
TGZ="$REPO/$PKGDIR.tgz"
CHECK="$REPO/scratch/relcheck"

echo "== building the static binary (commit $COMMIT)"
make -C "$EMU" -f Makefile.linux64 portable > "$REPO/scratch/build-release-static.log" 2>&1
if ! ldd "$EMU/picat-portable" 2>&1 | grep -q "not a dynamic executable"; then
    echo "FAIL: the binary is not static" >&2; exit 1
fi

echo "== staging"
rm -rf "$STAGE"
mkdir -p "$STAGE/$PKGDIR/bin" "$STAGE/$PKGDIR/lib2"
cp "$EMU/picat-portable" "$STAGE/$PKGDIR/bin/picat"
cp "$EMU/fdn_hook.pi" "$STAGE/$PKGDIR/bin/fdn_hook.pi"
cp "$EMU/satshim" "$STAGE/$PKGDIR/bin/satshim"
cp "$REPO"/lib2/*.pi "$STAGE/$PKGDIR/lib2/"

cat > "$STAGE/$PKGDIR/picat" <<EOF
#!/bin/bash
# Picat experimental (linux64, statically linked) launcher.
# Sets PICATPATH to this distribution's lib2 so all shipped modules load.
DIR=\$(cd "\$(dirname "\$(readlink -f "\$0")")" && pwd)
export PICATPATH="\$DIR/lib2\${PICATPATH:+:\$PICATPATH}"
exec "\$DIR/bin/picat" "\$@"
EOF
chmod +x "$STAGE/$PKGDIR/picat"

sed -e "s/@COMMIT@/$COMMIT/g" "$REPO/tools/release-README.md.in" > "$STAGE/$PKGDIR/README.md"

echo "== packing"
tar czf "$TGZ" -C "$STAGE" "$PKGDIR"

echo "== verifying (unpack + run)"
rm -rf "$CHECK"; mkdir -p "$CHECK"
tar xzf "$TGZ" -C "$CHECK"
P="$CHECK/$PKGDIR/picat"

fail=0
banner=$(timeout 10 "$P" 2>/dev/null | head -1) || true
case "$banner" in Picat*) echo "  banner: $banner";; *) echo "  FAIL: no banner ('$banner')"; fail=1;; esac

printf 'main => println("ok").\n' > "$CHECK/plain.pi"
timeout 30 bash -c "'$P' '$CHECK/plain.pi' | grep -q ok" && echo "  plain example: ok" || { echo "  FAIL: plain example"; fail=1; }
"$P" -c "$CHECK/plain.pi" >/dev/null 2>&1 && echo "  -c clean file: rc 0" || { echo "  FAIL: -c clean"; fail=1; }

printf 'import cp.\nmain => X :: 1..5, X #= 3, solve([X]), println(X).\n' > "$CHECK/cptest.pi"
timeout 60 bash -c "'$P' '$CHECK/cptest.pi' | grep -q 3" && echo "  cp example: ok" || { echo "  FAIL: cp example"; fail=1; }

printf 'import satext.\nmain => true.\n' > "$CHECK/modtest.pi"
timeout 60 bash -c "'$P' '$CHECK/modtest.pi' >/dev/null 2>&1" && echo "  lib2 module via wrapper: rc 0" || { echo "  FAIL: lib2 module"; fail=1; }
printf 'import no_such_module_zz.\nmain => true.\n' > "$CHECK/broken.pi"
timeout 10 "$P" -c "$CHECK/broken.pi" >/dev/null 2>&1 && { echo "  FAIL: -c broken file"; fail=1; } || echo "  -c broken file (failed import): rc 1"

echo "== restoring the dynamic build"
rm -f "$EMU/picat-portable"

rm -rf "$CHECK" "$STAGE"
if [ "$fail" -ne 0 ]; then echo "FAILED" >&2; exit 1; fi
echo "OK: $TGZ ($(du -h "$TGZ" | cut -f1))"
