#!/bin/sh
# Gate for the main merge (corona/merge-main: main a0128885b into corona
# 23b0b0e5c), at MERGE_PIN, against canonical Stratum main at 7dd103d, built
# with the Go fork at 3471e2a: the merged kernel has main's WAIT_PACKED, which
# that fork's Wait reads, and not b3's SYS_UMASK/SYS_GETPPID, which the shared
# fork's later f67e51e calls. Derived from work/oct21-mag/gate.sh; the header,
# OUT, the pins and NAMED differ, and no test of the three suite images may
# print [skip] (a missing probe binary passes otherwise).
#   NAMED      derived by work/oct22-merge/named.py, never listed by hand: vote
#              16's NAMED, every test registered on only one parent, and every
#              test defined in a test file the merge left unlike both parents.
#   CONTROL    ci image: the whole suite; NAMED PASS with NA-4d's verdicts
#              (fixture 76, probes 78/74, no ring, no connection); joey's D7
#              overlapping login PASS; no [skip] line.
#   CONTROL-Q  qualification image: the same, fixture 77, probes 79/75, two
#              rings and six connections.
#   UBSAN      the ci image's kernel in UBSan trapping mode: the control's
#              verdicts, again.
#   PRODUCTION tools/check-production.sh --all (the KERNEL_TESTS=OFF kernels).
#   SMP        tools/ci-smp-gate.sh, the full matrix (default-smp1/4/8,
#              ubsan-smp4/8) at N=10, with its disk floor at build.sh's 6 GiB.
#   HOST       not run here: work/oct22-merge/host.sh (every tools/test-*.py,
#              the loom-client suite, the Rust host tests, the sims mutants,
#              the burrow spec cfgs) runs before this, and mutants.sh too (R1:
#              the restored ref-rollback witness; R2: audit F1's reap before
#              the private executor's charge, in the qualification image).
#   Not proven here: the A72/KVM axis.
# DRYRUN=1 stops after the preconditions (no lease needed, nothing built).
# Run from the checkout root (no cd): MERGE_PIN=<full sha> sh work/oct22-merge/gate.sh
set -e
ROOT=/Users/northkillpd/projects/thylacine-corona
[ "$(pwd)" = "$ROOT" ] || { echo "REFUSING: run from $ROOT, not $(pwd)"; exit 3; }
STAMP=$(date -u '+%Y%m%dT%H%M%SZ')
OUT=work/oct22-merge/gate-$STAMP
mkdir -p "$OUT"
YIP="${THYLA_WAKE_YIP:-$(command -v yip || echo "$HOME/.local/bin/yip")}"
FLOOR_MB=${FLOOR_MB:-8192}
SMP_FLOOR_MB=6144
FX_BIN=build/ramfs-src/bin/loom-fixture
RP_BIN=build/ramfs-src/bin/loom-native-probe
CP_BIN=build/ramfs-src/bin/loom-c-probe
BR_BIN=build/ramfs-src/bin/loom-bridge-probe
QDIR=build/kernel-private-async-qual
QUAL_LINE='PRIVATE-ASYNC QUALIFICATION build'
MERGE_PIN=${MERGE_PIN:?set MERGE_PIN to the full sha under test}
STRATUM_PIN=7dd103ddbccabe95dfe1d36fdddb4d6a1bfd99dc
export STRATUM_SRC=/Users/northkillpd/projects/stratum/v2
# Two stale tracked edits (2026-07-11) that no build reads; any other refuses.
STRATUM_DIRTY_OK='v2/.audit_r172_findings.md v2/specs/compound_ops_per_inode.tla'
STRATUMD_LINE="==> building stratumd (aarch64-thylacine) from $STRATUM_SRC"
D7_LINE='joey: D7 overlapping login probe PASS'
GOFORK_PIN_DIR=/Users/northkillpd/projects/go-thylacine-pin-3471e2a
GOFORK_PIN=3471e2ac027de4a5cbbee9337a3f335b96156e29
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
# The status first: anything run before it would replace $?. A signal death
# never reaches $? (sh runs the EXIT trap with the last completed command's
# status: work/oct19-asr9's killed SMP run recorded "exit 0"), so each signal
# exits with its own status.
trap 'rc=$?; record "exit $rc"' EXIT
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
smp_row_clean() { # smp_row_clean <smp log> <label>: the row's own summary, N=10, every boot a PASS
  occurs "$1" 1 "== $2: 10 PASS / 0 CORRUPTION / 0 external-kill / 0 inject-miss / 0 timing / 0 other  (N=10) =="
}
probes_hash() { # probes_hash <file>...: one hash over the guest programs; a missing or empty one fails
  for _f in "$@"; do [ -s "$_f" ] || { echo "probes_hash: $_f is missing or empty" >&2; return 1; }; done
  cat "$@" | shasum -a 256 | cut -d' ' -f1
}
PROBES="$FX_BIN $RP_BIN $CP_BIN $BR_BIN"
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
  # A test whose probe binary is missing from the ramfs prints [skip] and
  # returns, and the suite counts it as a pass; the gate images carry every
  # probe (0 such lines in each of vote 16's three gate boots).
  occurs "$B" 0 "[skip]"
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
_t=$OUT/verdict-self-check.txt
printf '[test] a.x ... PASS\n[test] a.y ... \n  noise\nFAIL: the y message\n[test] a.z ... PASS\n' > "$_t"
[ "$(verdict "$_t" a.x)" = PASS ] && [ "$(verdict "$_t" a.y)" = "FAIL: the y message" ] && [ "$(verdict "$_t" a.q)" = NONE ] \
  || { echo "REFUSING: verdict() misreads its self-check"; exit 3; }
