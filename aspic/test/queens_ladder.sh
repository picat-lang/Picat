#!/usr/bin/env bash
# queens_ladder.sh: time the n-queens example (examples/queens5.lp) scaled to
# a ladder of n values with the CURRENT aspic sources and the UNMODIFIED
# upstream sources (git HEAD), to see how the v0.4.0 udf rewrite scales.
#
# The example's header gives the reference times (with completion on
# AMD EPYC 9654): 128 -> 0.9 s, 256 -> 3.2 s, 512 -> 26.2 s, 768 -> 89.3 s,
# 1024 -> 944.6 s.  The ladder tops out at 768: 1024 exceeds the 5-minute
# per-run cap this script enforces (timeout 300 s per run).
#
# Methodology (per n, per version): a copy of queens5.lp with
# "#const n = <k>." is transpiled, the generated program is extracted with
# awk (everything from the first "import sat/cp/aspic_runtime." line), run
# once as a warmup (the first run compiles gen.pi, writing .qi bytecode the
# following runs load), then timed; small points get 2 timed runs, points
# from 512 up get 1 (their runs are minutes long).
#
# Usage: bash test/queens_ladder.sh
#   (runnable from anywhere; expects "picat" in the PATH and the git repo;
#   the repo's lib2 is put on PICATPATH for the udf layer; the run dirs live
#   under the repo's scratch/ folder and are removed at exit)
#
# Output: one line per n with the run times of both versions (the median of
# the timed runs) and the ratio (new/old; >100% means the current version is
# slower).  Exit status is always 0 (the script only reports).

set -u
ASPIC_DIR="$(cd "$(dirname "$0")/.." && pwd)"
REPO_DIR="$(cd "$ASPIC_DIR/.." && pwd)"
cd "$ASPIC_DIR" || exit 1

if ! command -v picat >/dev/null 2>&1; then
    echo "queens_ladder: picat not found in PATH" >&2
    exit 1
fi

export PICATPATH="$REPO_DIR/lib2${PICATPATH:+:$PICATPATH}"

WORK="$(mktemp -d "$REPO_DIR/scratch/queenslad_XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

BASE="$WORK/baseline"
mkdir -p "$BASE" || exit 1
for f in aspic_transpiler.pi aspic_runtime_template.pi; do
    git show "HEAD:aspic/$f" > "$BASE/$f" || exit 1
done

LADDER="8 16 32 64 128 256 512 768"
TMO=300

# now_ms: the current time in milliseconds
now_ms() { date +%s%3N; }

# gen DIR SRCDIR N: transpile the n-queens copy of the given source version
# and extract the generated program.  The transpiler writes the runtime with
# "sed ... aspic_runtime_template.pi > aspic_runtime.pi" reading the template
# from the CWD, so the version-matched template is copied into the run dir
# first (without it the sed fails and aspic_runtime.pi is broken).
gen() {
    local dir="$1" src="$2" n="$3"
    mkdir -p "$dir"
    cp "$src/aspic_runtime_template.pi" "$dir/" || return 1
    sed "s/^#const n = 128\./#const n = $n./" examples/queens5.lp > "$dir/queens.lp"
    ( cd "$dir" \
      && PICATPATH="$REPO_DIR/lib2" timeout $TMO picat -log "$src/aspic_transpiler.pi" run queens.lp >/dev/null 2>gen.txt \
      && awk '/^import (sat|cp|aspic_runtime)\./{found=1} found' gen.txt > gen.pi ) || return 1
}

# run_timed DIR N OUT: run the generated program once as a warmup (the first
# run compiles gen.pi, writing .qi bytecode), then time RUNS runs and append
# each run's wall time in ms to OUT
run_timed() {
    local dir="$1" n="$2" out="$3"
    local runs
    case "$n" in
    512|768) runs=1 ;;
    *)       runs=2 ;;
    esac
    local i t0 t1
    for i in $(seq $((runs + 1))); do
        ( cd "$dir" \
          && t0=$(now_ms) \
          && PICATPATH="$dir:$REPO_DIR/lib2" timeout $TMO picat -log -s 1234567890 gen.pi >run_out.txt 2>&1 \
          && t1=$(now_ms) \
          && echo $((t1 - t0)) >> "$out" )
    done
}

# median LIST: the median of the whitespace-separated numbers in LIST
median() {
    echo "$1" | tr ' ' '\n' | sort -n | awk '{a[NR]=$1} END{if(NR%2) print a[(NR+1)/2]; else print int((a[NR/2]+a[NR/2+1])/2)}'
}

printf "%6s %10s %10s %8s\n" "n" "new_ms" "old_ms" "ratio"
printf "%6s %10s %10s %8s\n" "------" "------" "------" "-----"

for n in $LADDER; do
    rm -f "$WORK/new_$n.txt" "$WORK/old_$n.txt"
    gen "$WORK/n_${n}_new" "$ASPIC_DIR" "$n"
    run_timed "$WORK/n_${n}_new" "$n" "$WORK/new_$n.txt"
    gen "$WORK/n_${n}_old" "$BASE" "$n"
    run_timed "$WORK/n_${n}_old" "$n" "$WORK/old_$n.txt"
    mn=$(median "$(cat "$WORK/new_$n.txt" 2>/dev/null)")
    mo=$(median "$(cat "$WORK/old_$n.txt" 2>/dev/null)")
    if [ -z "$mn" ] || [ -z "$mo" ] || [ "$mo" -eq 0 ]; then
        printf "%6s %10s %10s %8s\n" "$n" "${mn:-fail}" "${mo:-fail}" "-"
        continue
    fi
    printf "%6s %10s %10s %7s%%\n" "$n" "$mn" "$mo" "$((mn * 100 / mo))"
done
exit 0
