#!/usr/bin/env bash
# tools/build-everything.sh
#
# Build a COMPLETE, QUICK-TO-BOOT Thylacine: every bake chunk we built, with the
# boot tests dropped so the image boots straight to the login getty.
#
#   "everything, every chunk"   -> every bake toggle ON: /goroot, /clade + /storm,
#                                  /chase-w2, the viv + Alpine bundles, /quake.
#
# Optional chunks need external inputs (a Go fork, an LLVM fork, an Alpine
# rootfs). An ABSENT input SKIPS that chunk with a clear warning; the rest of the
# image still builds. Full detail + the mechanism: docs/BUILD-HARNESS.md.
#
# NOTE ON SPEED: "quick to boot" is about BOOT time, not BUILD time. This build
# is the SLOW one -- especially /clade, a full LLVM cross-build the first time.
# The resulting IMAGE boots fast.
#
# Usage:
#   tools/build-everything.sh [extra build.sh flags...]   # e.g. --release --kaslr
#   SKIP_CLADE=1 tools/build-everything.sh                # skip the slow LLVM build
#
# After it finishes:  tools/run-vm.sh   (or: make run)

set -euo pipefail
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

log() { printf '\n== build-everything: %s ==\n' "$*"; }

# --- detect the optional-chunk inputs ----------------------------------------
GOFORK="${GOFORK:-$HOME/projects/go-thylacine}"
LLVMFORK="${LLVMFORK:-$HOME/projects/llvm-thylacine}"
alpine_tar="${THYLACINE_ALPINE_TARBALL:-$(ls build/cache/alpine-minirootfs-*-aarch64.tar.gz 2>/dev/null | head -1 || true)}"
bb_apk="${THYLACINE_BUSYBOX_STATIC_APK:-$(ls build/cache/busybox-static-*.apk 2>/dev/null | head -1 || true)}"

have_go=0;   [[ -x "$GOFORK/bin/go" ]] && have_go=1
have_clade=0
[[ -f build/clade/llvm-build/bin/llvm ]] && have_clade=1              # already cross-built
[[ $have_clade = 0 && -f "$LLVMFORK/llvm/CMakeLists.txt" ]] && have_clade=1  # buildable from the fork
have_alpine=0; [[ -n "$alpine_tar" && -n "$bb_apk" ]] && have_alpine=1

log "optional-chunk inputs"
printf '  /goroot           : %s\n' "$([[ $have_go     = 1 ]] && echo "YES  ($GOFORK)"                    || echo 'no   (set GOFORK -> chunk skipped)')"
printf '  /clade + /storm   : %s\n' "$([[ $have_clade  = 1 ]] && echo 'YES'                              || echo 'no   (set LLVMFORK or prebuild build/clade -> chunk skipped)')"
printf '  /vivarium/alpine* : %s\n' "$([[ $have_alpine = 1 ]] && echo "YES  ($(basename "$alpine_tar"))"  || echo 'no   (drop an Alpine rootfs + busybox-static apk in build/cache/ -> bundle skipped)')"
printf '  /quake            : YES  (build_tyrquake auto-fetches the shareware pak; needs network)\n'
printf '  /vivarium (probe) : YES  (always staged)\n'

# --- stage the chunks that `build.sh all` does NOT stage on its own -----------
# /quake: build_all never runs build_tyrquake. Stage it now (auto-fetches the
# ~9 MB shareware pak). Tolerant: a fetch failure just leaves /quake absent.
log "staging /quake (tyrquake)"
tools/build.sh tyrquake || echo "  WARN: quake staging failed (no network/unzip?) -- /quake will be absent"

# /clade: the DEVICE LLVM toolchain -- the one genuinely slow chunk (a full
# cross-build the first time; incremental after). build_all stages /storm and
# bakes /clade ONLY if build/clade/stage/bin already exists, so build + stage it
# BEFORE the final `all`. Both build.sh targets skip gracefully with no fork.
if [[ "${SKIP_CLADE:-0}" = 1 ]]; then
    log "SKIP_CLADE=1 -> /clade + /storm deliberately skipped"
elif [[ $have_clade = 1 ]]; then
    log "building + staging /clade (SLOW the first time -- a full LLVM cross-build)"
    tools/build.sh clade       || echo "  WARN: clade build failed -- /clade + /storm will be absent"
    tools/build.sh stage-clade || echo "  WARN: clade staging failed -- /clade will be absent"
else
    log "SKIP /clade + /storm (no LLVMFORK and no prebuilt build/clade/llvm-build)"
fi

# --- the one complete, quick-to-boot build -----------------------------------
# Every toggle ON; --production drops the test suite + the boot-probe ladder.
# This single `all` run also stages /goroot + /storm + /vivarium internally and
# bakes the pool ONCE with every staged chunk present. The final SUMMARY block
# reports exactly what was baked vs skipped -- read it.
log "the complete --config everything build (no boot tests) -- the long part"
export THYLACINE_BAKE_GOROOT=1
export THYLACINE_CHASE_W2=1
# Bake /clade + /storm IFF clade is genuinely staged THIS run. SKIP_CLADE, an
# absent LLVMFORK, or a failed clade build all leave build/clade/stage/bin
# absent -> BAKE_CLADE=0. This also stops a STALE /storm stage from a prior build
# baking WITHOUT its /clade -- the "storm without clade" state #154 calls
# boot-fatal (harmless only because --production drops joey's storm gate).
if [[ -d build/clade/stage/bin ]]; then
    export THYLACINE_BAKE_CLADE=1
else
    export THYLACINE_BAKE_CLADE=0
fi
[[ -n "$alpine_tar" ]] && export THYLACINE_ALPINE_TARBALL="$alpine_tar"
[[ -n "$bb_apk"     ]] && export THYLACINE_BUSYBOX_STATIC_APK="$bb_apk"

tools/build.sh all --config everything "$@"

log "DONE -- complete image built, boot tests OFF"
echo "  Boot it:  tools/run-vm.sh        (or: make run)"
echo "  The build SUMMARY above lists exactly which chunks were baked vs skipped."
