#!/bin/sh
# Sabotage run for the main merge's restored witness: main's ref-rollback
# assertion in loom.register_rejects, which the merge had dropped by taking
# corona's restructured test whole. R1 (mutate.py) stops REGISTER's rollback
# releasing the Spoors it had already resolved; it is applied to the committed
# tree, built (ci image), booted through the whole suite, and reverted. R1 is
# KILLED only by the restored assertion's own FAIL; any other verdict, and any
# extinction, is recorded and counts as a survivor. build/ is left holding the
# mutant's kernel: run gate.sh afterwards. Derived from work/oct21-mag/mutants.sh.
# DRYRUN=1 stops after the preconditions (no lease needed, nothing built).
# Run from the checkout root (no cd): MERGE_PIN=<full sha> sh work/oct22-merge/mutants.sh
set -e
ROOT=/Users/northkillpd/projects/thylacine-corona
[ "$(pwd)" = "$ROOT" ] || { echo "REFUSING: run from $ROOT, not $(pwd)"; exit 3; }
STAMP=$(date -u '+%Y%m%dT%H%M%SZ')
OUT=work/oct22-merge/mutants-$STAMP
mkdir -p "$OUT"
YIP="${THYLA_WAKE_YIP:-$(command -v yip || echo "$HOME/.local/bin/yip")}"
MERGE_PIN=${MERGE_PIN:?set MERGE_PIN to the full sha under test}
export STRATUM_SRC=/Users/northkillpd/projects/stratum/v2
GOFORK_PIN_DIR=/Users/northkillpd/projects/go-thylacine-pin-3471e2a
GOFORK_PIN=3471e2ac027de4a5cbbee9337a3f335b96156e29
FLOOR_MB=${FLOOR_MB:-6144}
NEW="loom.register_rejects"
KILL_FAIL="^FAIL: and the resolved fd's ref was rolled back\$"
MUTANTS=${MUTANTS:-"R1"}
MUTATE="python3 -I work/oct22-merge/mutate.py"
record() { echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ')  $*" | tee -a "$OUT/stages.txt"; }
free_mb() { df -m . | awk 'NR==2 {print $4}'; }
floor() {
  _f=$(free_mb); record "disk $1: $_f MiB free (floor $FLOOR_MB)"
  [ "$_f" -ge "$FLOOR_MB" ] || { echo "REFUSING at $1: under the floor"; exit 3; }
}
lease() {
  [ -z "${DRYRUN:-}" ] || return 0
  "$YIP" resources > "$OUT/lease-$1.txt" 2>&1 || true
  /usr/bin/grep -qE '^mac +HELD by you' "$OUT/lease-$1.txt" || {
    echo "STOPPING before $1: the mac lease is not mine"; exit 6; }
}
dirty() { git status --porcelain --untracked-files=no | wc -l | tr -d ' '; }
MUTATED=0
restore() {
  git checkout -- kernel/syscall.c
  MUTATED=0
  [ "$(dirty)" = 0 ] || { echo "RESTORE left $(dirty) tracked change(s)"; exit 8; }
}
trap '_rc=$?; [ "$MUTATED" = 0 ] || restore; record "exit $_rc"' EXIT
# A signal death never reaches $? (sh runs the EXIT trap with the last
# completed command's status), so each signal exits with its own.
trap 'exit 143' TERM; trap 'exit 130' INT; trap 'exit 129' HUP
verdict() { # verdict <boot log> <test>: prints PASS, FAIL: <msg>, or NONE
  awk -v t="$2" '
    /\[test\] [^ ]+ \.\.\./ {
      if (inblk) exit
      if (index($0, "[test] " t " ... ") > 0) {
        inblk = 1; rest = substr($0, index($0, " ... ") + 5)
        sub(/\r$/, "", rest)
        if (rest ~ /^(PASS|FAIL)/) { print rest; found = 1; exit }
        next
      }
    }
    inblk { sub(/\r$/, ""); if ($0 ~ /^(PASS|FAIL)/) { print; found = 1; exit } }
    END { if (!found) print "NONE" }
  ' "$1"
}
tally() { /usr/bin/grep -E '  tests: [0-9]+/[0-9]+' "$1" | tail -1 | sed -E 's/.*tests: ([0-9]+)\/([0-9]+).*/\1\/\2/'; }

echo "=== preconditions ==="
[ -x "$YIP" ] || { echo "REFUSING: no yip at $YIP"; exit 3; }
lease pre
[ "$(dirty)" = 0 ] || { echo "REFUSING: $(dirty) tracked modification(s); commit first"; exit 3; }
[ "$(git rev-parse HEAD)" = "$MERGE_PIN" ] || { echo "REFUSING: HEAD $(git rev-parse HEAD) is not $MERGE_PIN"; exit 3; }
[ "${GOFORK:-}" = "$GOFORK_PIN_DIR" ] || { echo "REFUSING: GOFORK must be $GOFORK_PIN_DIR (got '${GOFORK:-}')"; exit 3; }
export GOFORK
[ "$(git -C "$GOFORK" rev-parse HEAD 2>/dev/null)" = "$GOFORK_PIN" ] || { echo "REFUSING: GOFORK HEAD is not $GOFORK_PIN"; exit 3; }
[ -z "$(git -C "$GOFORK" status --porcelain)" ] || { echo "REFUSING: GOFORK $GOFORK has uncommitted changes"; exit 3; }
$MUTATE --check "$ROOT" | tee -a "$OUT/stages.txt"
for t in $NEW; do
  /usr/bin/grep -qF "{ \"$t\"," kernel/test/test.c || { echo "REFUSING: $t not registered"; exit 3; }
done
for m in $MUTANTS; do
  _w=$($MUTATE --list | awk -v m="$m" '$1 == m { $1 = ""; sub(/^ /, ""); print }')
  [ -n "$_w" ] || { echo "REFUSING: $m names no test"; exit 3; }
  for t in $_w; do
    case " $NEW " in *" $t "*) ;; *) echo "REFUSING: $m names $t, which is not one of NEW"; exit 3 ;; esac
  done
