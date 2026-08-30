#!/bin/sh
# Run the cp2 udf test(s).  cp2 = disk-compiled clone of lib/cp.pi
# (module cp2) in lib/cp2.pi;  cp2_udf_native.pi  is the only test -
# a zero-boilerplate showcase of udfs in CP constraints via  import cp2 .
# Requires emu/picat built.
#
#   sh exs/cp/run_cp2_udf.sh [testname ...]
#
# Notes (empirically observed in this 3.9#12 build):
#   * PICATPATH takes a single directory.
#   *  Read lookups (import / include / open) fall back to the
#     PICATPATH directories for bare names not found in the CWD
#     (picatpath_read_fallback in emu/file.c), so the files that
#     lib/cp2.pi includes need no copy into the temp CWD.
BASE=$(cd "$(dirname "$0")" && cd ../.. && pwd)
T=$(mktemp -d /tmp/cp2test.XXXXXX)
mkdir -p $T/lib
ln -sf $BASE/lib/*.pi $T/lib/
cd $T || exit 1
if [ $# -gt 0 ]; then set -- $@; else
  set -- cp2_udf_native
fi
for f in $@; do
  case $f in *.pi) ;; *) f=$f.pi;; esac
  [ -f $BASE/exs/cp/$f ] && cp $BASE/exs/cp/$f $T/
done
fail=0
for f in $@; do
  case $f in *.pi) ;; *) f=$f.pi;; esac
  echo "== $f"
  PICATPATH=$T/lib timeout 600 $BASE/emu/picat $f || fail=1
done
rm -rf $T
exit $fail
