#!/usr/bin/env bash
# gl-host-probe.sh -- can this Linux host run QEMU with virgl?
#
# The verification ladder from docs/GPU-DESIGN.md section 9.1, run on the
# prospective GL host (the Parallels Debian VM, or a GCP runner). Rung 6 is the
# one that decides the Warp arc; rungs 1-5 exist so that a failure at 6
# names its own cause.
#
# FAILS CLOSED, deliberately. A rung whose evidence cannot be read reports
# UNKNOWN, and any UNKNOWN makes the overall verdict INCONCLUSIVE -- never PASS.
# A gate that cannot parse its log must not pass; the alternative is a probe
# that certifies a host it never actually tested.
#
# Exit: 0 = PASS (rung 6 positively verified), 1 = FAIL, 2 = INCONCLUSIVE.

set -u

QEMU=${QEMU:-qemu-system-aarch64}
PASS=0; FAIL=0; UNKNOWN=0
DECISIVE=""            # the rung-6 verdict; empty until set

r_pass() { printf '  \033[32mPASS\033[0m  %s\n' "$1"; PASS=$((PASS+1)); }
r_fail() { printf '  \033[31mFAIL\033[0m  %s\n' "$1"; FAIL=$((FAIL+1)); }
r_unk()  { printf '  \033[33m????\033[0m  %s\n' "$1"; UNKNOWN=$((UNKNOWN+1)); }
r_info() { printf '        %s\n' "$1"; }
rung()   { printf '\n[%s] %s\n' "$1" "$2"; }

printf '=== gl-host-probe: %s ===\n' "$(uname -srm)"
printf 'qemu: %s\n' "$($QEMU --version 2>/dev/null | head -1 || echo 'NOT FOUND')"

# Rungs 6 and 7 need a bounded run: a QEMU that STAYS UP is the pass, so we must
# kill it and read the kill as success. Without a timeout(1) those rungs cannot
# be run at all -- which is UNKNOWN (we learned nothing), never FAIL (the host
# failed a capability). Conflating the two is how a probe reports a substrate
# verdict it never measured. macOS has no timeout(1); coreutils gtimeout is the
# usual stand-in.
TIMEOUT=""
for t in timeout gtimeout; do
    if command -v "$t" >/dev/null 2>&1; then TIMEOUT=$t; break; fi
done
[ -z "$TIMEOUT" ] && printf 'timeout(1): NOT FOUND -- rungs 6+7 will report UNKNOWN\n'

# ---------------------------------------------------------------- rung 1
rung 1 "DRM render node"
if ! [ -d /dev/dri ]; then
    r_fail "/dev/dri does not exist -- no DRM at all"
elif nodes=$(ls /dev/dri/renderD* 2>/dev/null) && [ -n "$nodes" ]; then
    r_pass "render node(s): $(echo $nodes | tr '\n' ' ')"
    for n in $nodes; do
        drv=$(basename "$(readlink -f /sys/class/drm/"$(basename "$n")"/device/driver 2>/dev/null)" 2>/dev/null)
        [ -n "$drv" ] && r_info "$n -> driver '$drv'"
    done
else
    r_fail "/dev/dri exists but holds no renderD* -- QEMU egl-headless cannot work"
    r_info "check: prlctl set <vm> --video-adapter-type virtio; lsmod | grep virtio_gpu"
fi
# Existence is not access. logind grants the device ACL to the ACTIVE LOCAL SEAT;
# an ssh session gets nothing, so a node that is plainly present is still
# unopenable and QEMU reports it as "no drm render node available" -- a message
# that sends you hunting for a missing device rather than a missing group.
if [ -e /dev/dri/renderD128 ]; then
    if (exec 3<>/dev/dri/renderD128) 2>/dev/null; then
        r_pass "renderD128 is OPENABLE by $(id -un)"
    else
        r_fail "renderD128 exists but $(id -un) CANNOT OPEN IT (groups: $(id -nG))"
        r_info "fix: sudo usermod -aG render $(id -un), then a FRESH login (ssh -O exit first)"
    fi
fi

