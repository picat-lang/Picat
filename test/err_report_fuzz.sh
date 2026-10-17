#!/bin/bash
# err_report_fuzz.sh -- error-reporting quality experiment.
#
# Picks N random standalone programs from exs/, makes one temp copy of
# each, mutates ONE random character (change / delete / insert, chosen
# at random, at a random position in the file), then executes the copy
# and records the exit code and the reported error verbatim.
#
# usage: bash err_report_fuzz.sh [N] [SEED]
#   N    number of samples (default 30)
#   SEED shuf seed for reproducibility (default: random)
#
# Output: per-case blocks in scratch/errfuzz/report.txt (the mutation,
# the before/after line, the exit code, the full output) plus summary
# counts.  Each case is then analysed by hand to classify what the error
# message does and does not tell.  NOTE: the <<HERE>> marker sits AFTER
# the dumped rule text (2-100 lines), so only a full-output capture
# reveals it -- an earlier head-6 capture wrongly classified most parse
# errors as marker-less.

N=${1:-30}
SEED=${2:-$RANDOM}
PICAT_BIN=${PICAT_BIN:-/home/jovyan/snapshotable/q38fokus/Picat-src/emu/picat}
SRC_ROOT=${SRC_ROOT:-/home/jovyan/snapshotable/q38fokus/Picat-src/exs}
WORK=${WORK:-/home/jovyan/snapshotable/q38fokus/Picat-src/scratch/errfuzz}
PBSOL=${PBSOL:-/home/jovyan/snapshotable/q38fokus/scratch/roundingsat-src/build_my/roundingsat}
BOOST=${BOOST185:-/home/jovyan/snapshotable/q38fokus/scratch/boost185}

mkdir -p "$WORK"
REPORT="$WORK/report.txt"
: > "$REPORT"
echo "# err_report_fuzz: N=$N seed=$SEED picat=$(readlink -f "$PICAT_BIN" | xargs basename 2>/dev/null)" >> "$REPORT"

# the mutation alphabet: punctuation-heavy so most edits break syntax
CHARS='!@#%^&*()[]{};:,.<>/?|~=+-'

# the sample list: standalone programs only (a main/0 must exist)
ALL=$(for f in $(find "$SRC_ROOT" -name "*.pi"); do grep -qE "^[[:space:]]*main" "$f" && echo "$f"; done | shuf -n "$N" --random-source=<(yes "$SEED"))

ok=0; err=0; crash=0; tmo=0
for f in $ALL; do
  base=$(basename "$f")
  copy="$WORK/$base"
  cp "$f" "$copy"
  # bring the program's data files along (the mutated copy runs in $WORK)
  for sib in "$(dirname "$f")"/*; do
    case "$sib" in
      *.pi|*.pi~|*.qi) ;;
      *) [ -f "$sib" ] && cp "$sib" "$WORK/" 2>/dev/null ;;
    esac
  done
  lines=$(wc -l < "$f")
  [ "$lines" -lt 2 ] && continue
  # pick a random line and a random position inside it
  ln=$(shuf -i 1-"$lines" -n 1 --random-source=<(yes "$SEED$base"))
  oldline=$(sed -n "${ln}p" "$f")
  len=${#oldline}
  [ "$len" -eq 0 ] && { sed -n "${ln}p" "$f" > "$WORK/last"; continue; }
  col=$(shuf -i 0-$((len - 1)) -n 1 --random-source=<(yes "$SEED$base$ln"))
  # pick the mutation kind: c=hange, d=elete, i=nsert
  kind=$(printf 'c\nc\nc\nd\ni\n' | shuf -n 1 --random-source=<(yes "$SEED$base$ln$col"))
  ch=$(printf '%s' "$CHARS" | fold -w1 | shuf -n 1 --random-source=<(yes "$SEED$base$ln$col$kind"))
  case "$kind" in
    c) newline=$(printf '%s' "$oldline" | awk -v c="$col" -v ch="$ch" '{substr($0,c+1,1)=ch; print}') ;;
    d) newline=$(printf '%s' "$oldline" | awk -v c="$col" '{print substr($0,1,c) substr($0,c+2)}') ;;
    i) newline=$(printf '%s' "$oldline" | awk -v c="$col" -v ch="$ch" '{print substr($0,1,c) ch substr($0,c+1)}') ;;
  esac
  # splice the mutated line back
  awk -v n="$ln" -v nl="$newline" '{ if (NR==n) print nl; else print }' "$f" > "$copy"
  before=$(printf '%s' "$oldline" | cut -c1-78)
  after=$(printf '%s' "$newline" | cut -c1-78)

  out=$(cd "$WORK" && env PICATPATH=/home/jovyan/snapshotable/q38fokus/Picat-src/lib2 \
        PBSOL="$PBSOL" LD_LIBRARY_PATH="$BOOST" \
        timeout 10 "$PICAT_BIN" "$base" 2>&1)
  rc=$?
  {
    echo "=== $f kind=$kind line=$ln col=$col ch='$ch'"
    echo "--- before: $before"
    echo "--- after : $after"
    echo "--- rc=$rc"
    printf '%s\n' "$out" | sed 's/^/--- out: /'
    echo
  } >> "$REPORT"
  case "$rc" in
    0) ok=$((ok+1)) ;;
    1|2) err=$((err+1)) ;;
    124) tmo=$((tmo+1)) ;;
    *) crash=$((crash+1)) ;;
  esac
  rm -f "$copy"
done
echo "# summary: no-error=$ok  error-rc=$err  timeout=$tmo  other-rc=$crash" >> "$REPORT"
echo "report: $REPORT"
printf 'no-error=%s error=%s timeout=%s other=%s\n' "$ok" "$err" "$tmo" "$crash"
