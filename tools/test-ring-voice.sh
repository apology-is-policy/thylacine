#!/usr/bin/env bash
# tools/test-ring-voice.sh -- the Nocturne N-2b-1 zero-copy ring SUBSTRATE
# witness (docs/NOCTURNE.md section 6.5).
#
# Boots the default build ONCE with thylacine.ringprobe, which makes joey run
# /ring-voice-probe after the /dev/nocturne mount: mint a voice, open its `data`
# leaf THROUGH the mount (SYS_WEFT_MAP needs a dev9p fd), SYS_WEFT_MAP it, and
# validate the shared Weft ring geometry (WEFT_MAGIC, K slots, a K-period
# payload). The probe also runs two controls -- a second map is idempotent, and
# voice 0's data map is REFUSED -- so a PASS proves discrimination, not mere
# detection (#245).
#
# No audio: this is the substrate witness. The period producer/consumer protocol
# over the ring and its wav witness are N-2b-2. Needs no host audio hardware.
# Not a multi-boot; like every boot gate it must not run beside another VM from
# this tree (#224).
#
# Usage: tools/test-ring-voice.sh    -- one boot + the guest-side PASS assertion

set -euo pipefail
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

echo "==> booting with THYLACINE_RINGPROBE=1 (/ring-voice-probe in the boot ladder)"
if ! THYLACINE_RINGPROBE=1 "$REPO_ROOT/tools/test.sh"; then
    echo "==> FAIL: the boot did not reach the banner (see build/test-boot.log)"
    exit 1
fi
if ! grep -q 'joey: ring-voice-probe OK' "$REPO_ROOT/build/test-boot.log"; then
    echo "==> FAIL: no 'joey: ring-voice-probe OK' line (the guest-side witness)"
    grep -n -E 'ring-voice-probe|RING-VOICE|weft|nocturne' "$REPO_ROOT/build/test-boot.log" | tail -20 || true
    exit 1
fi
echo "==> PASS: ring-voice-probe mapped the Weft ring + validated geometry + ran both controls"
