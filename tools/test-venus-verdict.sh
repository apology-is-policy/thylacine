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

printf '\n%d pass, %d fail\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
echo "venus-verdict: DISCRIMINATES"
