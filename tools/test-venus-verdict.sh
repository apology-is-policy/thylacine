#!/usr/bin/env bash
# The venus gate's verdict, tested without booting (fast; drives the REAL
# verdict verb, not a copy of it). tools/test-smp-classify.sh is the precedent.
#
# Why this exists: `warp-host.sh venus` costs two ~220 s guest boots on a remote
# GL host, so its verdict is the least affordable thing in the tree to test by
# running it. A verdict nothing can afford to exercise is a verdict that rots.
#
# Every case is ONE variable away from the clean pair, and the clean pair is
# included -- a suite of only-negative cases is satisfied by a verdict that
# always fails, and a suite of only-positive cases by one that always passes.
set -u
cd "$(dirname "$0")/.."

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
PASS=0; FAIL=0

# The clean pair, verbatim in shape from a real thyla-pi run (2026-08-18).
mk_control() {
    cat > "$1" <<'EOF'
BOOT-vencontrol: polls=43 (~215s)
BOOT-vencontrol: PASS
tapestryd: gpu virgl -- num_scanouts=1 num_capsets=2
tapestryd: gpu capset[0] id=1 max_version=1 max_size=308
tapestryd: gpu capset[1] id=2 max_version=2 max_size=1384
tapestryd: gpu ctx-capset id=2 CREATED
tapestryd: gpu ctx-capset id=4 skipped (capset not enumerated)
tapestryd: gpu blob-create skipped (F_RESOURCE_BLOB not offered)
tapestryd: gpu host3d-map skipped (F_RESOURCE_BLOB not offered)
tapestryd: gpu hostmem-ring skipped (F_RESOURCE_BLOB not offered)
EOF
}
mk_test() {
    cat > "$1" <<'EOF'
BOOT-venustest: polls=43 (~215s)
BOOT-venustest: PASS
tapestryd: gpu virgl -- num_scanouts=1 num_capsets=3
tapestryd: gpu capset[0] id=1 max_version=1 max_size=308
tapestryd: gpu capset[1] id=2 max_version=2 max_size=1384
tapestryd: gpu capset[2] id=4 max_version=0 max_size=160
tapestryd: gpu ctx-capset id=2 CREATED
tapestryd: gpu ctx-capset id=4 CREATED
tapestryd: gpu blob-create guest CREATED
tapestryd: gpu host3d-map venus-ctx MAPPED (map_info=0x1)
tapestryd: gpu host3d-map global create refused
tapestryd: gpu hostmem-ring MAPPED+ROUNDTRIP x2 (off_a=0x0 off_b=0x1000 cache=CACHED) teardown+remint-reuse OK
EOF
}

# check <name> <want-exit 0|1> <control-mutator> <test-mutator>
check() {
    local name="$1" want="$2" cmut="$3" tmut="$4"
    local c="$WORK/c.log" t="$WORK/t.log"
    mk_control "$c"; mk_test "$t"
    [ -n "$cmut" ] && eval "$cmut"
    [ -n "$tmut" ] && eval "$tmut"
    local out; out=$(tools/warp-host.sh venus-verdict "$c" "$t" 2>&1); local rc=$?
    if [ "$rc" -eq "$want" ]; then
        printf '  PASS  %s (rc=%d)\n' "$name" "$rc"; PASS=$((PASS+1))
    else
        printf '  FAIL  %s (rc=%d, wanted %d)\n%s\n' "$name" "$rc" "$want" "$out"
        FAIL=$((FAIL+1))
    fi
}

echo "== venus-verdict discrimination =="
# The positive control, and it goes first deliberately: without it EVERY
# negative below is satisfied by a verdict that refuses everything. Stated
# without a count -- a count in a comment is a status field whose flip is
# nobody's step, and this one already said "four" while seven followed it.
check "clean pair VERIFIES"                 0 "" ""
# V-3b-1a: the HOST3D ring substrate legs, one variable away each.
check "test leg lacks host3d venus-ctx MAP -> UNVERIFIED" 1 "" \
      'grep -v "host3d-map venus-ctx MAPPED" "$t" > "$t.x" && mv "$t.x" "$t"'
check "test leg lacks the device-global refusal control -> UNVERIFIED" 1 "" \
      'grep -v "host3d-map global create refused" "$t" > "$t.x" && mv "$t.x" "$t"'
check "control leg lacks host3d skip -> UNVERIFIED"       1 \
      'grep -v "host3d-map skipped" "$c" > "$c.x" && mv "$c.x" "$c"' ""
check "control leg ALSO sees host3d MAPPED -> UNVERIFIED" 1 \
      'printf "tapestryd: gpu host3d-map venus-ctx MAPPED (map_info=0x1)\n" >> "$c"' ""
# V-3b-1c: the persistent hostmem ring-engine legs, one variable away each.
check "test leg lacks hostmem-ring MAPPED+ROUNDTRIP x2 -> UNVERIFIED" 1 "" \
      'grep -v "hostmem-ring MAPPED+ROUNDTRIP" "$t" > "$t.x" && mv "$t.x" "$t"'
# The reuse/distinct discrimination: the probe emits the success line ONLY when
# both rings mapped AND the free-list reclaimed a freed offset -- a lifecycle
# regression (reuse=false) emits FAIL, which the gate must reject. Replacing the
# success line with FAIL proves the gate keys on the verdict, not the mere token.
check "test leg shows hostmem-ring FAIL (reuse regressed) -> UNVERIFIED" 1 "" \
      'sed "s/hostmem-ring MAPPED+ROUNDTRIP.*/hostmem-ring FAIL (a_ok=true b_ok=true distinct=true reuse=false)/" "$t" > "$t.x" && mv "$t.x" "$t"'
