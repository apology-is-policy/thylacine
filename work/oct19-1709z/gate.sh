#!/bin/sh
# Push gate for 17:09Z (the ctl identity, CORONA_PIN below) against canonical Stratum main at 7dd103d
# (operator decision 2026-10-19: Astra's three commits fast-forward onto Stratum
# main; no stratum-astra pin). Derived from work/oct15-na4d/kernel-run.sh.
#   CONTROL    ci image: the whole suite; the four 17:09Z vivsock tests, 13:05Z's narrowing witness and
#              the NA-4d loom tests PASS with NA-4d's verdicts (fixture 76,
#              probes 78/74, no ring, no connection); joey's D7 overlapping
#              login PASS -- the leg that extincted against ac519fc.
#   CONTROL-Q  qualification image: the same, fixture 77, probes 79/75,
#              two rings and six connections.
#   UBSAN      the ci image's kernel in UBSan trapping mode (undefined behaviour
#              is a BRK, so an extinction): the control's verdicts, again.
#   PRODUCTION tools/check-production.sh --all (the KERNEL_TESTS=OFF kernel builds).
#   HOST       not run: no host code changed since 2a0c60b62 (17:09Z is kernel only).
#   Not proven here: SMP (main's land runs tools/ci-smp-gate.sh N=10).
# Run from the checkout root (no cd): sh work/oct19-1709z/gate.sh
set -e
ROOT=/Users/northkillpd/projects/thylacine-corona
[ "$(pwd)" = "$ROOT" ] || { echo "REFUSING: run from $ROOT, not $(pwd)"; exit 3; }
STAMP=$(date -u '+%Y%m%dT%H%M%SZ')
OUT=work/oct19-1709z/gate-$STAMP
mkdir -p "$OUT"
YIP="${THYLA_WAKE_YIP:-$(command -v yip || echo "$HOME/.local/bin/yip")}"
FLOOR_MB=${FLOOR_MB:-8192}
FX_BIN=build/ramfs-src/bin/loom-fixture
RP_BIN=build/ramfs-src/bin/loom-native-probe
CP_BIN=build/ramfs-src/bin/loom-c-probe
BR_BIN=build/ramfs-src/bin/loom-bridge-probe
QDIR=build/kernel-private-async-qual
QUAL_LINE='PRIVATE-ASYNC QUALIFICATION build'
CORONA_PIN=c6a3dca72edc9d255ef96d051b282f66e2dbfdf0
STRATUM_PIN=7dd103ddbccabe95dfe1d36fdddb4d6a1bfd99dc
export STRATUM_SRC=/Users/northkillpd/projects/stratum/v2
# Two stale tracked edits (2026-07-11) that no build reads; any other refuses.
STRATUM_DIRTY_OK='v2/.audit_r172_findings.md v2/specs/compound_ops_per_inode.tla'
STRATUMD_LINE="==> building stratumd (aarch64-thylacine) from $STRATUM_SRC"
D7_LINE='joey: D7 overlapping login probe PASS'
GOFORK_PIN_DIR=/Users/northkillpd/projects/go-thylacine-pin-a6567ba
GOFORK_PIN=a6567baeaa716811783031fdf543477fdcd1707d
NAMED="loom.native_probe_rust loom.native_probe_c loom.bridge_probe vivsock.sendto_recvfrom_narrow_fd vivsock.ctl_verbs_need_their_ctl vivsock.ctl_verbs_meet_a_racing_peer vivsock.ctl_identity_travels_with_the_fd vivsock.connect_finished_by_a_peer vivsock.connect_commits_a_realiased_row"
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
trap 'record "exit $?"' EXIT
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
fails() { /usr/bin/grep -E '^FAIL:|\[test\] [^ ]+ \.\.\. FAIL:' "$1" || true; }
tally() { /usr/bin/grep -E '  tests: [0-9]+/[0-9]+' "$1" | tail -1 | sed -E 's/.*tests: ([0-9]+)\/([0-9]+).*/\1 \2/'; }
run_suite() { # run_suite <label> [test.sh args]
  _l=$1; shift
  rm -f build/test-boot.log
  ( set +e; tools/test.sh "$@" > "$OUT/$_l-test.log" 2>&1; echo $? > "$OUT/$_l.rc" )
  suite_rc=$(cat "$OUT/$_l.rc")
  [ -f build/test-boot.log ] || { echo "!! no build/test-boot.log after $_l"; exit 1; }
  cp build/test-boot.log "$OUT/$_l-boot.log"
  record "$_l: test.sh exit $suite_rc"
}
# Not "times": sh finds that special builtin before any function, so a
# function of that name never runs (the self-check below proves this one does).
occurs() { # occurs <file> <count> <fixed text>: the text occurs exactly that often
  _c=$(/usr/bin/grep -cF "$3" "$1" || true)
  [ "$_c" = "$2" ] || { echo "'$3' occurs $_c times in $1, want $2"; /usr/bin/grep -nE 'loom-(fixture|native-probe|c-probe)|status|rings|connections|registries|D7' "$1" | head -40; exit 1; }
}
probes_hash() { cat "$FX_BIN" "$RP_BIN" "$CP_BIN" "$BR_BIN" | shasum -a 256 | cut -d' ' -f1; }
all_pass() {
  for t in $NAMED; do
    v=$(verdict "$2" "$t")
    [ "$v" = PASS ] || { echo "$1: $t verdict '$v'"; exit 1; }
  done
}
image() { # image <label> <boot log> <fixture> <rust> <c> <rings> <connections>
  B=$2
  /usr/bin/grep -q '^EXTINCTION:' "$B" && { echo "$1 EXTINCTED"; /usr/bin/grep -E '^EXTINCTION:|D7' "$B" | head; fails "$B" | head; exit 1; } || true
  set -- "$1" "$2" "$3" "$4" "$5" "$6" "$7" $(tally "$B")
  [ "${8:-}" = "$EXPECT" ] && [ "${9:-}" = "$EXPECT" ] || { echo "$1 tally '${8:-} ${9:-}' != $EXPECT/$EXPECT"; fails "$B" | head; exit 1; }
  [ "$suite_rc" = 0 ] || { echo "$1 test.sh exit $suite_rc"; exit 1; }
  all_pass "$1" "$B"
  occurs "$B" 1 "$D7_LINE"
  occurs "$B" 2 "  fixture status $3 expected $3"
  occurs "$B" 1 "  probe status $4 expected $4"
  occurs "$B" 1 "  probe status $5 expected $5"
  occurs "$B" 2 "  rings made $6 expected $6"
  occurs "$B" 2 "  rings retired $6 expected $6"
  occurs "$B" 2 "  connections made $7 expected $7"
  occurs "$B" 2 "  connections freed $7 expected $7"
  occurs "$B" 2 "  registries destroyed 1 expected 1"
  occurs "$B" 2 "loom-fixture: sessions $7 ended $7 violations 0 "
}
measured() { # the kernel test's report lines and the fixture's, on one line
  tr -d '\r' < "$1" | /usr/bin/grep -E '^ +(loom-native-probe|loom-c-probe):$|^ +(fixture status|probe status|rings made|rings retired|connections made|connections freed|registries destroyed) |loom-fixture: sessions ' | sed -E 's/^ +//' | tr '\n' ';'
}
stratum_dirty() { git -C "$STRATUM_SRC" status --porcelain --untracked-files=no | sed 's/^...//' | LC_ALL=C sort | tr '\n' ' ' | sed 's/ $//'; }
stratum_inputs() { git -C "$STRATUM_SRC" status --porcelain --untracked-files=all | /usr/bin/grep '^??' | sed 's/^?? //' | /usr/bin/grep -E '\.(c|h|cc|cpp|S|s|cmake)$|CMakeLists\.txt$' || true; }
stratum_head_ok() {
  _h=$(git -C "$STRATUM_SRC" rev-parse HEAD)
  [ "$_h" = "$STRATUM_PIN" ] || { echo "REFUSING at $1: Stratum HEAD $_h is not $STRATUM_PIN"; exit 3; }
}
stratum_tree_ok() {
  [ "$(stratum_dirty)" = "$STRATUM_DIRTY_OK" ] || { echo "REFUSING at $1: Stratum tracked edits '$(stratum_dirty)' are not '$STRATUM_DIRTY_OK'"; exit 3; }
  [ -z "$(stratum_inputs)" ] || { echo "REFUSING at $1: untracked Stratum build inputs: $(stratum_inputs)"; exit 3; }
}
cache_ok() { # cache_ok <CMakeCache.txt>: configured against the canonical tree
  _h=$(/usr/bin/grep -m1 '^CMAKE_HOME_DIRECTORY' "$1" | cut -d= -f2)
  [ "$_h" = "$STRATUM_SRC" ] || { echo "$1 is configured against '$_h', not $STRATUM_SRC"; exit 1; }
}
built_from_canonical() { # built_from_canonical <label> <build log>
  occurs "$2" 1 "$STRATUMD_LINE"
  /usr/bin/grep -q 'stratum-astra' "$2" && { echo "$1: the build log names stratum-astra"; /usr/bin/grep -n 'stratum-astra' "$2" | head; exit 1; } || true
  cache_ok build/host-stratum/CMakeCache.txt
  cache_ok build/pouch/stratumd-cmake/CMakeCache.txt
}

