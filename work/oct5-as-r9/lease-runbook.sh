#!/bin/sh
# AS-R9 guest verification -- the lease window, in order. PREPARED OFF-LEASE so
# the scarce Mac lease is spent executing, not exploring.
#
# This base PREDATES main's disk_floor_check (grep count 0 in tools/build.sh),
# so the floor is enforced HERE instead: an unguarded bake on a shared volume is
# what broke every agent's shell at 10:46Z. Refuse rather than ENOSPC.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
# The floor is not here to protect ME -- it is here to stop my bake pushing the
# shared volume below MAIN's floor (THYLACINE_MIN_FREE_GB=6), which would make
# THEIR build.sh refuse mid-landing. So: 6 (their floor) + ~1 GB (my expected
# delta: two kernel flavours ~70M, a pool+ramfs re-bake ~200M, cargo deltas ~0
# because no userspace source changed) + margin. Measured 7.4 GiB free at
# 21:0xZ while main's CI legs ran, down from 9.47 at 20:49Z -- the volume is
# TIGHTENING, so re-read it at every stage rather than once at the start.
FLOOR_GB=${FLOOR_GB:-8}

free_gb() { df -g . | awk 'NR==2 {print $4}'; }
floor() {
  g=$(free_gb)
  [ "$g" -ge "$FLOOR_GB" ] || { echo "REFUSING at stage '$1': ${g} GiB free < ${FLOOR_GB} GiB floor"; exit 1; }
  echo "-- stage '$1': ${g} GiB free (floor ${FLOOR_GB})"
}

floor start

# Stage 0 -- THE SPEC OBLIGATION, and it is INDEPENDENT of everything below.
# CLAUDE.md: any change to a mechanism modelled in specs/ re-runs that spec's
# buggy cfgs. Two apply:
#   burrow.tla   -- models the dual-refcount lifecycle (I-7) whose {0,0}
#                   decision this repair RELOCATED into the settled drops.
#                   3 buggy cfgs: free_on_close (66), free_on_unmap (54),
#                   never_free (43) -- each must still violate NoUseAfterFree.
#   capacity.tla -- one of the specs that models the CHARGE accounting (I-32),
#                   which is the mechanism AS-R9 is about.
# These need NO image, NO ramfs, NO pool and none of astra's artifacts -- only
# java + TLC. So they run on the lease regardless of how yip 0169 is ruled, and
# they are the first thing to do when the lease lands.
# TRAP: ~/tla2tools.jar is STALE -- use the jar SPEC-POLICY.md names.
if [ "${SPECS:-1}" = 1 ]; then
  # The jar lives in /tmp, which the 2026-10-06 reboot CLEARED -- and
  # ~/tla2tools.jar is STALE (it reports violated temporal properties
  # differently). Fetch SPEC-POLICY's pinned release, then PRINT the version,
  # because the whole point of pinning it is that the output format differs.
  JAR=/tmp/tla2tools.jar
  [ -f "$JAR" ] || curl -sL -o "$JAR" \
    https://github.com/tlaplus/tlaplus/releases/download/v1.8.0/tla2tools.jar
  java -cp "$JAR" tlc2.TLC 2>&1 | grep -m1 'TLC2 Version' || true

  # CLEAN-cfg runs are SUSPENDED (SPEC-POLICY, since 2026-05-21). The binding
  # obligation is the BUGGY cfgs: each must STILL produce its counterexample.
  # A buggy cfg that now PASSES is a FINDING, not a convenience -- it means the
  # spec stopped constraining the thing it was written to catch, which is
  # exactly how a repair can silently void its own proof.
  cd specs
  # burrow.tla -- I-7, the dual-refcount lifecycle whose {0,0} decision this
  # repair RELOCATED into the settled drops. Each must violate NoUseAfterFree.
  for c in burrow_buggy_free_on_close burrow_buggy_free_on_unmap burrow_buggy_never_free; do
    echo "-- $c (expect: NoUseAfterFree VIOLATED)"
    java -cp "$JAR" tlc2.TLC -workers auto -deadlock -config "$c.cfg" burrow.tla 2>&1 | tail -4
  done
  # capacity.tla -- the I-32 charge accounting itself. detach_no_refund is
  # literally AS-R9's second arm: the holder that frees finds the record
  # cleared and refunds nothing. DISCRIMINATING form: each must violate
  # NoOrphan *with ChargeConserved listed ahead of it and HOLDING*. A run that
  # reported the COUNTER violated instead would mean the model no longer says
  # the counter is blind -- a different finding, not a pass.
  for c in capacity_buggy_detach_no_refund capacity_buggy_replace_orphans; do
    echo "-- $c (expect: NoOrphan VIOLATED, ChargeConserved HOLDING)"
    java -cp "$JAR" tlc2.TLC -workers auto -deadlock -config "$c.cfg" capacity.tla 2>&1 | tail -6
  done
  cd "$ROOT"
fi
floor post-specs

