#!/usr/bin/env bash
# tools/test.sh — Thylacine integration-test harness.
#
# Per TOOLING.md §5: pre-Utopia (Phases 1-4), the agentic loop verifies boot
# success by pattern-matching UART output for the canonical strings. Post-
# Utopia, this script grows into a structured-result protocol with the
# `thylacine_run` shell wrapper.
#
# Pass = `Thylacine boot OK` banner observed within timeout. Fail = no banner,
# or `EXTINCTION:` prefix observed (kernel ELE — Extinction Level Event;
# named for the thylacine's own fate when the kernel dies), or QEMU crashes.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"

# P1-I: optional --sanitize=ubsan flag selects the alternate build dir
# so the production build's CMake cache isn't clobbered.
sanitize=""
extra_args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --sanitize=*)
            sanitize="${1#--sanitize=}"
            shift
            ;;
        *)
            extra_args+=("$1")
            shift
            ;;
    esac
done

case "$sanitize" in
    "")              KERNEL_BUILD="$BUILD_DIR/kernel" ;;
    ubsan|undefined) KERNEL_BUILD="$BUILD_DIR/kernel-undefined" ;;
    *)
        echo "Unknown --sanitize value: $sanitize (valid: ubsan/undefined)" >&2
        exit 1
        ;;
esac

KERNEL_ELF="$KERNEL_BUILD/thylacine.elf"
LOG_FILE="$BUILD_DIR/test-boot.log"

# Stage 6: with the Go GOROOT baked by default, joey's go4c on-device
# compile+link probe rides every boot, so a baked build's default timeout is
# the bake pipeline's 300 s. Keyed on the staged tree (build_go_goroot leaves
# it iff the pool was baked); an explicit BOOT_TIMEOUT always wins.
if [[ -d "$BUILD_DIR/go/goroot" ]]; then
    BOOT_TIMEOUT="${BOOT_TIMEOUT:-300}"
else
    BOOT_TIMEOUT="${BOOT_TIMEOUT:-90}"
fi
                                            # seconds -- wallclock max,
                                            # not a fixed wait. test.sh
                                            # exits early on banner or
                                            # extinction (polls log every
                                            # 0.1 s).
                                            #
                                            # Boot-time variance (deep-dived
                                            # 2026-05-30; see DEBUGGING-
                                            # PLAYBOOK 6.12): the idle-host
                                            # distribution is BIMODAL (~19-26 s
                                            # "fast" | ~33-37 s "slow", clean
                                            # ~7 s gap). PROVEN cause = macOS
                                            # placing QEMU's 4 TCG vCPU
                                            # threads across the Apple-Silicon
                                            # P-cores vs E-cores (an E-core in
                                            # the mix drags the whole
                                            # synchronized guest ~2.5x). NOT
                                            # stratumd, NOT pool RNG. Proven
                                            # by: -smp 1 -> 0.39 s spread;
                                            # taskpolicy -b (force E) -> 170-
                                            # 220 s; default -smp 4 ->
                                            # bimodal. The constant ~10 s long
                                            # pole is the irq-bench kernel-
                                            # side busy-spin (NOT the
                                            # variance). 90 s default clears
                                            # the slow mode + moderate
                                            # concurrent host load (the
                                            # original >45 s timeouts were
                                            # slow-mode + a concurrent build/
                                            # agent). HVF -- the default accel
                                            # since W3.5 -- boots far faster;
                                            # the 90 s bound is sized for the
                                            # slower TCG compat run.
                                            # THYLACINE_TEST_CPUS=1 is
                                            # fast + has no boot-time variance,
                                            # but DO NOT rely on it for CI yet:
                                            # at -smp 1 joey exits non-zero in
                                            # ~45% of boots (a separate smp1-
                                            # specific bug, task #791) AND it
                                            # loses in-kernel SMP coverage.
                                            # macOS has no upward "pin to
                                            # P-cores" knob (taskpolicy only
                                            # clamps DOWN), so -smp 4 + this
                                            # 90 s timeout is the CI default.
                                            # UBSan needs ~120-300 s; set
                                            # BOOT_TIMEOUT=300 for that
                                            # pipeline explicitly.
BOOT_MARKER="Thylacine boot OK"
EXTINCTION_MARKER="EXTINCTION:"             # per TOOLING.md §10 ABI