echo "=== preconditions ==="
_t=$OUT/occurs-self-check.txt
printf 'x\nx\n' > "$_t"
( occurs "$_t" 2 x ) > /dev/null || { echo "REFUSING: occurs refuses a true count"; exit 3; }
( occurs "$_t" 1 x ) > /dev/null && { echo "REFUSING: occurs accepts a false count"; exit 3; } || true
record "occurs self-check: a true count passes, a false one fails"
[ -x "$YIP" ] || { echo "REFUSING: no yip at $YIP"; exit 3; }
lease pre
dirty=$(git status --porcelain --untracked-files=no | wc -l | tr -d ' ')
[ "$dirty" = 0 ] || { echo "REFUSING: $dirty tracked modification(s); commit first"; exit 3; }
[ "$(git rev-parse HEAD)" = "$CORONA_PIN" ] || { echo "REFUSING: HEAD $(git rev-parse HEAD) is not $CORONA_PIN"; exit 3; }
record "HEAD $CORONA_PIN, clean"
for t in $NAMED; do
  /usr/bin/grep -qF "{ \"$t\"," kernel/test/test.c || { echo "REFUSING: $t not registered"; exit 3; }
done
EXPECT=$(/usr/bin/grep -c -E '^[[:space:]]*\{[[:space:]]*"[^"]+"' kernel/test/test.c)
[ "$EXPECT" -ge 1000 ] || { echo "REFUSING: derived only $EXPECT registrations"; exit 3; }
record "expected tests (derived from test.c): $EXPECT"
stratum_head_ok pre
( STRATUM_DIRTY_OK='v2/.audit_r172_findings.md'; stratum_tree_ok self-check ) > /dev/null && { echo "REFUSING: stratum_tree_ok accepts a wrong edit set"; exit 3; } || true
stratum_tree_ok pre
record "stratum $STRATUM_SRC at $STRATUM_PIN; tracked edits exactly '$STRATUM_DIRTY_OK' (self-check: a wrong set refuses); no untracked build input"
[ "${GOFORK:-}" = "$GOFORK_PIN_DIR" ] || { echo "REFUSING: GOFORK must be $GOFORK_PIN_DIR (got '${GOFORK:-}')"; exit 3; }
export GOFORK
[ "$(git -C "$GOFORK" rev-parse HEAD 2>/dev/null)" = "$GOFORK_PIN" ] || { echo "REFUSING: GOFORK HEAD is not $GOFORK_PIN"; exit 3; }
[ -z "$(git -C "$GOFORK" status --porcelain)" ] || { echo "REFUSING: GOFORK $GOFORK has uncommitted changes"; exit 3; }
record "go fork $GOFORK at $GOFORK_PIN, clean"
[ -z "${DRYRUN:-}" ] || { record "DRYRUN: the preconditions hold; stopping before any build"; exit 0; }
floor pre

