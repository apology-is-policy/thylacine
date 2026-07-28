#!/bin/bash
# ci-idle-gate.sh -- the idle-CPU gate: boot, settle to the login prompt, and
# FAIL if the guest is spinning a core at idle.
#
# Why this exists: a boot probe (ambush-probe) once leaked a busy-yielding Go
# debuggee (a `for { runtime.Gosched() }` target that a debugger `exec`-launched
# then left resumed + orphaned to init), which pegged ~2.5 host cores forever at
# idle -- HVF sat at ~256% CPU where a healthy boot idles at <~30%. Nothing
# caught it: every functional test passed (the leak is invisible unless you
# watch steady-state CPU), and the boot banner printed OK. This gate is the
# standing guard against that class -- "an OS under a hypervisor must not heat
# the cores when it has nothing to do."
#
# Signal: the qemu process's steady-state %CPU. This is the GUEST's own CPU
# consumption (qemu's threads ARE the guest vCPUs + IO). It is robust against
# host load by construction: host contention can only DEFLATE qemu's share of
# wall-clock CPU, never inflate it -- so a busy host can never make an IDLE guest
# read high (no false FAIL). A real spin (the regression was 256%, >3x the
# threshold) fails it decisively regardless of host state. A FAIL here is a real
# guest defect to hunt, never a "host load" artifact (docs/DEBUGGING-PLAYBOOK.md).
#
# HVF only (the substrate where the idle cost is real + the user's dev loop);
# SKIPs gracefully where HVF is unavailable, like tools/test-interactive.sh.
#
# Env knobs:
#   THYLACINE_IDLE_THRESHOLD  fail above this mean %cpu (default 80 -- above the
#                             ~28% resident-compositor cost, below any 1-pegged-
#                             core spin at ~100%+)
#   THYLACINE_IDLE_SETTLE     seconds to settle before sampling (default 12)
#   THYLACINE_IDLE_SAMPLES    number of 2s %cpu samples to mean (default 5)
#   THYLACINE_IDLE_BOOT_TO    seconds to reach boot OK (default 150)
set -u
cd "$(dirname "$0")/.."

THRESHOLD="${THYLACINE_IDLE_THRESHOLD:-80}"
SETTLE="${THYLACINE_IDLE_SETTLE:-12}"
SAMPLES="${THYLACINE_IDLE_SAMPLES:-5}"
BOOT_TO="${THYLACINE_IDLE_BOOT_TO:-150}"
LOG="$(mktemp /tmp/thyla-idle-gate.XXXXXX.log)"
# #89: THIS tree's absolute disk path -- the discriminator every process match
# below is scoped to. Mirrors run-vm.sh's resolution (we cd'd to the repo root
# above), so the pattern is exactly the string qemu carries on its cmdline.
DISK_IMG="${THYLACINE_DISK_IMG:-$PWD/build/disk.img}"

# HVF gate: SKIP (exit 0) if the host cannot run HVF, so this is safe in a
# TCG-only CI without failing the pipeline.
if ! qemu-system-aarch64 -accel help 2>/dev/null | grep -qw hvf; then
    echo "ci-idle-gate: SKIP (no HVF on this host)"
    exit 0
fi

echo "ci-idle-gate: boot (hvf, headless) -> settle ${SETTLE}s -> sample ${SAMPLES}x2s; FAIL if mean > ${THRESHOLD}%"

THYLACINE_ACCEL=hvf THYLACINE_DISPLAY=none tools/run-vm.sh < /dev/null > "$LOG" 2>&1 &
RUNPID=$!

booted=0
for _ in $(seq 1 "$BOOT_TO"); do
    if grep -aq "Thylacine boot OK" "$LOG" 2>/dev/null; then booted=1; break; fi
    grep -aq "EXTINCTION:" "$LOG" 2>/dev/null && { echo "ci-idle-gate: FAIL (kernel EXTINCTION during boot; see $LOG)"; kill "$RUNPID" 2>/dev/null; exit 1; }
    kill -0 "$RUNPID" 2>/dev/null || { echo "ci-idle-gate: FAIL (qemu exited before boot OK; see $LOG)"; exit 1; }
    sleep 1
done
[ "$booted" = 1 ] || { echo "ci-idle-gate: FAIL (no boot OK within ${BOOT_TO}s; see $LOG)"; kill "$RUNPID" 2>/dev/null; pkill -f "$DISK_IMG" 2>/dev/null; exit 1; }

sleep "$SETTLE"

# #89: match on THIS tree's absolute disk path, never a bare `.*disk.img`.
# A sibling worktree (thylacine-aux) runs its own qemu off
# <...>/thylacine-aux/build/disk.img, which a loose pattern matches too -- so
# the old form could KILL another agent's VM, and worse, `head -1` could select
# it and we would then sample the WRONG process and report a bogus idle number.
# A silently-wrong measurement is the failure mode to design against here, so an
# ambiguous match is a loud FAIL rather than a pick.
QPID=$(pgrep -f "$DISK_IMG" || true)
QN=$(printf '%s\n' "$QPID" | grep -c . || true)
[ "$QN" = 1 ] || { echo "ci-idle-gate: FAIL (expected exactly 1 qemu on $DISK_IMG, found $QN: $QPID) -- do not run this gate alongside another VM on the same tree; the idle number would be ambiguous either way"; kill "$RUNPID" 2>/dev/null; exit 1; }

sum=0; k=0; samples=""
for _ in $(seq 1 "$SAMPLES"); do
    v=$(ps -o %cpu= -p "$QPID" 2>/dev/null | tr -d ' ')
    [ -n "$v" ] || v=0
    samples="$samples $v"
    sum=$(echo "$sum + $v" | bc)
    k=$((k + 1))
    sleep 2
done
mean=$(echo "scale=1; $sum / $k" | bc)

# clean kill (by pid -- never a broad pkill of unrelated qemus)
kill "$QPID" 2>/dev/null
kill "$RUNPID" 2>/dev/null
sleep 1
kill -9 "$QPID" 2>/dev/null

echo "ci-idle-gate: idle %cpu samples =${samples} ; mean = ${mean}%"
# bc returns 1 for true; guard the empty-string case.
over=$(echo "$mean > $THRESHOLD" | bc)
if [ "$over" = 1 ]; then
    echo "ci-idle-gate: FAIL -- idle mean ${mean}% > ${THRESHOLD}% (a core is spinning at idle; hunt the mechanism, see $LOG + docs/DEBUGGING-PLAYBOOK.md)"
    exit 1
fi
echo "ci-idle-gate: PASS -- idle mean ${mean}% <= ${THRESHOLD}%"
rm -f "$LOG"
exit 0