# task #70: debug-probe's cross-Proc hardware-watchpoint leg. QEMU TCG programs
# DBGWVR/DBGWCR but never raises EC 0x34, and instead wedges the guest at the
# watched access; the probe bounds its wait and prints a SKIP line there rather
# than hanging the boot. On every substrate that CAN deliver (HVF, real hardware)
# the fire stays a hard requirement -- enforced below, so a real kernel regression
# cannot hide behind the emulator's gap.
HWWATCH_MARKER="debug-probe: hwwatch ok"

# P4-K-events: QMP key injection. Wakes the userspace virtio-input
# probe via QEMU's `send-key` after the probe prints its
# AWAITING_QMP_KEY sentinel to the boot log. THYLACINE_INPUT_INJECT=0
# disables (used when running against THYLACINE_NO_INPUT=1 or
# THYLACINE_NO_QMP=1 where the device or socket isn't present).
INPUT_INJECT="${THYLACINE_INPUT_INJECT:-1}"
INPUT_SENTINEL="virtio-input: AWAITING_QMP_KEY"
INPUT_SUCCESS_MARKER="virtio-input: saw target key"
QMP_SOCK="${THYLACINE_QMP_SOCK:-$BUILD_DIR/qmp.sock}"
INJECT_LOG="$BUILD_DIR/test-inject.log"

mkdir -p "$BUILD_DIR"

# Build first if the ELF is missing.
if [[ ! -f "$KERNEL_ELF" ]]; then
    echo "==> Kernel ELF missing; building..."
    if [[ -n "$sanitize" ]]; then
        "$REPO_ROOT/tools/build.sh" kernel "--sanitize=$sanitize"
    else
        "$REPO_ROOT/tools/build.sh" kernel
    fi
fi

echo "==> Booting kernel under QEMU (timeout ${BOOT_TIMEOUT}s)..."
echo "    Log: $LOG_FILE"

# Launch QEMU in background; capture UART output to log file. Pass the
# alternate build dir via env so run-vm.sh picks up the sanitizer ELF.
THYLACINE_BUILD_DIR="$KERNEL_BUILD" "$REPO_ROOT/tools/run-vm.sh" --no-share --cpus "${THYLACINE_TEST_CPUS:-4}" < /dev/null > "$LOG_FILE" 2>&1 &
QEMU_PID=$!

# P4-K-events: spawn the QMP key-injector (a single long-lived python
# process, tools/qmp-inject-key.py) at QEMU launch if enabled. It
# connects to QMP immediately -- paying interpreter startup + connect +
# capabilities during the guest's multi-second boot -- then tail-follows
# the boot log and issues `send-key` (qcode "a") within ~25 ms of the
# AWAITING_QMP_KEY sentinel. On QEMU virt's virtio-keyboard-device this
# fires KEY press (EV_KEY/30/1) + SYN + KEY release + SYN into the
# eventq. The pre-#362 shape (a bash grep-poll spawning python3 only on
# sentinel match) paid its whole startup inside the guest probe's poll
# window and lost the race under gate load (23/40 ci-smp-gate boots).
INJECT_PID=""
if [[ "$INPUT_INJECT" != "0" ]]; then
    : > "$INJECT_LOG"
    python3 "$REPO_ROOT/tools/qmp-inject-key.py" \
        "$QMP_SOCK" "$LOG_FILE" "$INPUT_SENTINEL" "$INPUT_SUCCESS_MARKER" \
        "$BOOT_TIMEOUT" >>"$INJECT_LOG" 2>&1 &
    INJECT_PID=$!
fi