echo "=== CONTROL ==="
lease control
tools/build.sh kernel --config ci > "$OUT/control-build.log" 2>&1 || { echo "control build failed"; tail -20 "$OUT/control-build.log"; exit 1; }
floor post-control-build
built_from_canonical CONTROL "$OUT/control-build.log"
stratum_head_ok post-control-build; stratum_tree_ok post-control-build
for f in "$FX_BIN" "$RP_BIN" "$CP_BIN"; do [ -f "$f" ] || { echo "CONTROL: $f was not built"; exit 1; }; done
CONTROL_BIN=$(shasum -a 256 build/kernel/thylacine.bin | cut -d' ' -f1)
CONTROL_PROBES=$(probes_hash)
record "control kernel $CONTROL_BIN; programs $CONTROL_PROBES"
find build/ramfs-src build/pouch -name stratumd -type f ! -path '*stale*' ! -path '*pre-d7*' -exec shasum -a 256 {} \; | tee -a "$OUT/stages.txt"
run_suite control
/usr/bin/grep -qF "$QUAL_LINE" "$OUT/control-build.log" && { echo "CONTROL: the default build saw the lever"; exit 1; } || true
image control "$OUT/control-boot.log" 76 78 74 0 0
occurs "$OUT/control-boot.log" 1 "loom-native-probe: default image: setup refuses -EOPNOTSUPP"
occurs "$OUT/control-boot.log" 1 "loom-c-probe: default image: setup refuses -EOPNOTSUPP"
record "CONTROL GREEN: $EXPECT/$EXPECT, D7 PASS; measured: $(measured "$OUT/control-boot.log")"