record "verdict self-check: PASS, a FAIL's message and an absent test read as such"
# a59667812 glued `cat"$FX_BIN"`: every hash was the empty input's and the
# program comparisons could not fail. Same files, same hash; one byte, not.
printf a > "$OUT/probes-a"; printf b > "$OUT/probes-b"
_same=$(probes_hash "$OUT/probes-a" "$OUT/probes-a") && _diff=$(probes_hash "$OUT/probes-a" "$OUT/probes-b") \
  && [ "$_same" = "$(probes_hash "$OUT/probes-a" "$OUT/probes-a")" ] && [ "$_same" != "$_diff" ] \
  && [ "$_same" != e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 ] \
  || { echo "REFUSING: probes_hash does not tell the programs apart"; exit 3; }
( probes_hash "$OUT/probes-a" "$OUT/probes-none" ) > /dev/null 2>&1 && { echo "REFUSING: probes_hash accepts a missing program"; exit 3; } || true
record "probes_hash self-check: equal files agree, a changed byte differs, a missing program refuses"
[ -x "$YIP" ] || { echo "REFUSING: no yip at $YIP"; exit 3; }
lease pre
dirty=$(git status --porcelain --untracked-files=no | wc -l | tr -d ' ')
[ "$dirty" = 0 ] || { echo "REFUSING: $dirty tracked modification(s); commit first"; exit 3; }
[ "$(git rev-parse HEAD)" = "$MERGE_PIN" ] || { echo "REFUSING: HEAD $(git rev-parse HEAD) is not $MERGE_PIN"; exit 3; }
record "HEAD $MERGE_PIN, clean"
NAMED=$(python3 -I work/oct22-merge/named.py 2> "$OUT/named-counts.txt") || { cat "$OUT/named-counts.txt"; echo "REFUSING: named.py failed"; exit 3; }
echo "$NAMED" > "$OUT/named.txt"
record "NAMED derived: $(cat "$OUT/named-counts.txt")"
for t in $NAMED; do
  /usr/bin/grep -qF "{ \"$t\"," kernel/test/test.c || { echo "REFUSING: $t not registered"; exit 3; }
done
EXPECT=$(/usr/bin/grep -c -E '^[[:space:]]*\{[[:space:]]*"[^"]+"' kernel/test/test.c)
[ "$EXPECT" -ge 1000 ] || { echo "REFUSING: derived only $EXPECT registrations"; exit 3; }
record "expected tests (derived from test.c): $EXPECT; named: $(echo $NAMED | wc -w | tr -d ' ')"
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
CONTROL_PROBES=$(probes_hash $PROBES)
record "control kernel $CONTROL_BIN; programs $CONTROL_PROBES"
find build/ramfs-src build/pouch -name stratumd -type f ! -path '*stale*' ! -path '*pre-d7*' -exec shasum -a 256 {} \; | tee -a "$OUT/stages.txt"
run_suite control
/usr/bin/grep -qF "$QUAL_LINE" "$OUT/control-build.log" && { echo "CONTROL: the default build saw the lever"; exit 1; } || true
image control "$OUT/control-boot.log" 76 78 74 0 0
occurs "$OUT/control-boot.log" 1 "loom-native-probe: default image: setup refuses -EOPNOTSUPP"
occurs "$OUT/control-boot.log" 1 "loom-c-probe: default image: setup refuses -EOPNOTSUPP"
record "CONTROL GREEN: $EXPECT/$EXPECT, NAMED PASS, D7 PASS; measured: $(measured "$OUT/control-boot.log")"

echo "=== CONTROL-Q ==="
lease control-q
tools/build.sh kernel --config ci --set PRIVATE_ASYNC_QUAL=y > "$OUT/control-q-build.log" 2>&1 || { echo "control-q build failed"; tail -20 "$OUT/control-q-build.log"; exit 1; }
/usr/bin/grep -qF "$QUAL_LINE" "$OUT/control-q-build.log" || { echo "CONTROL-Q: cmake never saw the lever"; exit 1; }
floor post-control-q-build
built_from_canonical CONTROL-Q "$OUT/control-q-build.log"
stratum_head_ok post-control-q-build; stratum_tree_ok post-control-q-build
Q_BIN=$(shasum -a 256 "$QDIR/thylacine.bin" | cut -d' ' -f1)
record "control-q kernel $Q_BIN"
[ "$(probes_hash $PROBES)" = "$CONTROL_PROBES" ] || { echo "CONTROL-Q: the programs differ from the control's"; exit 1; }
[ "$Q_BIN" != "$CONTROL_BIN" ] || { echo "CONTROL-Q: the qualification kernel equals the default one"; exit 1; }
run_suite control-q --private-async-qual
image control-q "$OUT/control-q-boot.log" 77 79 75 2 6
occurs "$OUT/control-q-boot.log" 1 "loom-native-probe: qualification image drives every leg through the Rust adapter"
occurs "$OUT/control-q-boot.log" 1 "loom-c-probe: qualification image drives every leg through the C adapter"
record "CONTROL-Q GREEN: $EXPECT/$EXPECT, NAMED PASS, D7 PASS; measured: $(measured "$OUT/control-q-boot.log")"