# Wait up to BOOT_TIMEOUT seconds for the boot marker to appear.
#
# A-5a (login + session): the "Thylacine boot OK" banner no longer ends the boot.
# joey signals SYS_BOOT_COMPLETE (the banner) and then KEEPS RUNNING -- it
# getty-loops /sbin/login. So post-banner code (the getty, a login session) can
# now fault AFTER the banner. To avoid masking a post-banner extinction as a
# false PASS, (a) the EXTINCTION check takes PRECEDENCE over the banner on every
# poll, and (b) on banner-observed we watch a short grace window (still running)
# for a post-banner extinction before declaring PASS. BANNER_GRACE is
# env-overridable (bump it for very slow / oversubscribed SMP boots).
BANNER_GRACE="${BANNER_GRACE:-3}"
deadline=$(( $(date +%s) + BOOT_TIMEOUT ))
result="timeout"
while [[ $(date +%s) -lt $deadline ]]; do
    # Extinction first: a crash is a FAIL whether or not the banner also printed.
    if [[ -f "$LOG_FILE" ]] && grep -q "^$EXTINCTION_MARKER" "$LOG_FILE"; then
        result="extinction"
        break
    fi
    if [[ -f "$LOG_FILE" ]] && grep -q "$BOOT_MARKER" "$LOG_FILE"; then
        # Banner observed. Watch a grace window for a POST-banner extinction
        # (getty/login fault) before declaring PASS -- joey persists past the
        # banner, so a later crash must not be masked.
        result="pass"
        grace_deadline=$(( $(date +%s) + BANNER_GRACE ))
        while [[ $(date +%s) -lt $grace_deadline ]]; do
            if grep -q "^$EXTINCTION_MARKER" "$LOG_FILE"; then
                result="extinction"
                break
            fi
            if ! kill -0 "$QEMU_PID" 2>/dev/null; then
                break
            fi
            sleep 0.1
        done
        # G-1/G-3/G-4 console gate (EVOLVED at G-4, never dropped): the boot
        # scanout owner is now AURORA -- the console renderer (the fbcon) --
        # so the verify is the -c CONSOLE signature (the exact Bonfire bg
        # dominant + exact default-fg text pixels: the login banner/prompt
        # rendered through drain -> VT -> Cornucopia atlas -> the FULL G-2/
        # G-3 surface protocol). The LIVENESS leg proves the loop live via a
        # bounded retry-compare: successive dumps must eventually differ from
        # the first (the 1 Hz cursor blink guarantees change within a second;
        # the login prompt's arrival is a second independent driver) --
        # identical-forever dumps = a dead present loop. The bounded retry
        # absorbs aurora's connect + first-present + login's prompt timing; a
        # persistent miss is a FAIL (tapestryd or aurora down/blank). Skipped
        # when the device/socket is absent (THYLACINE_NO_GPU=1 /
        # THYLACINE_NO_QMP=1), THYLACINE_GPU_GATE=0, or python3 is missing.
        if [[ "$result" == "pass" && "${THYLACINE_NO_GPU:-0}" != "1" \
              && "${THYLACINE_NO_QMP:-0}" != "1" && "${THYLACINE_GPU_GATE:-1}" != "0" ]] \
           && [[ -S "$QMP_SOCK" ]] && command -v python3 >/dev/null 2>&1 \
           && kill -0 "$QEMU_PID" 2>/dev/null; then
            gpu_gate_ok=0
            for _try in $(seq 1 20); do
                if "$REPO_ROOT/tools/screendump.sh" -s "$QMP_SOCK" -c \
                       "$BUILD_DIR/pattern-gate.png" >/dev/null 2>&1; then
                    gpu_gate_ok=1
                    break
                fi
                sleep 0.5
            done
            if [[ "$gpu_gate_ok" == "1" ]]; then
                # The liveness leg: retry-compare against the first dump.
                gpu_gate_ok=0
                for _try in $(seq 1 6); do
                    sleep 0.7
                    if "$REPO_ROOT/tools/screendump.sh" -s "$QMP_SOCK" \
                           "$BUILD_DIR/pattern-gate2.png" >/dev/null 2>&1; then
                        if ! cmp -s "$BUILD_DIR/pattern-gate.png" \
                                    "$BUILD_DIR/pattern-gate2.png"; then
                            gpu_gate_ok=1
                            break
                        fi
                    fi
                done
            fi
            if [[ "$gpu_gate_ok" != "1" ]]; then
                result="gpu-gate"
            fi
        fi
        break
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        # QEMU exited before the marker. Final check on the log (extinction first).
        if grep -q "^$EXTINCTION_MARKER" "$LOG_FILE"; then
            result="extinction"
        elif grep -q "$BOOT_MARKER" "$LOG_FILE"; then
            result="pass"
        else
            result="qemu-exit"
        fi
        break
    fi
    sleep 0.1
