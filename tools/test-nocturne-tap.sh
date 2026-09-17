#!/usr/bin/env bash
# tools/test-nocturne-tap.sh -- the Nocturne N-3c-1 sink-tap (capture) authority
# witness (docs/NOCTURNE.md 6.4/6.8, I-46).
#
# Boots the default build ONCE with thylacine.tapprobe, which makes joey run
# /nocturne-tap-probe after the /dev/nocturne mount. The sink tap is an EAR on
# the mixed output -- reading it is recording, so it is eavesdropping unless the
# reader holds the sink authority. The tap lives ONLY on the per-connection
# /srv/nocturne-ctl post (peer = the reader), never on the shared /dev/nocturne
# mount (peer = the mounter=SYSTEM -- the N-3a-2 F1 lesson). The probe proves it:
#
#   POSITIVE (the probe itself, SYSTEM): opening /srv/nocturne-ctl/tap is
#   ACCEPTED, a SECOND concurrent open is EBUSY (single-reader), and a played
#   tone is CAPTURED as non-silence on the tap.
#
#   NEGATIVE: a /dev/nocturne/audio READ is REFUSED (no eavesdrop via the shared
#   mount), and a user-principal child (SPAWN_IDENTITY_SET) is DENIED the tap
#   (EPERM) -- not SYSTEM, not the console-owner session, no CAP_AUDIO_GRAPH.
#   Without these a gate that refused every read would pass the denials alone (a
#   control must prove discrimination).
#
# No wav capture -- the tap reads nocturned's software mirror of the mix, not the
# device -- so it needs no host audio backend. Not a multi-boot; like every boot
# gate it must not run beside another VM from this tree (#224).
#
# Usage: tools/test-nocturne-tap.sh   -- one boot + the guest-side PASS lines

set -euo pipefail
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOG="$REPO_ROOT/build/test-boot.log"

echo "==> booting with THYLACINE_TAPPROBE=1 (/nocturne-tap-probe in the boot ladder)"
if ! THYLACINE_TAPPROBE=1 "$REPO_ROOT/tools/test.sh"; then
    echo "==> FAIL: the boot did not reach the banner (see $LOG)"
    exit 1
fi

# The guest-side witness: joey's OK line is printed only after the probe's fd-1
# PASS marker is seen (pouch_smoke_one_caps content-check), so this asserts the
# positive capture arm AND the user-principal deny arm both passed.
if ! grep -q 'joey: nocturne-tap-probe OK' "$LOG"; then
    echo "==> FAIL: no 'joey: nocturne-tap-probe OK' line (the sink-tap gate witness)"
    grep -n -E 'nocturne-tap-probe|NOCTURNE-TAP|tap' "$LOG" | tail -20 || true
    exit 1
fi

# Corroborate the DENY arm explicitly: the child's own marker must be the OK
# (refused) form, never the FAIL (bypassed) form. A grep for the FAIL string is
# the negative control -- its presence is a hard failure even if the OK line is
# also somehow present.
if grep -q 'NOCTURNE-TAP-DENY FAIL' "$LOG"; then
    echo "==> FAIL: the user-principal deny arm reported the tap gate was BYPASSED"
    grep -n -E 'NOCTURNE-TAP-DENY' "$LOG" || true
    exit 1
fi
if ! grep -q 'NOCTURNE-TAP-DENY OK' "$LOG"; then
    echo "==> FAIL: no 'NOCTURNE-TAP-DENY OK' -- the deny arm did not run (a control must fire)"
    exit 1
fi

echo "==> PASS: nocturne-tap-probe -- SYSTEM tap capture + single-reader + mount read/user tap deny (EPERM)"
