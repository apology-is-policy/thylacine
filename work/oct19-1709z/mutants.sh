#!/bin/sh
# Sabotage runs for 17:09Z (the ctl identity). Each mutant (mutate.py) is applied
# to the committed tree, built (ci image), booted through the whole suite, and
# reverted. A mutant is KILLED when every test it names reports FAIL (or the
# boot extincts inside one of them); the FAIL text is recorded so the assertion
# that fired can be read, not just the verdict. Survivors make the run exit 1.
# build/ is left holding the LAST mutant's kernel: run gate.sh afterwards.
# Run from the checkout root (no cd): sh work/oct19-1709z/mutants.sh
set -e
ROOT=/Users/northkillpd/projects/thylacine-corona
[ "$(pwd)" = "$ROOT" ] || { echo "REFUSING: run from $ROOT, not $(pwd)"; exit 3; }
STAMP=$(date -u '+%Y%m%dT%H%M%SZ')
OUT=work/oct19-1709z/mutants-$STAMP
mkdir -p "$OUT"
YIP="${THYLA_WAKE_YIP:-$(command -v yip || echo "$HOME/.local/bin/yip")}"
CORONA_PIN=c6a3dca72edc9d255ef96d051b282f66e2dbfdf0
export STRATUM_SRC=/Users/northkillpd/projects/stratum/v2
GOFORK_PIN_DIR=/Users/northkillpd/projects/go-thylacine-pin-a6567ba
GOFORK_PIN=a6567baeaa716811783031fdf543477fdcd1707d
NEW="vivsock.ctl_verbs_need_their_ctl vivsock.ctl_verbs_meet_a_racing_peer vivsock.ctl_identity_travels_with_the_fd vivsock.connect_finished_by_a_peer vivsock.connect_commits_a_realiased_row"
MUTANTS=${MUTANTS:-"M1 M2a M2b M2c M3 M4 M5 M7 M8 M9 M10 M11"}
MUTATE="python3 -I work/oct19-1709z/mutate.py"
record() { echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ')  $*" | tee -a "$OUT/stages.txt"; }
lease() {
  "$YIP" resources > "$OUT/lease-$1.txt" 2>&1 || true
  /usr/bin/grep -qE '^mac +HELD by you' "$OUT/lease-$1.txt" || {
    echo "STOPPING before $1: the mac lease is not mine"; exit 6; }
}
dirty() { git status --porcelain --untracked-files=no | wc -l | tr -d ' '; }
MUTATED=0
restore() {
  git checkout -- kernel/syscall.c kernel/vivarium.c
  MUTATED=0
  [ "$(dirty)" = 0 ] || { echo "RESTORE left $(dirty) tracked change(s)"; exit 8; }
}
trap '_rc=$?; [ "$MUTATED" = 0 ] || restore; record "exit $_rc"' EXIT
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
last_test() { tr -d '\r' < "$1" | /usr/bin/grep -oE '\[test\] [^ ]+ \.\.\.' | tail -1 | sed -E 's/^\[test\] ([^ ]+) \.\.\./\1/'; }
tally() { /usr/bin/grep -E '  tests: [0-9]+/[0-9]+' "$1" | tail -1 | sed -E 's/.*tests: ([0-9]+)\/([0-9]+).*/\1\/\2/'; }

echo "=== preconditions ==="
[ -x "$YIP" ] || { echo "REFUSING: no yip at $YIP"; exit 3; }
lease pre
[ "$(dirty)" = 0 ] || { echo "REFUSING: $(dirty) tracked modification(s); commit first"; exit 3; }
[ "$(git rev-parse HEAD)" = "$CORONA_PIN" ] || { echo "REFUSING: HEAD $(git rev-parse HEAD) is not $CORONA_PIN"; exit 3; }
[ "${GOFORK:-}" = "$GOFORK_PIN_DIR" ] || { echo "REFUSING: GOFORK must be $GOFORK_PIN_DIR (got '${GOFORK:-}')"; exit 3; }
export GOFORK
[ "$(git -C "$GOFORK" rev-parse HEAD 2>/dev/null)" = "$GOFORK_PIN" ] || { echo "REFUSING: GOFORK HEAD is not $GOFORK_PIN"; exit 3; }
$MUTATE --check "$ROOT" | tee -a "$OUT/stages.txt"
for t in $NEW; do
  /usr/bin/grep -qF "{ \"$t\"," kernel/test/test.c || { echo "REFUSING: $t not registered"; exit 3; }
done
# The checker checks itself: a PASS block reads PASS, a FAIL block its message,
# an absent test NONE.
_t=$OUT/verdict-self-check.txt
printf '[test] a.x ... PASS\n[test] a.y ... \n  noise\nFAIL: the y message\n[test] a.z ... PASS\n' > "$_t"
[ "$(verdict "$_t" a.x)" = PASS ] && [ "$(verdict "$_t" a.y)" = "FAIL: the y message" ] && [ "$(verdict "$_t" a.q)" = NONE ] \
  || { echo "REFUSING: verdict() misreads its self-check"; exit 3; }
record "HEAD $CORONA_PIN, clean; anchors checked; verdict self-check passes; mutants: $MUTANTS"

survivors=""
for m in $MUTANTS; do
  echo "=== $m ==="
  lease "$m"
  want=$($MUTATE --list | awk -v m="$m" '$1 == m { $1 = ""; sub(/^ /, ""); print }')
  [ -n "$want" ] || { echo "REFUSING: $m names no test"; exit 3; }
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
    case "$v" in
      FAIL*) ;;
      *) if [ -n "$ext" ] && [ "$(last_test "$B")" = "$t" ]; then
           record "$m: $t extincted: $ext"
         else
           killed=0
         fi ;;
    esac
  done
  for t in $NEW; do
    case " $want " in *" $t "*) continue ;; esac
    record "$m: (also) $t -> $(verdict "$B" "$t")"
  done
  record "$m: tally $(tally "$B"); test.sh exit $(cat "$OUT/$m.rc"); ${ext:-no extinction}"
  if [ "$killed" = 1 ]; then record "$m: KILLED"; else record "$m: SURVIVED"; survivors="$survivors $m"; fi
done

[ "$(git rev-parse HEAD)" = "$CORONA_PIN" ] && [ "$(dirty)" = 0 ] || { echo "corona HEAD moved or the tree changed during the run"; exit 8; }
if [ -n "$survivors" ]; then record "SURVIVORS:$survivors"; exit 1; fi
record "ALL KILLED: $MUTANTS (build/ holds the last mutant's kernel -- run gate.sh next)"