done

# Halt QEMU + reap injector.
kill -KILL "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
if [[ -n "$INJECT_PID" ]]; then
    kill -KILL "$INJECT_PID" 2>/dev/null || true
    wait "$INJECT_PID" 2>/dev/null || true
fi

case "$result" in
    pass)
        echo "==> PASS: boot banner observed."
        # P4-K-events: enforce QMP injection success on the CI path.
        # When INPUT_INJECT is on AND the AWAITING_QMP_KEY sentinel
        # was printed (meaning the userspace probe reached its wait
        # state), the log MUST also contain INPUT_SUCCESS_MARKER —
        # otherwise the injector failed or events weren't delivered.
        if [[ "$INPUT_INJECT" != "0" ]] \
           && grep -q "$INPUT_SENTINEL" "$LOG_FILE" \
           && ! grep -q "$INPUT_SUCCESS_MARKER" "$LOG_FILE"; then
            echo "==> FAIL: virtio-input probe reached AWAITING_QMP_KEY but never observed the injected EV_KEY event." >&2
            echo "    Expected log line: '$INPUT_SUCCESS_MARKER'" >&2
            echo "    Likely causes: QMP socket connect failed, python3 missing, send-key not delivered, or driver poll cap too short." >&2
            echo "--- virtio-input log slice ---" >&2
            grep -A 0 "virtio-input:" "$LOG_FILE" >&2 || true
            echo "------------------------------" >&2
            exit 1
        fi
        # task #70: require the watchpoint fire on any accel that can deliver it.
        # run-vm.sh logs the chosen accel ("==> qemu: accel=<a> ..."), so read it
        # back rather than re-deriving the auto-detect here.
        boot_accel="$(sed -n 's/^==> qemu: accel=\([a-z0-9]*\).*$/\1/p' "$LOG_FILE" | head -1)"
        if [[ -n "$boot_accel" && "$boot_accel" != "tcg" ]] \
           && ! grep -q "$HWWATCH_MARKER" "$LOG_FILE"; then
            echo "==> FAIL: accel=$boot_accel delivers EL0 watchpoints, but debug-probe never reported it." >&2
            echo "    Expected log line: '$HWWATCH_MARKER'" >&2
            echo "    A SKIP here means the kernel stopped delivering EC 0x34 on a substrate" >&2
            echo "    that supports it -- i.e. a real regression on the I-39 debug surface," >&2
            echo "    not the QEMU-TCG gap of task #70." >&2
            echo "--- debug-probe log slice ---" >&2
            grep "debug-probe:" "$LOG_FILE" >&2 || true
            echo "-----------------------------" >&2
            exit 1
        fi
        echo "--- log tail ---"
        tail -20 "$LOG_FILE"
        echo "----------------"
        exit 0
        ;;
    gpu-gate)
        echo "==> FAIL: G-4 console gate -- the Aurora scanout did not verify." >&2
        echo "    Either tools/screendump.sh -c could not see the rendered console" >&2
        echo "    (Bonfire bg dominant + exact-fg text) post-boot, or the liveness" >&2
        echo "    retry never saw a differing dump (cursor blink / prompt arrival" >&2
        echo "    -- a dead present loop)." >&2
        echo "--- aurora/tapestryd/warden log slice ---" >&2
        grep -E "aurora:|tapestryd:|warden:" "$LOG_FILE" | tail -20 >&2 || true
        echo "---------------------------------------" >&2
        exit 1
        ;;
    extinction)
        echo "==> FAIL: kernel extinction detected." >&2
        echo "--- ELE context ---" >&2
        grep -B 2 -A 10 "^$EXTINCTION_MARKER" "$LOG_FILE" >&2
        echo "-------------------" >&2
        exit 1
        ;;
    qemu-exit)
        echo "==> FAIL: QEMU exited without emitting boot marker." >&2
        echo "--- full log ---" >&2
        cat "$LOG_FILE" >&2
        echo "----------------" >&2
        exit 1
        ;;
    timeout)
        echo "==> FAIL: timeout (${BOOT_TIMEOUT}s) — no boot marker." >&2
        echo "--- log tail ---" >&2
        tail -30 "$LOG_FILE" >&2
        echo "----------------" >&2
        exit 1
        ;;
esac
