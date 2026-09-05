#!/usr/bin/env bash
# tools/test-audio.sh -- the Nocturne wav witness gate (docs/NOCTURNE.md section 7, W-1).
#
# Boots the default build ONCE with QEMU's `wav` audio backend capturing what
# the guest plays, lets joey's boot-probe ladder run /nocturne-probe (0.5 s of
# 1 kHz, 0.5 s of 2 kHz, then silence, written to /dev/nocturne/audio), and
# then judges the capture FILE with tools/audio-verdict.py: pre-tone silence
# (the negative control), 1 kHz dominant, then 2 kHz dominant (the positive
# control: a different tone lands in a different bin), in that order.
#
# The verdict's own selftest runs FIRST (synthetic signals: the signature must
# PASS; the reversed order, silence, a single tone and a noisy prefix must
# FAIL) -- a checker that cannot fail proves nothing (#245).
#
# Needs no host audio hardware; runs on the mac and on thyla-pi. Not a
# multi-boot: one capture is one verdict. Uses tools/test.sh for the boot
# (BOOT_TIMEOUT etc. apply), so like every boot gate it must not run beside
# another VM from this tree (#224).
#
# Usage:
#   tools/test-audio.sh              -- selftest + one boot + verdict
#   tools/test-audio.sh --no-boot    -- judge the existing capture only
#   THYLACINE_AUDIO_WAV=path         -- capture path (default build/audio-tone.wav)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WAV="${THYLACINE_AUDIO_WAV:-$REPO_ROOT/build/audio-tone.wav}"
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
    echo "==> booting with THYLACINE_AUDIODEV=wav -> $WAV"
    if ! THYLACINE_AUDIODEV=wav THYLACINE_AUDIO_WAV="$WAV" "$REPO_ROOT/tools/test.sh"; then
        echo "==> FAIL: the boot did not reach the banner (see build/test-boot.log)"
        exit 1
    fi
    if ! grep -q 'joey: nocturne-probe OK' "$REPO_ROOT/build/test-boot.log"; then
        echo "==> FAIL: the boot log carries no 'joey: nocturne-probe OK' line (the guest-side half)"
        grep -n -E 'nocturne|virtio-snd' "$REPO_ROOT/build/test-boot.log" | tail -20 || true
        exit 1
    fi
fi

if [[ ! -s "$WAV" ]]; then
    echo "==> FAIL: no capture at $WAV"
    exit 1
fi
echo "==> judging $WAV ($(stat -f %z "$WAV" 2>/dev/null || stat -c %s "$WAV") bytes)"
python3 "$REPO_ROOT/tools/audio-verdict.py" "$WAV" --expect 1000,2000
