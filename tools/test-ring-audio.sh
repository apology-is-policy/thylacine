#!/usr/bin/env bash
# tools/test-ring-audio.sh -- the Nocturne N-2b-2a zero-copy ring AUDIO witness
# (docs/NOCTURNE.md section 6.5).
#
# Boots the default build ONCE with QEMU's `wav` backend capturing what the guest
# plays AND thylacine.ringprobe, which makes joey run /ring-voice-probe INSTEAD of
# the byte /nocturne-probe (the two are exclusive -- one wav, one chord span). The
# probe streams a 1 kHz + 2 kHz chord THROUGH the zero-copy Weft rings of two
# voices, nocturned's mixer sums them, and the sink plays them. tools/audio-verdict.py
# --chord judges the capture FILE (never a guest log line, #186): BOTH tones in
# the SAME 20 ms windows, one contiguous span, a silent tail. A sequential or
# single-tone capture FAILS the chord check.
#
# Because the byte probe stands down under thylacine.ringprobe, the ONLY audio in
# the capture came through the ring -- that exclusivity is the control that this
# witnesses the zero-copy DATA path, not the byte path.
#
# The verdict's own selftest runs FIRST (chord PASS; sequential / single / silent
# / gap / noisy-tail all FAIL) -- a checker that cannot fail proves nothing (#245).
#
# Needs no host audio hardware; runs on the mac and on thyla-pi. Not a multi-boot:
# one capture is one verdict. Uses tools/test.sh for the boot, so like every boot
# gate it must not run beside another VM from this tree (#224).
#
# Usage:
#   tools/test-ring-audio.sh              -- selftest + one boot + verdict
#   tools/test-ring-audio.sh --no-boot    -- judge the existing capture only
#   THYLACINE_RING_WAV=path               -- capture path (default build/ring-chord.wav)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WAV="${THYLACINE_RING_WAV:-$REPO_ROOT/build/ring-chord.wav}"
boot=1
for arg in "$@"; do
    case "$arg" in
        --no-boot) boot=0 ;;
        *) echo "Usage: $0 [--no-boot]" >&2; exit 2 ;;
    esac
done

echo "==> audio-verdict selftest (synthetic discrimination)"
python3 "$REPO_ROOT/tools/audio-verdict.py" --selftest

if (( boot )); then
    rm -f "$WAV"
    echo "==> booting with THYLACINE_AUDIODEV=wav + THYLACINE_RINGPROBE=1 -> $WAV"
    if ! THYLACINE_AUDIODEV=wav THYLACINE_AUDIO_WAV="$WAV" THYLACINE_RINGPROBE=1 "$REPO_ROOT/tools/test.sh"; then
        echo "==> FAIL: the boot did not reach the banner (see build/test-boot.log)"
        exit 1
    fi
    if ! grep -q 'joey: ring-voice-probe OK' "$REPO_ROOT/build/test-boot.log"; then
        echo "==> FAIL: the boot log carries no 'joey: ring-voice-probe OK' line (the guest-side half)"
        grep -n -E 'ring-voice-probe|RING-VOICE|weft|nocturne|virtio-snd' "$REPO_ROOT/build/test-boot.log" | tail -20 || true
        exit 1
    fi
fi

if [[ ! -s "$WAV" ]]; then
    echo "==> FAIL: no capture at $WAV"
    exit 1
fi
echo "==> judging $WAV ($(stat -f %z "$WAV" 2>/dev/null || stat -c %s "$WAV") bytes) -- chord (ring path + mixing)"
python3 "$REPO_ROOT/tools/audio-verdict.py" "$WAV" --chord --expect 1000,2000