echo "=== CONTROL-Q ==="
lease control-q
tools/build.sh kernel --config ci --set PRIVATE_ASYNC_QUAL=y > "$OUT/control-q-build.log" 2>&1 || { echo "control-q build failed"; tail -20 "$OUT/control-q-build.log"; exit 1; }
/usr/bin/grep -qF "$QUAL_LINE" "$OUT/control-q-build.log" || { echo "CONTROL-Q: cmake never saw the lever"; exit 1; }
floor post-control-q-build
built_from_canonical CONTROL-Q "$OUT/control-q-build.log"
stratum_head_ok post-control-q-build; stratum_tree_ok post-control-q-build
Q_BIN=$(shasum -a 256 "$QDIR/thylacine.bin" | cut -d' ' -f1)
record "control-q kernel $Q_BIN"
[ "$(probes_hash)" = "$CONTROL_PROBES" ] || { echo "CONTROL-Q: the programs differ from the control's"; exit 1; }
[ "$Q_BIN" != "$CONTROL_BIN" ] || { echo "CONTROL-Q: the qualification kernel equals the default one"; exit 1; }
run_suite control-q --private-async-qual
image control-q "$OUT/control-q-boot.log" 77 79 75 2 6
occurs "$OUT/control-q-boot.log" 1 "loom-native-probe: qualification image drives every leg through the Rust adapter"
occurs "$OUT/control-q-boot.log" 1 "loom-c-probe: qualification image drives every leg through the C adapter"
record "CONTROL-Q GREEN: $EXPECT/$EXPECT, D7 PASS; measured: $(measured "$OUT/control-q-boot.log")"

echo "=== UBSAN ==="
lease ubsan
tools/build.sh kernel --config ci --sanitize=ubsan > "$OUT/ubsan-build.log" 2>&1 || { echo "ubsan build failed"; tail -20 "$OUT/ubsan-build.log"; exit 1; }
/usr/bin/grep -qF "sanitize='undefined'" "$OUT/ubsan-build.log" || { echo "UBSAN: the build never selected the sanitizer"; exit 1; }
floor post-ubsan-build
built_from_canonical UBSAN "$OUT/ubsan-build.log"
stratum_head_ok post-ubsan-build; stratum_tree_ok post-ubsan-build
U_BIN=$(shasum -a 256 build/kernel-undefined/thylacine.bin | cut -d' ' -f1)
record "ubsan kernel $U_BIN"
[ "$(probes_hash)" = "$CONTROL_PROBES" ] || { echo "UBSAN: the programs differ from the control's"; exit 1; }
[ "$U_BIN" != "$CONTROL_BIN" ] || { echo "UBSAN: the sanitized kernel equals the default one"; exit 1; }
run_suite ubsan --sanitize=ubsan
image ubsan "$OUT/ubsan-boot.log" 76 78 74 0 0
record "UBSAN GREEN: $EXPECT/$EXPECT, D7 PASS, no trap; measured: $(measured "$OUT/ubsan-boot.log")"

echo "=== PRODUCTION ==="
lease production
# The KERNEL_TESTS=OFF kernel: the pin hook and the test drivers must stay out
# of it without breaking it.
tools/check-production.sh --all > "$OUT/production.log" 2>&1 || { echo "check-production --all failed"; tail -20 "$OUT/production.log"; exit 1; }
record "PRODUCTION: tools/check-production.sh --all passes"

[ "$(git -C "$GOFORK" rev-parse HEAD)" = "$GOFORK_PIN" ] && [ -z "$(git -C "$GOFORK" status --porcelain)" ] \
  || { echo "GOFORK moved or changed during the run"; exit 8; }
stratum_head_ok end; stratum_tree_ok end
[ "$(git rev-parse HEAD)" = "$CORONA_PIN" ] && [ "$(git status --porcelain --untracked-files=no | wc -l | tr -d ' ')" = 0 ] \
  || { echo "corona HEAD moved or the tree changed during the run"; exit 8; }
record "ALL GREEN: corona $CORONA_PIN, stratum $STRATUM_PIN, go fork $GOFORK_PIN"