# ---------------------------------------------------------------- rung 2
rung 2 "GBM + EGL libraries"
for lib in libgbm.so.1 libEGL.so.1; do
    if ldconfig -p 2>/dev/null | grep -q "$lib"; then
        r_pass "$lib present"
    elif find /usr/lib /lib -name "$lib*" 2>/dev/null | grep -q .; then
        r_pass "$lib present (found on disk; ldconfig cache silent)"
    else
        r_fail "$lib missing -- install libgbm1 / libegl-mesa0"
    fi
done

# ---------------------------------------------------------------- rung 3
rung 3 "host GL stack initialises"
if command -v glxinfo >/dev/null 2>&1; then
    gl=$(glxinfo -B 2>/dev/null | grep -iE "OpenGL renderer|OpenGL version" | head -2)
    if [ -n "$gl" ]; then
        r_pass "host GL live"
        echo "$gl" | while IFS= read -r l; do r_info "$l"; done
    else
        r_unk "glxinfo produced no renderer line (no display? try under X/Wayland)"
    fi
else
    r_unk "glxinfo not installed (mesa-utils) -- cannot confirm host GL"
fi

# ---------------------------------------------------------------- rung 4
rung 4 "virtio-gpu-gl compiled into QEMU"
if ! command -v "$QEMU" >/dev/null 2>&1; then
    r_fail "$QEMU not found"
else
    devs=$("$QEMU" -device help 2>&1)
    if [ -z "$devs" ]; then
        r_unk "'-device help' produced no output -- cannot determine"
    elif echo "$devs" | grep -q "virtio-gpu-gl"; then
        r_pass "virtio-gpu-gl present"
        echo "$devs" | grep -oE '"?virtio-gpu-gl[a-z-]*"?' | sort -u | while IFS= read -r d; do r_info "$d"; done
    else
        r_fail "virtio-gpu-gl ABSENT -- this QEMU was built without virglrenderer"
        r_info "control: virtio-gpu-pci is $(echo "$devs" | grep -q virtio-gpu-pci && echo present || echo 'ALSO absent -- suspect the -device help parse')"
    fi
fi

# ---------------------------------------------------------------- rung 5
rung 5 "egl-headless display backend"
if ! command -v "$QEMU" >/dev/null 2>&1; then
    r_unk "no qemu -- skipped"
else
    disp=$("$QEMU" -display help 2>&1)
    if [ -z "$disp" ]; then
        r_unk "'-display help' produced no output -- cannot determine"
    elif echo "$disp" | grep -qw "egl-headless"; then
        r_pass "egl-headless available"
        r_info "all backends: $(echo "$disp" | sed -n '2,/^$/p' | tr '\n' ' ')"
    else
        r_fail "egl-headless ABSENT"
        r_info "available: $(echo "$disp" | sed -n '2,/^$/p' | tr '\n' ' ')"
    fi
fi

# ---------------------------------------------------------------- rung 6
rung 6 "DECISIVE -- QEMU realises virtio-gpu-gl on egl-headless"
if ! command -v "$QEMU" >/dev/null 2>&1; then
    r_unk "no qemu -- cannot run the decisive test"; DECISIVE=unknown
elif [ -z "$TIMEOUT" ]; then
    r_unk "no timeout(1)/gtimeout -- the decisive test CANNOT BE RUN here"
    r_info "this is 'not measured', NOT 'the host failed'; install coreutils or run on the target"
    DECISIVE=unknown
