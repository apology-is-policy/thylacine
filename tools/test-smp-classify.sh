#!/usr/bin/env bash
# Discrimination test for the SMP gate's failure classifier (#222).
#
# WHY THIS EXISTS. The gate's EXTERNAL-KILL bucket was structurally incapable
# of catching the signal that most needs it: it keyed on QEMU's own
# 'terminating on signal' report, which SIGKILL -- being uncatchable --
# prevents QEMU from ever printing. Two real #200 sightings therefore landed
# in OTHER. Nothing detected that, because no test ever exercised the ladder;
# each arm was assumed to fire rather than known to.
#
# So this drives the REAL classifier, sourced out of tools/smp-multiboot.sh,
# over fixtures. It deliberately does NOT re-declare the patterns: a checker
# that re-derives its reference from the thing under test can only report that
# the copy agrees with itself (#143).
#
# It proves DISCRIMINATION, not detection -- every arm gets a negative that
# must NOT fire it, because an arm that matches everything is not a detector.
# The load-bearing negative is case G: the harness's OWN teardown kill also
# emits the shell's 'Killed: 9' notification whenever a command boundary
# elapses before the reap (measured, not assumed), so the notification alone
# never means "someone else did it".
#
# Fast (no boots). Fixtures: two REAL captures under tools/testdata/, plus
# synthetic guest logs written per-case so each case's content is explicit.
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DATA="$REPO_ROOT/tools/testdata/smp-classify"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Source the gate for its classifier. Its own arg defaults apply; the source
# guard returns before any boot or filesystem side effect.
# shellcheck source=/dev/null
source "$REPO_ROOT/tools/smp-multiboot.sh"

fails=0
checked=0

# expect <case> <expected-token> <guest-log> <harness-log>
expect() {
    local name="$1" want="$2" glog="$3" hlog="$4"
    local got
    got="$(classify_boot "$glog" "$hlog")"
    checked=$((checked+1))
    if [[ "$got" == "$want" ]]; then
        printf '  ok   %-46s -> %s\n' "$name" "$got"
    else
        printf '  FAIL %-46s -> %s (want %s)\n' "$name" "$got" "$want"
        fails=$((fails+1))
    fi
}

expect_token() {
    local name="$1" want="$2" hlog="$3"
    local got
    got="$(harness_result_token "$hlog")"
    checked=$((checked+1))
    if [[ "$got" == "$want" ]]; then
        printf '  ok   %-46s -> %s\n' "$name" "$got"
    else
        printf '  FAIL %-46s -> %s (want %s)\n' "$name" "$got" "$want"
        fails=$((fails+1))
    fi
}

# ---- synthetic guest logs -------------------------------------------------
# A healthy guest that reached the banner and passed its suite.
cat > "$TMP/guest-green.log" <<'EOF'
Thylacine v0.1.0-dev booting...
Thylacine boot OK
[test] tests: 1394/1394 PASS
EOF

# The same, cut off mid-run: QEMU vanished, so no banner and no summary.
cat > "$TMP/guest-truncated.log" <<'EOF'
Thylacine v0.1.0-dev booting...
fsbench: seqwrite: 8 MiB in 54 ms -> 146.72 MiB/s
EOF

# A real SMP context-corruption signature.
cat > "$TMP/guest-corrupt.log" <<'EOF'
Thylacine v0.1.0-dev booting...
EXTINCTION: sched: invalid prev state
EOF

# QEMU's own catchable-signal report (EXTERNAL-KILL arm 1).
cat > "$TMP/guest-qemureport.log" <<'EOF'
Thylacine v0.1.0-dev booting...
qemu-system-aarch64: terminating on signal 15 from pid 4242 (zsh)
EOF

# The #362 inject-miss shape: provably green guest, key never delivered.
cat > "$TMP/guest-inject.log" <<'EOF'
Thylacine boot OK
virtio-input: AWAITING_QMP_KEY
virtio-input: SKIP
[test] tests: 1394/1394 PASS
EOF

# Benign host-fragility soft-warn.
cat > "$TMP/guest-timing.log" <<'EOF'
Thylacine boot OK
[SOFT-WARN] IRQ-to-userspace p99 exceeds CI sanity budget
EOF

# The forgery vector, and it has to be in the HARNESS log to be one: on the
# qemu-exit path test.sh cats the ENTIRE guest log into its own stream, so
# guest output ends up inside the very file the SIGKILL arm greps. Everything
# here except the anchored 'line N: PID Killed: N' shape is satisfied --
# teardown-dead is set -- so this case fails iff the pattern is loosened to a
# bare token. (Putting the forged text only in the guest log would test
# nothing: the arm never reads that file.)
cat > "$TMP/harn-embedded-forgery.log" <<'EOF'
==> Booting kernel under QEMU (timeout 90s)...
==> harness: qemu_alive_at_teardown=0
==> FAIL: QEMU exited without emitting boot marker.
--- full log ---
Thylacine boot OK
[test] proc.group_terminate: child Killed: 9 (expected)
[test] tests: 1394/1394 PASS
EOF