check "control leg ALSO sees hostmem-ring MAPPED+ROUNDTRIP x2 -> UNVERIFIED" 1 \
      'printf "tapestryd: gpu hostmem-ring MAPPED+ROUNDTRIP x2 (off_a=0x0 off_b=0x1000 cache=CACHED) teardown+remint-reuse OK\n" >> "$c"' ""
check "control leg lacks hostmem-ring skip -> UNVERIFIED"       1 \
      'grep -v "hostmem-ring skipped" "$c" > "$c.x" && mv "$c.x" "$c"' ""
# One variable away, each direction of the discrimination the gate claims.
check "control leg ALSO sees id=4 -> UNVERIFIED" 1 \
      'printf "tapestryd: gpu capset[2] id=4 max_version=0 max_size=160\n" >> "$c"' ""
check "test leg sees NO id=4 -> UNVERIFIED"      1 "" \
      'grep -v "id=4" "$t" > "$t.x" && mv "$t.x" "$t"'
# A leg that did not boot has no verdict to give -- distinct from a leg that
# booted and disagreed, and the reason both boot lines are checked at all.
check "control leg did not boot -> UNVERIFIED"   1 \
      'grep -v "BOOT-vencontrol: PASS" "$c" > "$c.x" && mv "$c.x" "$c"' ""
check "test leg did not boot -> UNVERIFIED"      1 "" \
      'grep -v "BOOT-venustest: PASS" "$t" > "$t.x" && mv "$t.x" "$t"'
# A control that measured NOTHING (2D fallback: virgl not negotiated, no capset
# lines at all) trivially lacks id=4. Without this arm the gate reads that as
# "venus absent" when it means "capsets absent" -- a negative assertion
# satisfied by a broken fixture.
check "control leg enumerated NO capsets -> UNVERIFIED" 1 \
      'grep -v "gpu capset\[" "$c" > "$c.x" && mv "$c.x" "$c"' ""
check "control leg lost only baseline id=1 -> UNVERIFIED" 1 \
      'grep -v "id=1 " "$c" > "$c.x" && mv "$c.x" "$c"' ""
# Prefix collision: a capset numbered 40+ must not satisfy the id=4 check. Both
# id checks anchor on a trailing space for this reason; without it the test leg
# would "see id=4" on a device that never advertised Venus.
check "test leg has id=40, not id=4 -> UNVERIFIED" 1 "" \
      'sed "s/id=4 /id=40 /" "$t" > "$t.x" && mv "$t.x" "$t"'

# --- V-0b ctx-capset arms: a capset-selected context must actually create ---
# The false-pass the whole rung exists to catch: a device that ignored
# context_init would create a capset-4 context even without venus. If the
# control leg shows id=4 CREATED, the test leg's id=4 CREATED proves nothing.
check "control leg shows id=4 CREATED -> UNVERIFIED" 1 \
      'printf "tapestryd: gpu ctx-capset id=4 CREATED\n" >> "$c"' ""
# The Venus context must create on the test leg -- capset advertised but context
# unreachable (the render-server question) is a real, distinct failure.
check "test leg: Venus ctx did NOT create -> UNVERIFIED" 1 "" \
      'grep -v "ctx-capset id=4 CREATED" "$t" > "$t.x" && mv "$t.x" "$t"'
# The id=2 positive control anchors both legs: without it, "control lacks id=4
# CREATED" is satisfied by a leg where context creation was broken outright.
check "control leg: virgl id=2 control did NOT create -> UNVERIFIED" 1 \
      'grep -v "ctx-capset id=2 CREATED" "$c" > "$c.x" && mv "$c.x" "$c"' ""
check "test leg: virgl id=2 control did NOT create -> UNVERIFIED" 1 "" \
      'grep -v "ctx-capset id=2 CREATED" "$t" > "$t.x" && mv "$t.x" "$t"'
# main audit F3: the control leg must POSITIVELY show the id=4 skip, not merely
# lack "id=4 CREATED". Strip the skipped line and the verdict must fail.
check "control leg: no id=4 'skipped' line -> UNVERIFIED" 1 \
      'grep -v "ctx-capset id=4 skipped" "$c" > "$c.x" && mv "$c.x" "$c"' ""

# --- V-1 guest-blob arms: a blob must create where the feature is negotiated ---
# Test leg must CREATE; a device that refused RESOURCE_CREATE_BLOB (or a driver
# that never negotiated the feature) fails the rung.
check "test leg: guest blob did NOT create -> UNVERIFIED" 1 "" \
      'grep -v "blob-create guest CREATED" "$t" > "$t.x" && mv "$t.x" "$t"'
# The false-pass this arm exists for: a blob created on the control leg means
# the driver put a blob command on a wire that never offered F_RESOURCE_BLOB,
# so the test leg's CREATED proves nothing about the gate.
check "control leg shows blob CREATED -> UNVERIFIED" 1 \
      'printf "tapestryd: gpu blob-create guest CREATED\n" >> "$c"' ""
# The positive skip (same F3 lesson): control must SHOW it took the no-feature
# path, not merely lack CREATED -- an absent line is satisfied by a probe that
# never ran at all.
check "control leg: no blob 'skipped' line -> UNVERIFIED" 1 \
      'grep -v "blob-create skipped" "$c" > "$c.x" && mv "$c.x" "$c"' ""

printf '\n%d pass, %d fail\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
echo "venus-verdict: DISCRIMINATES"