else
    out=$($TIMEOUT 8 "$QEMU" -M virt -cpu max -m 256 -nodefaults -no-user-config \
            -display egl-headless -device virtio-gpu-gl -S \
            -monitor none -serial none 2>&1)
    rc=$?
    # timeout(1) returns 124 when it had to kill a still-running process: QEMU
    # sat there stopped with the device realised, which is exactly the pass.
    if [ $rc -eq 124 ]; then
        r_pass "QEMU realised virtio-gpu-gl and stayed up -- THE ARC IS UNBLOCKED"
        DECISIVE=pass
    elif echo "$out" | grep -q "not a valid device model name"; then
        r_fail "device model missing (see rung 4)"; DECISIVE=fail
    elif echo "$out" | grep -q "qemu-system-modules-opengl"; then
        r_fail "GL support is a SEPARATE Debian package"; DECISIVE=fail
        r_info "fix: sudo apt-get install qemu-system-modules-opengl"
    elif echo "$out" | grep -q "does not accept value 'egl-headless'"; then
        r_fail "egl-headless not compiled into this QEMU (see rung 5)"; DECISIVE=fail
    elif echo "$out" | grep -q "does not have OpenGL support enabled"; then
        r_fail "display backend has no GL -- try sdl,gl=on / gtk,gl=on"; DECISIVE=fail
    elif echo "$out" | grep -q "no drm render node available"; then
        r_fail "no DRM render node (see rung 1) -- THE blocker"; DECISIVE=fail
    elif echo "$out" | grep -q "egl: not available on this platform"; then
        r_fail "EGL unavailable (see rung 2)"; DECISIVE=fail
    elif [ -z "$out" ] && [ $rc -eq 0 ]; then
        r_unk "QEMU exited 0 with no output -- unexpected; treat as unproven"
        DECISIVE=unknown
    else
        r_unk "unrecognised outcome (rc=$rc) -- read it and classify by hand"
        DECISIVE=unknown
        echo "$out" | head -12 | while IFS= read -r l; do r_info "| $l"; done
    fi
fi

# ---------------------------------------------------------------- rung 7
rung 7 "interactive path (gtk,gl=on) -- for watching the game"
if ! command -v "$QEMU" >/dev/null 2>&1; then
    r_unk "no qemu -- skipped"
elif [ -z "$TIMEOUT" ]; then
    r_unk "no timeout(1)/gtimeout -- not measured (see rung 6)"
elif [ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
    r_unk "no DISPLAY/WAYLAND_DISPLAY -- rerun inside the desktop session to test"
else
    out=$($TIMEOUT 8 "$QEMU" -M virt -cpu max -m 256 -nodefaults -no-user-config \
            -display gtk,gl=on -device virtio-gpu-gl -S \
            -monitor none -serial none 2>&1)
    rc=$?
    if [ $rc -eq 124 ]; then
        r_pass "gtk,gl=on works -- GLQuake will be watchable"
    else
        r_fail "gtk,gl=on failed (rc=$rc): $(echo "$out" | head -1)"
    fi
fi

# ---------------------------------------------------------------- rung 8
rung 8 "Vulkan ICD (informational -- the Warp-6 Venus leg)"
if command -v vulkaninfo >/dev/null 2>&1; then
    dev=$(vulkaninfo --summary 2>/dev/null | grep -iE "deviceName" | head -2)
    if [ -n "$dev" ]; then
        r_pass "Vulkan present"
        echo "$dev" | while IFS= read -r l; do r_info "$l"; done
    else
        r_unk "vulkaninfo ran but named no device"
    fi
else
    r_unk "vulkaninfo absent (vulkan-tools) -- Venus leg unassessed, not blocking Warp-1"
fi

# ---------------------------------------------------------------- verdict
printf '\n=== verdict ===\n'
printf 'rungs: %d pass, %d fail, %d unknown\n' "$PASS" "$FAIL" "$UNKNOWN"
case "$DECISIVE" in
  pass)
    if [ "$UNKNOWN" -gt 0 ]; then
        printf '\033[32mPASS\033[0m -- rung 6 verified; %d informational rung(s) unknown.\n' "$UNKNOWN"
        printf 'The GL host works. Proceed to Warp-1.\n'
    else
        printf '\033[32mPASS\033[0m -- every rung verified. The GL host works. Proceed to Warp-1.\n'
    fi
    exit 0 ;;
  fail)
    printf '\033[31mFAIL\033[0m -- rung 6 failed for a NAMED reason (above).\n'
    printf 'See docs/GPU-HOST-SETUP.md section 8 for the fix ladder.\n'
    exit 1 ;;
  *)
    printf '\033[33mINCONCLUSIVE\033[0m -- rung 6 was not positively verified.\n'
    printf 'This is NOT a pass. Read the rung-6 output above and classify it by\n'
    printf 'hand before drawing any conclusion about the substrate.\n'
    exit 2 ;;
esac