# ---- synthetic harness logs -----------------------------------------------
# An external SIGKILL: the shell notification AND proof the harness's own
# kill hit an already-dead process.
cat > "$TMP/harn-extkill.log" <<'EOF'
==> Booting kernel under QEMU (timeout 90s)...
/Users/x/projects/thylacine-aux/tools/test.sh: line 259: 7655 Killed: 9               THYLACINE_BUILD_DIR=... run-vm.sh
==> harness: qemu_alive_at_teardown=0
==> FAIL: QEMU exited without emitting boot marker.
EOF

# CASE G, the load-bearing negative: the shell notification WITHOUT
# alive_at_teardown=0. This is what the harness's own teardown kill looks
# like when a command boundary elapsed before the reap -- measured to happen
# (tools/test.sh keeps its kill and wait adjacent precisely to avoid it).
# Classifying this as EXTERNAL-KILL would blame the outside world for the
# harness's own routine teardown.
cat > "$TMP/harn-selfkill.log" <<'EOF'
==> Booting kernel under QEMU (timeout 90s)...
/Users/x/projects/thylacine-aux/tools/test.sh: line 259: 7655 Killed: 9               THYLACINE_BUILD_DIR=... run-vm.sh
==> harness: qemu_alive_at_teardown=1
==> FAIL: G-4 console gate -- the Aurora scanout did not verify.
EOF

# QEMU exited on its own, no signal: alive_at_teardown=0 but no notification.
# A real QEMU bug or a clean exit without the banner -- NOT an external kill.
cat > "$TMP/harn-quietexit.log" <<'EOF'
==> Booting kernel under QEMU (timeout 90s)...
==> harness: qemu_alive_at_teardown=0
==> FAIL: QEMU exited without emitting boot marker.
EOF

cat > "$TMP/harn-gpugate.log" <<'EOF'
==> harness: qemu_alive_at_teardown=1
==> FAIL: G-4 console gate -- the Aurora scanout did not verify.
EOF

cat > "$TMP/harn-timeout.log" <<'EOF'
==> harness: qemu_alive_at_teardown=1
==> FAIL: timeout (300s) - no boot marker.
EOF

: > "$TMP/harn-empty.log"

echo "== EXTERNAL-KILL arm 2 (SIGKILL via the shell notification) =="
expect "real external SIGKILL capture"        EXTKILL "$TMP/guest-truncated.log" "$DATA/real-extkill-harness.log"
expect "synthetic SIGKILL + teardown-dead"    EXTKILL "$TMP/guest-truncated.log" "$TMP/harn-extkill.log"

echo "== the negatives that make it a discriminator, not a matcher =="
expect "REAL passing harness log"             OTHER   "$TMP/guest-truncated.log" "$DATA/real-pass-harness.log"
expect "case G: notification, qemu WAS alive" OTHER   "$TMP/guest-truncated.log" "$TMP/harn-selfkill.log"
expect "teardown-dead but no signal at all"   OTHER   "$TMP/guest-truncated.log" "$TMP/harn-quietexit.log"
expect "guest text embedded in harness log"   OTHER   "$TMP/guest-truncated.log" "$TMP/harn-embedded-forgery.log"
expect "no harness log at all"                OTHER   "$TMP/guest-truncated.log" "$TMP/nonexistent.log"

echo "== EXTERNAL-KILL arm 1 (QEMU's own report) still works =="
expect "qemu terminating-on-signal report"    EXTKILL "$TMP/guest-qemureport.log" "$TMP/harn-empty.log"

echo "== ladder order: nothing masks a real corruption =="
expect "corruption alone"                     CORRUPT "$TMP/guest-corrupt.log"  "$TMP/harn-empty.log"
expect "corruption + SIGKILL harness"         CORRUPT "$TMP/guest-corrupt.log"  "$TMP/harn-extkill.log"
expect "corruption + qemu kill report"        CORRUPT "$TMP/guest-corrupt.log"  "$TMP/harn-extkill.log"
expect "extkill outranks inject-miss"         EXTKILL "$TMP/guest-inject.log"   "$TMP/harn-extkill.log"

echo "== the pre-existing arms are unchanged =="
expect "inject-miss green guest"              INJECT  "$TMP/guest-inject.log"   "$TMP/harn-empty.log"
expect "benign timing soft-warn"              TIMING  "$TMP/guest-timing.log"   "$TMP/harn-empty.log"
expect "unclassified post-banner gate failure" OTHER  "$TMP/guest-green.log"    "$TMP/harn-gpugate.log"

echo "== result tokens (labelling only -- no verdict rests on these) =="
expect_token "real external-kill capture"     qemu-exit "$DATA/real-extkill-harness.log"
expect_token "real passing capture"           pass      "$DATA/real-pass-harness.log"
expect_token "gpu-gate"                       gpu-gate  "$TMP/harn-gpugate.log"
expect_token "timeout"                        timeout   "$TMP/harn-timeout.log"
expect_token "unrecognised verdict"           unknown   "$TMP/harn-empty.log"

echo
if (( fails == 0 )); then
    echo "== smp-classify: $checked/$checked PASS =="
    exit 0
fi
echo "== smp-classify: $fails/$checked FAILED =="
exit 1
