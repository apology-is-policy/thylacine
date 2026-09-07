#!/usr/bin/env bash
# tools/test-nocturne-capture.sh -- the Nocturne N-3c-2 device-capture (`source`)
# authority witness (docs/NOCTURNE.md 6.4/6.8, I-46).
#
# Boots the default build ONCE with THYLACINE_CAPTUREPROBE=1, which (a) forces the
# virtio-sound device to streams=2 (QEMU exposes stream 1 as a D_INPUT capture
# stream there) and (b) makes joey run /nocturne-capture-probe after the
# /dev/nocturne mount. The device source is the mic / line-in -- reading it is
# recording, so it is eavesdropping unless the reader holds the sink authority. The
# source lives ONLY on the per-connection /srv/nocturne-ctl post (peer = the
# reader), never on the shared /dev/nocturne mount (peer = the mounter=SYSTEM, the
# N-3a-2 F1 lesson). The probe proves it:
#
#   POSITIVE (the probe itself, SYSTEM): opening /srv/nocturne-ctl/source is
#   ACCEPTED, a SECOND concurrent open is EBUSY (single-reader), and -- the
#   DETERMINISTIC COUNT -- the driver's periods-captured CLIMBS while the source is
#   held. The witness runs under audiodev=none, so the captured CONTENT is silence;
#   asserting non-silence would be satisfied by a BROKEN RX path too (the broken-
#   fixture trap), so the probe asserts the COUNT, never the content.
#
#   NEGATIVE: /dev/nocturne/source does NOT EXIST (no eavesdrop via the shared
#   mount), and a user-principal child (SPAWN_IDENTITY_SET) is DENIED the source
#   (EPERM) -- not SYSTEM, not the console-owner session, no CAP_AUDIO_GRAPH.
#   Without these a gate that refused every open would pass the denials alone (a
#   control must prove discrimination).
#
# No wav capture -- the null backend clocks the capture stream with silence, which
# is all the COUNT witness needs -- so it needs no host capture backend. Not a
# multi-boot; like every boot gate it must not run beside another VM from this tree
# (#224).
#
# Usage: tools/test-nocturne-capture.sh   -- one boot + the guest-side PASS lines

set -euo pipefail
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOG="$REPO_ROOT/build/test-boot.log"

echo "==> booting with THYLACINE_CAPTUREPROBE=1 (streams=2 + /nocturne-capture-probe in the boot ladder)"
if ! THYLACINE_CAPTUREPROBE=1 "$REPO_ROOT/tools/test.sh"; then
    echo "==> FAIL: the boot did not reach the banner (see $LOG)"
    exit 1
fi

# The guest-side witness: joey's OK line is printed only after the probe's fd-1
# PASS marker is seen (pouch_smoke_one_caps content-check), so this asserts the
# positive capture-count arm AND the user-principal deny arm both passed.
if ! grep -q 'joey: nocturne-capture-probe OK' "$LOG"; then
    echo "==> FAIL: no 'joey: nocturne-capture-probe OK' line (the device-capture gate witness)"
    grep -n -E 'nocturne-capture-probe|NOCTURNE-CAPTURE|capture|stream 1' "$LOG" | tail -25 || true
    exit 1
fi

# Corroborate the DENY arm explicitly: the child's own marker must be the OK
# (refused) form, never the FAIL (bypassed) form. A grep for the FAIL string is
# the negative control -- its presence is a hard failure even if the OK line is
# also somehow present.
if grep -q 'NOCTURNE-CAPTURE-DENY FAIL' "$LOG"; then
    echo "==> FAIL: the user-principal deny arm reported the source gate was BYPASSED"
    grep -n -E 'NOCTURNE-CAPTURE-DENY' "$LOG" || true
    exit 1
fi
if ! grep -q 'NOCTURNE-CAPTURE-DENY OK' "$LOG"; then
    echo "==> FAIL: no 'NOCTURNE-CAPTURE-DENY OK' -- the deny arm did not run (a control must fire)"
    exit 1
fi

echo "==> PASS: nocturne-capture-probe -- SYSTEM source open + single-reader + periods-captured climbed + mount absent/user source deny (EPERM)"
