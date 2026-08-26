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
tapestryd: warp host3d-ring skipped (blob feature not offered)
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
tapestryd: warp host3d-ring venus-ctx=512 MAPPED+ROUNDTRIP refcount=1 teardown OK
tapestryd: warp ring-recreate ridx-reuse OK (destroy -> re-mint ridx 0)
tapestryd: warp mem-recreate handle-reuse OK (alloc -> sentinel -> destroy -> re-alloc handle 0)
venus-prove: device-memory sentinel OK (type 0 size 4096, zero-at-map + c0deface round-tripped)
venus-prove: placed-map de-advertised OK (76 device exts)
venus-prove: GPU round-trip OK (vkCmdCopyBuffer 4 KiB, fenced submit, pattern survived FIRST map -- the F1 reify-at-alloc proof)
venus-prove: second logical device + queue OK (timeline 2 acquired -- the multi-queue lift)
venus-prove: two-timeline interleave OK (fenced copies on t1 then t2, waited t2 before t1, both patterns intact -- the F3 per-timeline retirement witness)
venus-prove: cap-exhaustion cycle OK (refused at the 64 MiB ctx cap, freed, realloc'd -- the F2 no-burn proof)
venus-prove: offscreen triangle OK (render pass + SPIR-V pipeline + draw + copy-out 64x64; center red, corner blue -- the W-1 pipeline-class witness)
venus-prove: slot-exhaustion cycles OK (253/253/253 to refusal, steady-state equal -- the F2 discriminating no-burn proof)
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
# V-3b-1c-2a: the SERVER host3d-ring path legs, one variable away each. The
# "venus-ctx=" line is emitted ONLY on a successful round-trip; a sentinel
# mismatch emits a FAIL line instead, which the gate must reject -- so replacing
# the success line with FAIL proves the gate keys on the verdict, not the token.
check "test leg lacks server host3d venus-ctx round-trip -> UNVERIFIED" 1 "" \
      'grep -v "warp host3d-ring venus-ctx=" "$t" > "$t.x" && mv "$t.x" "$t"'
check "test leg shows server host3d FAIL (sentinel mismatch) -> UNVERIFIED" 1 "" \
      'sed "s/warp host3d-ring venus-ctx=.*/warp host3d-ring FAIL (sentinel wrote 0x1 read 0x2)/" "$t" > "$t.x" && mv "$t.x" "$t"'
check "control leg ALSO sees server host3d venus-ctx -> UNVERIFIED" 1 \
      'printf "tapestryd: warp host3d-ring venus-ctx=512 MAPPED+ROUNDTRIP refcount=1 teardown OK\n" >> "$c"' ""
check "control leg lacks server host3d skip -> UNVERIFIED"       1 \
      'grep -v "warp host3d-ring skipped" "$c" > "$c.x" && mv "$c.x" "$c"' ""
# V-3b-3c-1: the ring-recreate ridx-reuse regression witness (F1 full fix). The
# "ring-recreate ridx-reuse OK" line is emitted ONLY when a destroyed host3d
# ring's ridx re-mints; a regression (the destroy verb fails to free the slot)
# emits the FAIL form. Two test-leg arms (absent, replaced-by-FAIL) prove the
# gate keys on the verdict; the control-leg arm proves it stays venus-scoped
# (the control device runs no venus ctx, so the line is absent there).
check "test leg lacks ring-recreate ridx-reuse -> UNVERIFIED" 1 "" \
      'grep -v "warp ring-recreate ridx-reuse OK" "$t" > "$t.x" && mv "$t.x" "$t"'
check "test leg shows ring-recreate FAIL (slot not freed) -> UNVERIFIED" 1 "" \
      'sed "s/warp ring-recreate ridx-reuse OK.*/warp ring-recreate FAIL (destroyed=true slot_freed=false remint_ok=false)/" "$t" > "$t.x" && mv "$t.x" "$t"'
check "control leg ALSO sees ring-recreate ridx-reuse -> UNVERIFIED" 1 \
      'printf "tapestryd: warp ring-recreate ridx-reuse OK (destroy -> re-mint ridx 0)\n" >> "$c"' ""
# V-3b-3c-2: the device-memory handle-reuse witness. "mem-recreate handle-reuse
# OK" is emitted ONLY when a device-memory blob mints, round-trips a sentinel,
# destroys (freeing its slot), and re-mints at the same handle; a regression
# (slot not freed, or the sentinel round-trip fails) emits the FAIL form. Two
# test-leg arms (absent, replaced-by-FAIL) prove the gate keys on the verdict;
# the control-leg arm proves it stays venus-scoped.
check "test leg lacks mem-recreate handle-reuse -> UNVERIFIED" 1 "" \
      'grep -v "warp mem-recreate handle-reuse OK" "$t" > "$t.x" && mv "$t.x" "$t"'
check "test leg shows mem-recreate FAIL (slot not freed) -> UNVERIFIED" 1 "" \
      'sed "s/warp mem-recreate handle-reuse OK.*/warp mem-recreate FAIL (sentinel=true destroyed=true slot_freed=false remint_ok=false)/" "$t" > "$t.x" && mv "$t.x" "$t"'
check "control leg ALSO sees mem-recreate handle-reuse -> UNVERIFIED" 1 \
      'printf "tapestryd: warp mem-recreate handle-reuse OK (alloc -> sentinel -> destroy -> re-alloc handle 0)\n" >> "$c"' ""
# V-3b-3c-2b: the CLIENT device-memory E2E witness. "device-memory sentinel OK"
# is emitted by thylacine-venus-prove ONLY when the full HOST_VISIBLE
# vkAllocateMemory+vkMapMemory path succeeds (zero-at-map + sentinel round-trip)
# over the mesa vn_renderer device-memory bo_ops; a sentinel mismatch or a
# disclosure-floor breach emits the prove's FAIL form instead. Two test-leg arms
# (absent, replaced-by-FAIL) prove the gate keys on the verdict, not the token;
# the control-leg arm proves it stays venus-scoped (the control device stubs the
# instance, so the prove reports ABSENT and the line is absent).
check "test leg lacks device-memory sentinel OK -> UNVERIFIED" 1 "" \
      'grep -v "device-memory sentinel OK" "$t" > "$t.x" && mv "$t.x" "$t"'
check "test leg shows device-memory sentinel MISMATCH -> UNVERIFIED" 1 "" \
      'sed "s/venus-prove: device-memory sentinel OK.*/THYLACINE-VENUS-PROVE FAIL: device-memory sentinel mismatch (wrote c0deface\/3f210531 read 00000000\/00000000)/" "$t" > "$t.x" && mv "$t.x" "$t"'
check "control leg ALSO sees device-memory sentinel OK -> UNVERIFIED" 1 \
      'printf "venus-prove: device-memory sentinel OK (type 0 size 4096, zero-at-map + c0deface round-tripped)\n" >> "$c"' ""
# Multi-queue chunk + vkQuake-arc W-1: the remaining prove witness lines (the
# multi-queue r2 note -- a witness line the gate never asserts can silently
# vanish). Per line: the test leg must carry it, the control leg must not
# (the control prove reports ABSENT, so every venus-prove witness is absent
# there). Every step's failure mode emits a THYLACINE-VENUS-PROVE FAIL line
# containing none of these keys, so the absent-arm also covers replaced-by-
# FAIL.
check "test leg lacks GPU round-trip OK -> UNVERIFIED" 1 "" \
      'grep -v "GPU round-trip OK" "$t" > "$t.x" && mv "$t.x" "$t"'
check "control leg ALSO sees GPU round-trip OK -> UNVERIFIED" 1 \
      'printf "venus-prove: GPU round-trip OK (vkCmdCopyBuffer 4 KiB, fenced submit, pattern survived FIRST map -- the F1 reify-at-alloc proof)\n" >> "$c"' ""
check "test leg lacks placed-map de-advertised OK -> UNVERIFIED" 1 "" \
      'grep -v "placed-map de-advertised OK" "$t" > "$t.x" && mv "$t.x" "$t"'
check "control leg ALSO sees placed-map de-advertised OK -> UNVERIFIED" 1 \
      'printf "venus-prove: placed-map de-advertised OK (76 device exts)\n" >> "$c"' ""
check "test leg lacks second logical device OK -> UNVERIFIED" 1 "" \
      'grep -v "second logical device + queue OK" "$t" > "$t.x" && mv "$t.x" "$t"'
check "control leg ALSO sees second logical device OK -> UNVERIFIED" 1 \
      'printf "venus-prove: second logical device + queue OK (timeline 2 acquired -- the multi-queue lift)\n" >> "$c"' ""
check "test leg lacks cap-exhaustion cycle OK -> UNVERIFIED" 1 "" \
      'grep -v "cap-exhaustion cycle OK" "$t" > "$t.x" && mv "$t.x" "$t"'
check "control leg ALSO sees cap-exhaustion cycle OK -> UNVERIFIED" 1 \
      'grep "cap-exhaustion cycle OK" "$t" >> "$c"' ""
check "test leg lacks two-timeline interleave OK -> UNVERIFIED" 1 "" \
      'grep -v "two-timeline interleave OK" "$t" > "$t.x" && mv "$t.x" "$t"'
check "control leg ALSO sees two-timeline interleave OK -> UNVERIFIED" 1 \
      'grep "two-timeline interleave OK" "$t" >> "$c"' ""
check "test leg lacks offscreen triangle OK -> UNVERIFIED" 1 "" \
      'grep -v "offscreen triangle OK" "$t" > "$t.x" && mv "$t.x" "$t"'
check "control leg ALSO sees offscreen triangle OK -> UNVERIFIED" 1 \
      'grep "offscreen triangle OK" "$t" >> "$c"' ""
check "test leg lacks slot-exhaustion cycles OK -> UNVERIFIED" 1 "" \
      'grep -v "slot-exhaustion cycles OK" "$t" > "$t.x" && mv "$t.x" "$t"'
check "control leg ALSO sees slot-exhaustion cycles OK -> UNVERIFIED" 1 \
      'grep "slot-exhaustion cycles OK" "$t" >> "$c"' ""
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