# Stage 1 -- the warm cache. ONLY with astra's ruling on yip 0169; her tree is
# at our shared base 5ff62b788 and config-equivalent to --config ci. Drop every
# CMake tree whose cache names HER path, per reference-ci-image-worktree-recipe.
if [ "${CLONE_APPROVED:-0}" = 1 ]; then
  [ -d build ] || cp -Rc ../thylacine-astra/build build
  rm -rf build/kernel build/usr build/pouch/stratumd-cmake build/kernel-undefined build/host-stratum
  rsync -a --ignore-existing ../thylacine-astra/third_party/rust/ third_party/rust/ 2>/dev/null || true
  floor post-clone
else
  echo "CLONE_APPROVED!=1 -- not cloning astra's build/. Her artifacts, her ruling (yip 0169)."
  exit 2
fi

# Stage 2 -- MY kernel from MY source. The only thing the cache must not supply.
tools/build.sh kernel --config ci
floor post-build

# Stage 3 -- verify the image by CONTENT, not by the build's exit code. This is
# the BAKE-TRAP class: failure looks like absent content plus a green ledger.
# A stale or mis-baked kernel cannot make the suite total rise by exactly 4.
ELF=build/kernel/thylacine.elf
# THE DENOMINATOR FIRST. If `strings` finds nothing at all -- wrong path, empty
# or truncated ELF -- every name below reads as ABSENT and the loop would blame
# my tests for a broken search. So prove the search searched something, using a
# name that exists at my BASE and that I did not add.
if ! strings "$ELF" | grep -qF 'burrow.refcount_lifecycle'; then
  echo "   CONTROL ABSENT: burrow.refcount_lifecycle is not in $ELF."
  echo "   The SEARCH is broken, not the tests -- check the path and the bake. STOP"
  exit 1
fi
echo "   control present: burrow.refcount_lifecycle (the search works)"
# grep -F: the '.' in a test name is a literal, not a regex any-char.
echo "-- the four witnesses must be REGISTERED in the built ELF:"
for t in settled_drop_retains_nonfinal_charge settled_drop_exact_payer \
         settled_mapping_drop_defers_free unmap_failure_leaves_mapping_attached; do
  strings "$ELF" | grep -qF "burrow.$t" \
    && echo "   present: burrow.$t" \
    || { echo "   ABSENT: burrow.$t -- the ELF does not carry my tests; STOP"; exit 1; }
done

# Stage 4 -- the suite. The 4 new tests execute for the FIRST time here. A
# failing suite extincts the boot (main.c: extinction("kernel test suite
# failed")), so a red is loud, not silent.
tools/test.sh 2>&1 | tee work/oct5-as-r9/guest-test.log
echo "-- suite total must be base+4; a skip is NOT coverage (OPEN-BUGS: 17 ramfs"
echo "   probe tests pass when their initrd file is missing):"
grep -E '  tests: [0-9]+/[0-9]+' work/oct5-as-r9/guest-test.log || true
grep -c '\[skip\]' work/oct5-as-r9/guest-test.log || true

# Stage 5 -- the one that matters. AS-R9 is an SMP race: a single-CPU green
# proves little, and the Oct 1-2 single-boot waiver has expired.
floor pre-smp
tools/ci-smp-gate.sh 2>&1 | tee work/oct5-as-r9/guest-smp.log

# Stage 6 -- THE SECOND AXIS, and for a race fix it is not optional padding.
# Everything above runs on one memory model (Apple M2 under HVF). AS-R9 is an
# SMP race, so a green on one silicon is one reading; thyla-pi is the only
# non-Apple ARM64 in the loop (4x Cortex-A72, real KVM) and #214 was closed on
# it. Two causes can share one reading -- only a second axis separates them.
#
# pi is FREE per `yip resources` and has NO reservation protocol, but keep QEMU
# single-flight: ssh thyla-pi 'ps -eo pid,args | grep "[q]emu-system"'.
#
# PAIRING TRAP (docs/agent/THYLA-PI.md): pool.img and the key-bearing
# ramfs.cpio ship TOGETHER or the guest gets STM_EBADTAG -> stratumd rc=-201 ->
# EXTINCTION: joey exited non-zero. A rebuilt ramfs carries a fresh key that no
# longer matches an already-synced pool. So sync BOTH from this tree, and do
# NOT re-bake either one on the far side.
# 4 GB RAM: ONE 2048 MiB guest at a time.
if [ "${PI_AXIS:-1}" = 1 ]; then
  echo "-- second axis: syncing this tree's kernel + PAIRED pool/ramfs to thyla-pi"
  WARP_HOST=thyla-pi tools/warp-host.sh sync
  echo "-- then run the SMP boots there under real KVM, A72 weak memory"
fi

echo "DONE. Report SHAs + evidence to astra on yip 0161 BEFORE any integration."
echo "Report BOTH axes separately: M2/HVF and A72/KVM. A race fix green on one"
echo "memory model is one reading, not a qualification."
