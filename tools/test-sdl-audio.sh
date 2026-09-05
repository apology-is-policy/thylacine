#!/usr/bin/env bash
# tools/test-sdl-audio.sh -- the Nocturne SDL-backend wav witness (N-2a-2,
# docs/NOCTURNE.md section 6.5).
#
# Boots the default build ONCE with QEMU's `wav` backend capturing what the
# guest plays AND the thylacine.sdlaudio boot arg, which makes joey run
# /sdl-audio-probe INSTEAD of the N-1 /nocturne-probe (usr/joey/joey.c). The
# probe opens SDL audio through the real SDL_OpenAudioDevice path -- driver
# "thylacine" (usr/ports/sdl2/thylacine/SDL_thylacineaudio.c) -> a private
# Nocturne voice over a fresh /srv/nocturne connection -- and streams a
# 1 kHz + 2 kHz chord from its callback. The capture is a CLEAN SDL tone (the
# N-1 probe stood down), so the SAME chord verdict as test-audio.sh applies:
# BOTH tones in the SAME windows, one contiguous span, a silent tail.
#
# Why the chord (not a single tone): it reuses the audited audio-verdict.py
# --chord path AND proves the SDL byte path carries complex PCM, not just a
# pure sine. This does NOT re-prove nocturned's mixer (that is N-1); the app
# pre-mixes both tones into one voice. The witness here is "SDL audio reaches
# the host through the thylacine backend."
#
# The verdict's own selftest runs FIRST -- a checker that cannot fail proves
# nothing (#245). Needs no host audio hardware. Not a multi-boot; like every
# boot gate it must not run beside another VM from this tree (#224).
#
# Usage:
#   tools/test-sdl-audio.sh            -- selftest + one boot + verdict
#   tools/test-sdl-audio.sh --no-boot  -- judge the existing capture only
#   THYLACINE_AUDIO_WAV=path           -- capture path (default build/sdl-audio-tone.wav)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WAV="${THYLACINE_AUDIO_WAV:-$REPO_ROOT/build/sdl-audio-tone.wav}"
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
    echo "==> booting with THYLACINE_AUDIODEV=wav THYLACINE_SDLAUDIO=1 -> $WAV"
    if ! THYLACINE_AUDIODEV=wav THYLACINE_AUDIO_WAV="$WAV" THYLACINE_SDLAUDIO=1 \
            "$REPO_ROOT/tools/test.sh"; then
        echo "==> FAIL: the boot did not reach the banner (see build/test-boot.log)"
        exit 1
    fi
    if ! grep -q 'joey: sdl-audio-probe OK' "$REPO_ROOT/build/test-boot.log"; then
        echo "==> FAIL: no 'joey: sdl-audio-probe OK' line (the guest-side half)"
        grep -n -E 'sdl-audio-probe|nocturne|virtio-snd' "$REPO_ROOT/build/test-boot.log" | tail -20 || true
        exit 1
    fi
fi

if [[ ! -s "$WAV" ]]; then
    echo "==> FAIL: no capture at $WAV"
    exit 1
fi
echo "==> judging $WAV ($(stat -f %z "$WAV" 2>/dev/null || stat -c %s "$WAV") bytes) -- chord via the SDL path"
python3 "$REPO_ROOT/tools/audio-verdict.py" "$WAV" --chord --expect 1000,2000