echo "=== UBSAN ==="
lease ubsan
tools/build.sh kernel --config ci --sanitize=ubsan > "$OUT/ubsan-build.log" 2>&1 || { echo "ubsan build failed"; tail -20 "$OUT/ubsan-build.log"; exit 1; }
/usr/bin/grep -qF "sanitize='undefined'" "$OUT/ubsan-build.log" || { echo "UBSAN: the build never selected the sanitizer"; exit 1; }
floor post-ubsan-build
built_from_canonical UBSAN "$OUT/ubsan-build.log"
stratum_head_ok post-ubsan-build; stratum_tree_ok post-ubsan-build
U_BIN=$(shasum -a 256 build/kernel-undefined/thylacine.bin | cut -d' ' -f1)
record "ubsan kernel $U_BIN"
[ "$(probes_hash $PROBES)" = "$CONTROL_PROBES" ] || { echo "UBSAN: the programs differ from the control's"; exit 1; }
[ "$U_BIN" != "$CONTROL_BIN" ] || { echo "UBSAN: the sanitized kernel equals the default one"; exit 1; }
run_suite ubsan --sanitize=ubsan
image ubsan "$OUT/ubsan-boot.log" 76 78 74 0 0
record "UBSAN GREEN: $EXPECT/$EXPECT, NAMED PASS, D7 PASS, no trap; measured: $(measured "$OUT/ubsan-boot.log")"

echo "=== PRODUCTION ==="
lease production
# The KERNEL_TESTS=OFF kernel: the pin hook and the test drivers must stay out
# of it without breaking it.
tools/check-production.sh --all > "$OUT/production.log" 2>&1 || { echo "check-production --all failed"; tail -20 "$OUT/production.log"; exit 1; }
record "PRODUCTION: tools/check-production.sh --all passes"

echo "=== SMP ==="
lease smp
floor pre-smp
( set +e; SMP_GATE_MIN_FREE_MIB=$SMP_FLOOR_MB tools/ci-smp-gate.sh > "$OUT/smp.log" 2>&1; echo $? > "$OUT/smp.rc" )
_s=$(cat "$OUT/smp.rc")
[ "$_s" = 0 ] || { echo "ci-smp-gate.sh exit $_s"; tail -40 "$OUT/smp.log"; exit 1; }
/usr/bin/grep -qF 'ci-smp-gate: PASS -- 0 corruption across all configs' "$OUT/smp.log" || { echo "SMP: exit 0 without the PASS line"; tail -20 "$OUT/smp.log"; exit 1; }
# Stricter than ci-smp-gate.sh, which passes a row with timing failures: every
# boot of every row must PASS, so a timing failure stops this gate to be read.
for l in default-smp1 default-smp4 default-smp8 ubsan-smp4 ubsan-smp8; do
  smp_row_clean "$OUT/smp.log" "$l"
  occurs "$OUT/smp.log" 1 "  PASS  $l"
done
# Each flavour keeps its own kernel directory and nothing rebuilds after the
# matrix's two bakes, so these are the kernels its rows booted.
record "SMP GREEN: 5 configs x N=10; default kernel $(shasum -a 256 build/kernel/thylacine.bin | cut -d' ' -f1); ubsan kernel $(shasum -a 256 build/kernel-undefined/thylacine.bin | cut -d' ' -f1); $(/usr/bin/grep -m1 'bake config' "$OUT/smp.log")"

[ "$(git -C "$GOFORK" rev-parse HEAD)" = "$GOFORK_PIN" ] && [ -z "$(git -C "$GOFORK" status --porcelain)" ] \
  || { echo "GOFORK moved or changed during the run"; exit 8; }
stratum_head_ok end; stratum_tree_ok end
[ "$(git rev-parse HEAD)" = "$MERGE_PIN" ] && [ "$(git status --porcelain --untracked-files=no | wc -l | tr -d ' ')" = 0 ] \
  || { echo "HEAD moved or the tree changed during the run"; exit 8; }
record "ALL GREEN: merge $MERGE_PIN, stratum $STRATUM_PIN, go fork $GOFORK_PIN (CONTROL, CONTROL-Q, UBSAN, PRODUCTION, SMP; NAMED $(echo $NAMED | wc -w | tr -d ' '))"
