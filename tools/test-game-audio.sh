#!/usr/bin/env bash
# tools/test-game-audio.sh -- the W-4 game-audio witness (Nocturne N-2a-3,
# docs/NOCTURNE.md section 6.5): a PORTED GAME's sound reaches the host.
#
# Runs ONE interactive scenario (default ls-gfx-quake: software tyr-quake's
# +timedemo demo1, ~17 s of gunfire and grunts) through tools/test-interactive.sh
# with QEMU's `wav` backend capturing what the guest plays, then judges the
# capture with tools/audio-verdict.py --music: >= 2 s of sound that is neither
# noise (spectral flatness) nor a stationary buzz (the dominant bin moves).
#
# The boot's own audio probe is DECLINED for this run (thylacine.noaudioprobe,
# usr/joey/joey.c) so the wav carries ONLY what the session played. That is not
# a nicety: QEMU's wav backend appends only while the guest stream runs, so a
# boot-time chord and the game are ADJACENT in the file and no skip/tail window
# can tell them apart. The verdict's selftest runs first (a checker that cannot
# fail proves nothing, #245); the scenario's own gates (the guest-side "Sound
# Initialized" line, the timedemo) stay in force -- this adds the CAPTURE half.
#
# One scenario, one job, one boot per attempt (the harness's retry re-boots and
# QEMU re-creates the wav, so the judged file is the passing attempt's). Needs
# `expect` + HVF (the ls-gfx posture); must not run beside another VM from this
# tree (#224).
#
# Usage:
#   tools/test-game-audio.sh                    -- ls-gfx-quake
#   tools/test-game-audio.sh ls-gfx-dosbox-duke3d  -- another scenario with sound
#   tools/test-game-audio.sh --no-boot [scen]   -- judge the existing capture only

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
boot=1
scen="ls-gfx-quake"
for arg in "$@"; do
    case "$arg" in
        --no-boot) boot=0 ;;
        -*) echo "Usage: $0 [--no-boot] [scenario]" >&2; exit 2 ;;
        *) scen="$arg" ;;
    esac
done
WAV="${THYLACINE_AUDIO_WAV:-$REPO_ROOT/build/game-audio-$scen.wav}"

echo "==> audio-verdict selftest (synthetic discrimination)"
python3 "$REPO_ROOT/tools/audio-verdict.py" --selftest >/dev/null || {
    echo "==> FAIL: the verdict's own selftest does not discriminate"; exit 1; }

if (( boot )); then
    rm -f "$WAV"
    echo "==> $scen with THYLACINE_AUDIODEV=wav THYLACINE_NOAUDIOPROBE=1 -> $WAV"
    if ! THYLACINE_AUDIODEV=wav THYLACINE_AUDIO_WAV="$WAV" THYLACINE_NOAUDIOPROBE=1 \
            LS_CI_JOBS=1 "$REPO_ROOT/tools/test-interactive.sh" "$scen"; then
        echo "==> FAIL: the scenario itself failed (see build/ls-ci-$scen.log)"
        exit 1
    fi
    if ! grep -q 'joey: audio probe DECLINED' "$REPO_ROOT/build/ls-ci-$scen.log"; then
        echo "==> FAIL: the boot did not decline its audio probe -- the wav is not the game's alone"
        exit 1
    fi
    # A sanity check that Quake opened audio at all -- the RAW guest line in
    # the transcript (not the lc_step message, which goes to the .steps file).
    # This does NOT prove which driver: the dummy fallback also prints 48000.
    # The DRIVER proof is the wav below -- dummy plays to nowhere, so a dummy
    # capture is silent and --music fails "not enough audio". Here the device
    # is present (AUDIODEV=wav), so it is thylacine, and the wav confirms it.
    if [[ "$scen" == ls-gfx-quake || "$scen" == ls-gfx-play || "$scen" == ls-gfx-glquake ]] &&
       ! grep -q 'Sound Initialized: 16 bits @' "$REPO_ROOT/build/ls-ci-$scen.log"; then
        echo "==> FAIL: no 'Sound Initialized' line in the transcript -- audio init did not run"
        exit 1
    fi
fi

if [[ ! -s "$WAV" ]]; then
    echo "==> FAIL: no capture at $WAV (did the game open audio at all?)"
    exit 1
fi
echo "==> judging $WAV ($(stat -f %z "$WAV" 2>/dev/null || stat -c %s "$WAV") bytes) -- game audio"
python3 "$REPO_ROOT/tools/audio-verdict.py" "$WAV" --music
