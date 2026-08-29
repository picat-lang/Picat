#!/bin/sh
# Run the sat2 udf test(s).  sat2 = disk-compiled clone of lib/sat.pi
# (module sat2) in lib/sat2.pi;  sat2_udf_native.pi  is the only test -
# a zero-boilerplate showcase of udfs in SAT constraints via  import sat2 .
# Requires emu/picat built.
#
#   sh exs/sat/run_sat2_udf.sh [testname ...]
#
# Notes (empirically observed in this 3.9#12 build):
#   * PICATPATH takes a single directory.
#   *  include "..."  resolves against the CWD, so  cp_sat_mip_smt.pi
#     (included by lib/sat2.pi) is copied into the temp CWD.
BASE=$(cd "$(dirname "$0")" && cd ../.. && pwd)
T=$(mktemp -d /tmp/sat2test.XXXXXX)
mkdir -p $T/lib
ln -sf $BASE/lib/*.pi $T/lib/
cp $BASE/lib/cp_sat_mip_smt.pi $BASE/lib/global_reif.pi $T/
cd $T || exit 1
if [ $# -gt 0 ]; then set -- $@; else
  set -- sat2_udf_native
fi
for f in $@; do
  case $f in *.pi) ;; *) f=$f.pi;; esac
  [ -f $BASE/exs/sat/$f ] && cp $BASE/exs/sat/$f $T/
done
fail=0
for f in $@; do
  case $f in *.pi) ;; *) f=$f.pi;; esac
  echo "== $f"
  PICATPATH=$T/lib timeout 120 $BASE/emu/picat $f || fail=1
done
rm -rf $T
exit $fail
