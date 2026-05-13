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

BOOT_TIMEOUT="${BOOT_TIMEOUT:-15}"          # seconds
BOOT_MARKER="Thylacine boot OK"
EXTINCTION_MARKER="EXTINCTION:"             # per TOOLING.md §10 ABI

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
THYLACINE_BUILD_DIR="$KERNEL_BUILD" "$REPO_ROOT/tools/run-vm.sh" --no-share < /dev/null > "$LOG_FILE" 2>&1 &
QEMU_PID=$!

# P4-K-events: spawn background QMP key-injector if enabled. Polls
# the boot log for INPUT_SENTINEL; on match, connects to QMP and
# sends `send-key` for "a". On QEMU virt's virtio-keyboard-device
# this fires KEY press (EV_KEY/30/1) + SYN + KEY release + SYN into
# the eventq.
INJECT_PID=""
if [[ "$INPUT_INJECT" != "0" ]]; then
    : > "$INJECT_LOG"
    (
        # Wait up to BOOT_TIMEOUT for the sentinel to appear, then
        # connect to QMP and send-key. Both stages have brief retry
        # loops so socket-create + sentinel-print races don't matter.
        echo "[$(date +%H:%M:%S.%N)] injector: waiting for sentinel" >>"$INJECT_LOG"
        deadline=$(( $(date +%s) + BOOT_TIMEOUT ))
        while [[ $(date +%s) -lt $deadline ]]; do
            if [[ -f "$LOG_FILE" ]] && grep -q "$INPUT_SENTINEL" "$LOG_FILE"; then
                break
            fi
            sleep 0.05
        done
        if [[ ! -f "$LOG_FILE" ]] || ! grep -q "$INPUT_SENTINEL" "$LOG_FILE"; then
            echo "[$(date +%H:%M:%S.%N)] injector: sentinel never seen; exiting" >>"$INJECT_LOG"
            exit 0
        fi
        echo "[$(date +%H:%M:%S.%N)] injector: sentinel observed; connecting QMP" >>"$INJECT_LOG"
        python3 - "$QMP_SOCK" "$INJECT_LOG" <<'PY' 2>>"$INJECT_LOG" || echo "[$(date +%H:%M:%S.%N)] injector: python3 returned non-zero" >>"$INJECT_LOG"
import socket, json, sys, time, datetime
sock_path = sys.argv[1]
log_path = sys.argv[2]
def lg(msg):
    with open(log_path, "a") as fp:
        fp.write(f"[{datetime.datetime.now().strftime('%H:%M:%S.%f')}] python: {msg}\n")
sock = None
for i in range(50):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(sock_path)
        sock = s
        lg(f"connected to {sock_path} after {i} retries")
        break
    except (FileNotFoundError, ConnectionRefusedError) as e:
        time.sleep(0.05)
if sock is None:
    lg("socket not reachable; giving up")
    sys.exit("qmp: socket not reachable")
f = sock.makefile("rw", buffering=1)
greeting = f.readline()
lg(f"greeting: {greeting.strip()[:120]}")
f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n"); f.flush()
caps_resp = f.readline()
lg(f"caps resp: {caps_resp.strip()[:120]}")
f.write(json.dumps({
    "execute": "send-key",
    "arguments": {"keys": [{"type": "qcode", "data": "a"}]}
}) + "\n"); f.flush()
sk_resp = f.readline()
lg(f"send-key resp: {sk_resp.strip()[:120]}")
sock.close()
lg("sent + closed cleanly")
PY
        echo "[$(date +%H:%M:%S.%N)] injector: done" >>"$INJECT_LOG"
    ) &
    INJECT_PID=$!
fi

# Wait up to BOOT_TIMEOUT seconds for the boot marker to appear.
deadline=$(( $(date +%s) + BOOT_TIMEOUT ))
result="timeout"
while [[ $(date +%s) -lt $deadline ]]; do
    if [[ -f "$LOG_FILE" ]] && grep -q "$BOOT_MARKER" "$LOG_FILE"; then
        result="pass"
        break
    fi
    if [[ -f "$LOG_FILE" ]] && grep -q "^$EXTINCTION_MARKER" "$LOG_FILE"; then
        result="extinction"
        break
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        # QEMU exited before the marker. Final check on the log.
        if grep -q "$BOOT_MARKER" "$LOG_FILE"; then
            result="pass"
        elif grep -q "^$EXTINCTION_MARKER" "$LOG_FILE"; then
            result="extinction"
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
        echo "--- log tail ---"
        tail -20 "$LOG_FILE"
        echo "----------------"
        exit 0
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
