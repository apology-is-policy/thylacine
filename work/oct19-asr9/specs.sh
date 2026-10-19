#!/bin/sh
# AS-R9 backport specs (asr9 worktree). The BASELINE first: main 115465888's
# burrow.tla and its four cfgs under this jar must give the recorded P4-N
# counts (100 clean; 66/54/43 buggy on NoUseAfterFree). A red baseline is a
# toolchain question before it is a model question. Then the changed model
# through specs/check-burrow.sh, and capacity.tla through check-capacity.sh.
# Run from the corona checkout root: sh work/oct19-asr9/specs.sh
set -u
HERE=/Users/northkillpd/projects/thylacine-corona
ROOT=/Users/northkillpd/projects/thylacine-asr9
BASE=115465888
JAR=/tmp/tla2tools.jar
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
[ "$(pwd)" = "$HERE" ] || { echo "REFUSING: run from $HERE"; exit 3; }
OUT=$HERE/work/oct19-asr9/specs-$(date -u '+%Y%m%dT%H%M%SZ')
mkdir -p "$OUT/base"
echo "asr9 HEAD $(git -C $ROOT rev-parse HEAD); dirty: $(git -C $ROOT status --porcelain --untracked-files=no -- specs | tr '\n' ' ')" | tee "$OUT/summary.txt"
echo "jar $(shasum -a 256 $JAR | cut -c1-16); $(java -cp $JAR tlc2.TLC -h 2>&1 | grep -m1 -o 'TLC2 Version [^ ]* of [^)]*')" | tee -a "$OUT/summary.txt"
git -C $ROOT show $BASE:specs/burrow.tla > "$OUT/base/burrow.tla"
for c in burrow burrow_buggy_free_on_close burrow_buggy_free_on_unmap burrow_buggy_never_free; do
  git -C $ROOT show $BASE:specs/$c.cfg > "$OUT/base/$c.cfg"
done
for c in burrow burrow_buggy_free_on_close burrow_buggy_free_on_unmap burrow_buggy_never_free; do
  java -cp $JAR tlc2.TLC -workers auto -deadlock -metadir "$OUT/base/$c.meta" \
    -config "$OUT/base/$c.cfg" "$OUT/base/burrow.tla" > "$OUT/base/$c.log" 2>&1
  rc=$?
  st=$(grep -o '[0-9]* distinct states found' "$OUT/base/$c.log" | tail -1 | awk '{print $1}')
  dp=$(grep -o 'depth of the complete state graph search is [0-9]*' "$OUT/base/$c.log" | awk '{print $NF}')
  vi=$(grep -o 'Invariant [A-Za-z]* is violated' "$OUT/base/$c.log" | head -1)
  echo "BASE $c: rc=$rc states=$st depth=${dp:--} ${vi:-no violation}" | tee -a "$OUT/summary.txt"
done
rm -f "$OUT"/base/burrow_TTrace_* 2>/dev/null
echo "== NEW: specs/check-burrow.sh ==" | tee -a "$OUT/summary.txt"
sh $ROOT/specs/check-burrow.sh > "$OUT/check-burrow.log" 2>&1; echo "check-burrow rc=$?" | tee -a "$OUT/summary.txt"
cat "$OUT/check-burrow.log" | tee -a "$OUT/summary.txt" > /dev/null
echo "== capacity: specs/check-capacity.sh ==" | tee -a "$OUT/summary.txt"
sh $ROOT/specs/check-capacity.sh > "$OUT/check-capacity.log" 2>&1; echo "check-capacity rc=$?" | tee -a "$OUT/summary.txt"
cat "$OUT/check-capacity.log" >> "$OUT/summary.txt"
echo "stray java after the run:" >> "$OUT/summary.txt"; ps -axo pid,pcpu,etime,command | grep "[t]lc2.TLC" >> "$OUT/summary.txt" || echo "  none" >> "$OUT/summary.txt"
echo "OUT=$OUT"
