#!/usr/bin/env bash
# speed_baseline.sh: time every shipped aspic example with the CURRENT aspic
# sources and with the UNMODIFIED upstream sources (git HEAD), and report the
# per-example and overall slowdown (or speedup) of the current version.
#
# Answers the question "does the v0.4.0 udf rewrite of the generated code
# slow the programs down?".  The measured step is the RUN of the generated
# program (compile + runtime load + the ASP learning + the solve); the
# transpile step is timed separately and reported as a total.
#
# Methodology (per example): one warmup run per version (the transpiler
# compiles its modules once, writing .qi bytecode the following runs load),
# then 3 timed runs; the MEDIAN is reported.  The satext portfolio races its
# solvers, so single runs are noisy - the median damps that.
#
# Usage: bash test/speed_baseline.sh
#   (runnable from anywhere; expects "picat" in the PATH and the git repo;
#   the repo's lib2 is put on PICATPATH for the udf layer; each run gets
#   120s; the run dirs live under the repo's scratch/ folder and are
#   removed at exit)
#
# Output: one line per example with the median run time of both versions and
# the ratio (new/old; >1 means the current version is slower), then the
# totals.  Exit status is always 0 (the script only reports).

set -u
ASPIC_DIR="$(cd "$(dirname "$0")/.." && pwd)"
REPO_DIR="$(cd "$ASPIC_DIR/.." && pwd)"
cd "$ASPIC_DIR" || exit 1

if ! command -v picat >/dev/null 2>&1; then
    echo "speed_baseline: picat not found in PATH" >&2
    exit 1
fi

export PICATPATH="$REPO_DIR/lib2${PICATPATH:+:$PICATPATH}"

WORK="$(mktemp -d "$REPO_DIR/scratch/speedbase_XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

BASE="$WORK/baseline"
mkdir -p "$BASE" || exit 1
PIFILES="aspic.pi aspic_transpiler.pi aspic_gen.pi aspic_prep.pi aspic_runtime_template.pi"
for f in $PIFILES; do
    git show "HEAD:aspic/$f" > "$BASE/$f" || exit 1
done

TMO=120
RUNS=3

# now_ms: the current time in milliseconds
now_ms() { date +%s%3N; }

# run_lp DIR SRCDIR FILE OUT: transpile + extract + execute; the run's wall
# time in ms is appended to OUT
run_lp() {
    local dir="$1" src="$2" file="$3" out="$4"
    mkdir -p "$dir"
    for f in $PIFILES; do cp "$src/$f" "$dir/" 2>/dev/null; done
    cp "examples/$file" "$dir/"
    [ -f examples/edges.txt ] && cp examples/edges.txt "$dir/"
    local t0 t1
    ( cd "$dir" \
      && timeout $TMO picat -log aspic_transpiler.pi run "$file" >/dev/null 2>gen.txt \
      && awk '/^import (sat|cp|aspic_runtime)\./{found=1} found' gen.txt > gen.pi \
      && t0=$(now_ms) \
      && timeout $TMO picat -log -s 1234567890 gen.pi >run_out.txt 2>&1 \
      && t1=$(now_ms) \
      && echo $((t1-t0)) >> "$out" )
}

# median LIST: the median of the whitespace-separated numbers in LIST
median() {
    echo "$1" | tr ' ' '\n' | sort -n | awk '{a[NR]=$1} END{if(NR%2) print a[(NR+1)/2]; else print int((a[NR/2]+a[NR/2+1])/2)}'
}

printf "%-24s %10s %10s %8s\n" "example" "new_ms" "old_ms" "ratio"
printf "%-24s %10s %10s %8s\n" "------------------------" "------" "------" "-----"

sum_new=0; sum_old=0; n=0
for f in examples/*.lp; do
    name=$(basename "$f")
    rm -f "$WORK/new_$name.txt" "$WORK/old_$name.txt" "$WORK/warm.txt"
    run_lp "$WORK/sp_new_$name" "$ASPIC_DIR" "$name" "$WORK/warm.txt"
    for i in $(seq $RUNS); do
        run_lp "$WORK/sp_new_$name" "$ASPIC_DIR" "$name" "$WORK/new_$name.txt"
    done
    run_lp "$WORK/sp_old_$name" "$BASE" "$name" "$WORK/warm.txt"
    for i in $(seq $RUNS); do
        run_lp "$WORK/sp_old_$name" "$BASE" "$name" "$WORK/old_$name.txt"
    done
    mn=$(median "$(cat "$WORK/new_$name.txt" 2>/dev/null)")
    mo=$(median "$(cat "$WORK/old_$name.txt" 2>/dev/null)")
    if [ -z "$mn" ] || [ -z "$mo" ] || [ "$mo" -eq 0 ]; then
        printf "%-24s %10s %10s %8s\n" "$name" "${mn:-fail}" "${mo:-fail}" "-"
        continue
    fi
    r=$((mn * 100 / mo))
    printf "%-24s %10s %10s %7s%%\n" "$name" "$mn" "$mo" "$r"
    sum_new=$((sum_new + mn)); sum_old=$((sum_old + mo)); n=$((n + 1))
done

if [ "$n" -gt 0 ] && [ "$sum_old" -gt 0 ]; then
    echo "--------------------------------------------------------------"
    printf "%-24s %10s %10s %7s%%\n" "TOTAL (sum of medians)" "$sum_new" "$sum_old" "$((sum_new * 100 / sum_old))"
fi
exit 0
