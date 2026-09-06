#!/usr/bin/env bash
# tools/test-nocturne-volume.sh -- the Nocturne N-3a-2 sink-volume gate witness
# (docs/NOCTURNE.md 6.8/6.10, I-46).
#
# Boots the default build ONCE with thylacine.volprobe, which makes joey run
# /nocturne-vol-probe after the /dev/nocturne mount. The probe runs TWO arms
# over DIRECT /srv/nocturne connections (never joey's shared /dev/nocturne
# mount, whose server-side peer is the SYSTEM mounter -- so a write through it
# could never exercise the per-caller gate):
#
#   POSITIVE (the probe itself, SYSTEM): a direct-conn volume write is ACCEPTED
#   and the Plan 9 volume(3) grammar round-trips -- audio/mix, one value or L R,
#   mute (audio 0), and an unknown control -> EINVAL.
#
#   NEGATIVE (a user-principal child, spawned with SPAWN_IDENTITY_SET): the SAME
#   direct-conn write is REFUSED (EPERM) -- not SYSTEM, not console-attached, no
#   CAP_AUDIO_GRAPH. Without this arm a return-true gate would pass the positive
#   alone (a control must prove discrimination, not detection).
#
# No wav capture (a control-file + authority test), so it needs no host audio
# backend. Not a multi-boot; like every boot gate it must not run beside another
# VM from this tree (#224). The AUDIBLE gain-stage attenuation is deferred to the
# N-3d volume-OSD chunk that drives it end to end.
#
# Usage: tools/test-nocturne-volume.sh   -- one boot + the guest-side PASS lines

set -euo pipefail
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOG="$REPO_ROOT/build/test-boot.log"

echo "==> booting with THYLACINE_VOLPROBE=1 (/nocturne-vol-probe in the boot ladder)"
if ! THYLACINE_VOLPROBE=1 "$REPO_ROOT/tools/test.sh"; then
    echo "==> FAIL: the boot did not reach the banner (see $LOG)"
    exit 1
fi

# The guest-side witness: joey's OK line is printed only after the probe's fd-1
# PASS marker is seen (pouch_smoke_one_caps content-check), so this asserts BOTH
# the positive round-trip and the user-principal deny arm passed.
if ! grep -q 'joey: nocturne-vol-probe OK' "$LOG"; then
    echo "==> FAIL: no 'joey: nocturne-vol-probe OK' line (the sink-volume gate witness)"
    grep -n -E 'nocturne-vol-probe|NOCTURNE-VOL|volume' "$LOG" | tail -20 || true
    exit 1
fi

# Corroborate the DENY arm explicitly: the child's own marker must be the OK
# (refused) form, never the FAIL (bypassed) form. A grep for the FAIL string is
# the negative control -- its presence is a hard failure even if the OK line is
# also somehow present.
if grep -q 'NOCTURNE-VOL-DENY FAIL' "$LOG"; then
    echo "==> FAIL: the user-principal deny arm reported the gate was BYPASSED"
    grep -n -E 'NOCTURNE-VOL-DENY' "$LOG" || true
    exit 1
fi
if ! grep -q 'NOCTURNE-VOL-DENY OK' "$LOG"; then
    echo "==> FAIL: no 'NOCTURNE-VOL-DENY OK' -- the deny arm did not run (a control must fire)"
    exit 1
fi

echo "==> PASS: nocturne-vol-probe -- volume grammar round-trip + SYSTEM allow + user-principal deny (EPERM)"
