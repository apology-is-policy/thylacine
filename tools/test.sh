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
KERNEL_ELF="$BUILD_DIR/kernel/thylacine.elf"
LOG_FILE="$BUILD_DIR/test-boot.log"

BOOT_TIMEOUT="${BOOT_TIMEOUT:-10}"          # seconds
BOOT_MARKER="Thylacine boot OK"
EXTINCTION_MARKER="EXTINCTION:"             # per TOOLING.md §10 ABI

mkdir -p "$BUILD_DIR"

# Build first if the ELF is missing.
if [[ ! -f "$KERNEL_ELF" ]]; then
    echo "==> Kernel ELF missing; building..."
    "$REPO_ROOT/tools/build.sh" kernel
fi

echo "==> Booting kernel under QEMU (timeout ${BOOT_TIMEOUT}s)..."
echo "    Log: $LOG_FILE"

# Launch QEMU in background; capture UART output to log file.
"$REPO_ROOT/tools/run-vm.sh" --no-share < /dev/null > "$LOG_FILE" 2>&1 &
QEMU_PID=$!

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

# Halt QEMU.
kill -KILL "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

case "$result" in
    pass)
        echo "==> PASS: boot banner observed."
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
