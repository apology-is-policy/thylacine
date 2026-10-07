#!/bin/bash
# Tests pool_restore in tools/smp-multiboot.sh -- the function as EXTRACTED from
# the live file, so this cannot pass against a stale duplicate. No lease, no
# build, no guest.
#
# Why: pool_restore is the first statement of smp-multiboot's per-boot loop and
# is what makes N boots N INDEPENDENT boots. Its missing-snapshot arm used to
# return success mutely (yip 0187, main's option (b)). The arms below are the
# ones a reader needs to trust: deliberate opt-out stays silent, stale twins
# keep their existing announcement, and an impossible restore is now fatal.
#
#   bash work/oct5-as-r9/pool-restore-test.sh [smp-multiboot.sh]
SRC=${1:-tools/smp-multiboot.sh}
[[ -f "$SRC" ]] || { echo "no script at $SRC"; exit 2; }
T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT
FN=$T/fn.sh
awk '/^pool_restore\(\) \{/,/^\}$/' "$SRC" > "$FN"
# DENOMINATOR CONTROL: a silent extraction failure would make every arm vacuous.
n=$(wc -l < "$FN" | tr -d ' ')
# Threshold catches a FAILED extraction, not a smaller function: the pre-patch
# pool_restore is a legitimate 9 lines, and a control tuned to the current size
# would refuse to compare against it. The content check below is the real
# discriminator.
[[ "$n" -ge 5 ]] || { echo "REFUSING: extracted only $n lines of pool_restore from $SRC"; exit 2; }
grep -q 'SMP_GATE_POOL_RESTORE' "$FN" || { echo "REFUSING: extraction is not pool_restore"; exit 2; }
echo "-- exercising pool_restore as extracted from $SRC ($n lines)"

pass=0; fail=0
ck() { if [[ "$2" == "$3" ]]; then pass=$((pass+1)); else fail=$((fail+1)); echo "  WRONG: $1 -- expected [$2] got [$3]"; fi; }
setup() { # setup <dir> -- a complete, consistent twin set
  rm -rf "$1"; mkdir -p "$1"
  echo SNAPSHOT-POOL > "$1/pool.img.baked-snapshot"
  echo DIRTY-POOL    > "$1/pool.img"
  echo KEY           > "$1/system.key"
  echo KEY           > "$1/system.key.baked-snapshot"
}
run() { # run <dir> -- call the real function against that dir
  ( LABEL=test
    POOL_IMG="$1/pool.img";  POOL_SNAP="$POOL_IMG.baked-snapshot"
    KEY_IMG="$1/system.key"; KEY_SNAP="$KEY_IMG.baked-snapshot"
    source "$FN"; pool_restore ) 2> "$1/err"
}

echo "== A: complete set -- restores, silently, and the POOL ACTUALLY CHANGES =="
setup "$T/a"; run "$T/a"; ck "A rc" 0 $?
ck "A silent" "" "$(cat "$T/a/err")"
ck "A pool restored" "SNAPSHOT-POOL" "$(cat "$T/a/pool.img")"

echo "== B: snapshot missing -- FATAL and names the file =="
setup "$T/b"; rm -f "$T/b/pool.img.baked-snapshot"; run "$T/b"; ck "B rc" 1 $?
ck "B says IMPOSSIBLE" 1 "$(grep -c 'pool restore IMPOSSIBLE' "$T/b/err")"
ck "B names the snapshot" 1 "$(grep -c 'pool.img.baked-snapshot' "$T/b/err")"
ck "B explains independence" 1 "$(grep -c 'NOT independent' "$T/b/err")"

echo "== C: pool.img missing -- FATAL and names THAT file, not the snapshot =="
setup "$T/c"; rm -f "$T/c/pool.img"; run "$T/c"; ck "C rc" 1 $?
ck "C names pool.img" 1 "$(grep -c "c/pool.img$" "$T/c/err")"

echo "== D: deliberate opt-out -- still silent, still success (arm unchanged) =="
setup "$T/d"; rm -f "$T/d/pool.img.baked-snapshot"
( LABEL=test SMP_GATE_POOL_RESTORE=0
  POOL_IMG="$T/d/pool.img"; POOL_SNAP="$POOL_IMG.baked-snapshot"
  KEY_IMG="$T/d/system.key"; KEY_SNAP="$KEY_IMG.baked-snapshot"
  source "$FN"; pool_restore ) 2> "$T/d/err"
ck "D rc" 0 $?
ck "D silent" "" "$(cat "$T/d/err")"

echo "== E: stale twins -- existing announcement kept, still non-fatal =="
setup "$T/e"; echo OTHER > "$T/e/system.key"; run "$T/e"; ck "E rc" 0 $?
ck "E says SKIPPED" 1 "$(grep -c 'pool restore SKIPPED' "$T/e/err")"
ck "E did NOT restore" "DIRTY-POOL" "$(cat "$T/e/pool.img")"

echo
echo "RESULT: $pass pass, $fail wrong"
[[ "$fail" == 0 ]] || exit 1