done
# The checker checks itself: a PASS block reads PASS, a FAIL block its message,
# an absent test NONE.
_t=$OUT/verdict-self-check.txt
printf '[test] a.x ... PASS\n[test] a.y ... \n  noise\nFAIL: the y message\n[test] a.z ... PASS\n' > "$_t"
[ "$(verdict "$_t" a.x)" = PASS ] && [ "$(verdict "$_t" a.y)" = "FAIL: the y message" ] && [ "$(verdict "$_t" a.q)" = NONE ] \
  || { echo "REFUSING: verdict() misreads its self-check"; exit 3; }
# The kill pattern checks itself: it is the witness's own text and matches it;
# a neighbouring message of the same test does not.
_m="and the resolved fd's ref was rolled back"
/usr/bin/grep -qF "\"$_m\"" kernel/test/test_loom.c || { echo "REFUSING: '$_m' is not in test_loom.c"; exit 3; }
printf 'FAIL: %s\n' "$_m" | /usr/bin/grep -qE "$KILL_FAIL" || { echo "REFUSING: KILL_FAIL misses '$_m'"; exit 3; }
_p="a listed descriptor naming nothing answers -EBADF"
/usr/bin/grep -qF "\"$_p\"" kernel/test/test_loom.c || { echo "REFUSING: '$_p' is not in test_loom.c"; exit 3; }
! printf 'FAIL: %s\n' "$_p" | /usr/bin/grep -qE "$KILL_FAIL" || { echo "REFUSING: KILL_FAIL matches '$_p'"; exit 3; }
record "HEAD $MERGE_PIN, clean; go fork $GOFORK_PIN, clean; anchors checked; verdict + kill-pattern self-checks pass; mutants: $MUTANTS"
[ -z "${DRYRUN:-}" ] || { record "DRYRUN: the preconditions hold; stopping before any build"; exit 0; }

survivors=""
for m in $MUTANTS; do
  echo "=== $m ==="
  lease "$m"
  floor "$m"
  want=$($MUTATE --list | awk -v m="$m" '$1 == m { $1 = ""; sub(/^ /, ""); print }')
  MUTATED=1
  $MUTATE "$ROOT" "$m" > "$OUT/$m-mutate.txt"
  git diff > "$OUT/$m.diff"
  [ -s "$OUT/$m.diff" ] || { echo "$m changed nothing"; exit 1; }
  if ! tools/build.sh kernel --config ci > "$OUT/$m-build.log" 2>&1; then
    restore
    record "$m: BUILD FAILED (proves nothing); tail in $OUT/$m-build.log"
    survivors="$survivors $m(build)"
    continue
  fi
  rm -f build/test-boot.log
  ( set +e; tools/test.sh > "$OUT/$m-test.log" 2>&1; echo $? > "$OUT/$m.rc" )
  restore
  B=$OUT/$m-boot.log
  [ -f build/test-boot.log ] || { record "$m: no build/test-boot.log"; survivors="$survivors $m(noboot)"; continue; }
  cp build/test-boot.log "$B"
  ext=$(tr -d '\r' < "$B" | /usr/bin/grep -m1 '^EXTINCTION:' || true)
  killed=1
  for t in $want; do
    v=$(verdict "$B" "$t")
    record "$m: $t -> $v"
    printf '%s\n' "$v" | /usr/bin/grep -qE "$KILL_FAIL" || { record "$m: $t did not fail at the witness: proves nothing"; killed=0; }
  done
  record "$m: tally $(tally "$B"); test.sh exit $(cat "$OUT/$m.rc"); ${ext:-no extinction}"
  if [ "$killed" = 1 ]; then record "$m: KILLED"; else record "$m: SURVIVED"; survivors="$survivors $m"; fi
done

[ "$(git rev-parse HEAD)" = "$MERGE_PIN" ] && [ "$(dirty)" = 0 ] || { echo "HEAD moved or the tree changed during the run"; exit 8; }
[ "$(git -C "$GOFORK" rev-parse HEAD)" = "$GOFORK_PIN" ] && [ -z "$(git -C "$GOFORK" status --porcelain)" ] \
  || { echo "GOFORK moved or changed during the run"; exit 8; }
if [ -n "$survivors" ]; then record "SURVIVORS:$survivors"; exit 1; fi
record "ALL KILLED: $MUTANTS (build/ holds the last mutant's kernel -- run gate.sh next)"
