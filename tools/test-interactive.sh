#!/usr/bin/env bash
# tools/test-interactive.sh -- the LS-CI interactive E2E regression net.
#
# Boots Thylacine under QEMU and drives a REAL PTY into the `-serial mon:stdio`
# console via `expect`, logging in as a seeded user and asserting the rendered
# output. This is the ONLY way to inject console keystrokes (a piped stdin EOFs
# the chardev), and therefore the only harness that can catch the interactive
# regression class -- LS-1 (UART RX disabled) and LS-2 (external output dropped)
# both shipped silently because CI could not type. See docs/LIFE-SUPPORT.md
# ("LS-CI"). Every later LS chunk lands a scenario here.
#
# OPTIONAL gate: `expect` is an optional dependency. Absent it, this SKIPs
# (exit 0) so a pipeline that wires it stays green on hosts without expect.
#
# Usage:
#   tools/test-interactive.sh                 # run every tools/interactive/*.exp
#   tools/test-interactive.sh ls-ci           # run one scenario by name
#   tools/test-interactive.sh path/to/x.exp   # run one scenario by path
#
# Env:
#   THYLACINE_ACCEL=hvf|tcg     accel (default tcg -- deterministic compat run)
#   LS_CI_BOOT_TIMEOUT=N        seconds to reach the shell (default 180)
#   LS_CI_CMD_TIMEOUT=N         seconds per command's output (default 30)

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
SCEN_DIR="$REPO_ROOT/tools/interactive"

# --- optional-dependency degrade ---
if ! command -v expect >/dev/null 2>&1; then
    echo "==> SKIP: 'expect' not found -- LS-CI is an optional interactive gate."
    echo "    Install it to run (macOS ships /usr/bin/expect; Debian: apt-get install expect)."
    exit 0
fi

export THYLACINE_ACCEL="${THYLACINE_ACCEL:-tcg}"
# Stage 6: with the Go GOROOT baked by default, joey's go4c on-device
# compile+link probe rides every boot -- a TCG slow-mode boot + the probe can
# exceed 180 s. Same goroot-staged auto-bump as tools/test.sh; an explicit
# LS_CI_BOOT_TIMEOUT always wins.
if [[ -d "$BUILD_DIR/go/goroot" ]]; then
    export LS_CI_BOOT_TIMEOUT="${LS_CI_BOOT_TIMEOUT:-300}"
else
    export LS_CI_BOOT_TIMEOUT="${LS_CI_BOOT_TIMEOUT:-180}"
fi
export LS_CI_CMD_TIMEOUT="${LS_CI_CMD_TIMEOUT:-30}"

# Reap only THIS repo's qemu -- match THIS tree's build dir in the cmdline
# (-kernel $BUILD_DIR/kernel/thylacine.bin), so a co-resident qemu from a
# SIBLING WORKTREE survives. The old pattern ("qemu-system-aarch64.*thylacine")
# matched every thylacine tree: two sessions gating concurrently (main +
# thylacine-aux, 2026-07-21) shot each other's live VMs -- "qemu GONE, guest
# healthy" mid-scenario failures on both sides (task #59).
reap_qemu() { pkill -9 -f "qemu-system-aarch64.*$BUILD_DIR/" 2>/dev/null || true; }
trap reap_qemu EXIT

# --- ensure boot artifacts exist (match test.sh: build-if-missing) ---
KERNEL_BIN="$BUILD_DIR/kernel/thylacine.bin"
RAMFS="$BUILD_DIR/ramfs.cpio"
POOL="$BUILD_DIR/fixtures/pool.img"
if [[ ! -f "$KERNEL_BIN" || ! -f "$RAMFS" ]]; then
    echo "==> kernel/ramfs missing; building (tools/build.sh kernel)..."
    "$REPO_ROOT/tools/build.sh" kernel
fi
if [[ ! -f "$POOL" ]]; then
    echo "==> pool fixture missing; building (tools/build.sh pool)..."
    "$REPO_ROOT/tools/build.sh" pool
fi

# --- scenario selection ---
scenarios=()
if [[ $# -gt 0 ]]; then
    for a in "$@"; do
        if   [[ -f "$a" ]];               then scenarios+=("$a")
        elif [[ -f "$SCEN_DIR/$a" ]];     then scenarios+=("$SCEN_DIR/$a")
        elif [[ -f "$SCEN_DIR/$a.exp" ]]; then scenarios+=("$SCEN_DIR/$a.exp")
        else echo "==> scenario not found: $a" >&2; exit 2
        fi
    done
else
    for f in "$SCEN_DIR"/*.exp; do
        [[ "$(basename "$f")" == "lib.exp" ]] && continue   # library, not a scenario
        scenarios+=("$f")
    done
fi
if [[ ${#scenarios[@]} -eq 0 ]]; then
    echo "==> no scenarios under $SCEN_DIR" >&2
    exit 2
fi

mkdir -p "$BUILD_DIR"
echo "==> LS-CI: ${#scenarios[@]} scenario(s); accel=$THYLACINE_ACCEL boot<=${LS_CI_BOOT_TIMEOUT}s cmd<=${LS_CI_CMD_TIMEOUT}s"

# Bounded retry per scenario. It does NOT mask a real regression: a genuine
# break (e.g. LS-2 reverted) fails EVERY attempt deterministically (the output is
# missing each time), so a scenario fails only if ALL attempts fail.
#
# #72 CORRECTION -- this block used to justify the retry by asserting that "an
# unexpected qemu exit before a terminal PASS/FAIL is a host-timing artifact
# (TCG-under-oversubscription), never a kernel fault". That was WRONG, and it
# was never measured. Ground truth (N=10, instrumented): 5 of 10 boots were
# lost, and in ALL FIVE the VM was still ALIVE (stat R+/S+) while the `nc -U`
# serial relay had died of SIGPIPE (`bridge exit=141`). It was never a qemu
# exit at all -- it was the HARNESS's own relay dying, mislabelled by lib.exp's
# `eof` arm and then rationalized here as host timing. The relay is now
# serial-bridge.py (SIGPIPE-immune); the retry stays as belt-and-braces, but a
# retry is a TOLERANCE, never a diagnosis. If attempts start failing again,
# read the preserved evidence -- do not reach for "host timing".
fails=0
attempts="${LS_CI_ATTEMPTS:-3}"
for scen in "${scenarios[@]}"; do
    name="$(basename "$scen" .exp)"
    transcript="$BUILD_DIR/ls-ci-$name.log"
    steps="$BUILD_DIR/ls-ci-$name.steps"
    # #72: failed-attempt evidence must survive the retry. The per-attempt
    # truncation below used to destroy the very transcript a retry was
    # retrying over, so a claim like "attempt 1 was a host-timing artifact"
    # could never be checked against its own evidence -- the no-host-load
    # discipline needs the artifact to LOOK at. Each failed attempt is
    # archived as ls-ci-<name>.attempt<N>.{log,steps}; retention is bounded
    # to the LAST run (cleared here, per scenario, not per attempt).
    rm -f "$BUILD_DIR/ls-ci-$name.attempt"*.log \
          "$BUILD_DIR/ls-ci-$name.attempt"*.steps 2>/dev/null || true
    echo "==> scenario: $name (up to $attempts attempt(s))"
    passed=0
    for attempt in $(seq 1 "$attempts"); do
        : > "$transcript"
        : > "$steps"
        reap_qemu
        sleep 0.5
        # Run expect UNDER `script` so its stdio is a real PTY. macOS expect 5.45
        # corrupts its own std channels inside `spawn` when its stdout is NOT a tty
        # (a `>file` redirect OR a pipe) -- it aborts with "Tcl_RegisterChannel:
        # duplicate channel names" (SIGABRT) or breaks `puts` with "bad file number".
        # `script -q <file> <cmd>` gives <cmd> a controlling PTY, captures the full
        # session to <file> (our transcript), AND propagates <cmd>'s exit code. The
        # steps file is the flush-immune live view. `< /dev/null` is a clean stdin;
        # `script` still waits for the wrapped command to exit (verified).
        LS_CI_STEPS="$steps" script -q "$transcript" expect -f "$scen" < /dev/null >/dev/null 2>&1
        rc=$?
        reap_qemu
        if [[ $rc -eq 0 ]]; then
            if [[ $attempt -gt 1 ]]; then
                echo "    PASS: $name (attempt $attempt/$attempts; earlier failed attempt(s) preserved: $BUILD_DIR/ls-ci-$name.attempt*.{log,steps})"
            else
                echo "    PASS: $name"
            fi
            passed=1
            break
        fi
        # Preserve this attempt's evidence BEFORE the next attempt truncates.
        cp "$transcript" "$BUILD_DIR/ls-ci-$name.attempt$attempt.log" 2>/dev/null || true
        cp "$steps" "$BUILD_DIR/ls-ci-$name.attempt$attempt.steps" 2>/dev/null || true
        echo "    attempt $attempt/$attempts FAILED (rc=$rc; evidence: ls-ci-$name.attempt$attempt.{log,steps})" >&2
        [[ $attempt -lt $attempts ]] && echo "    retrying (an unexplained early exit -- see the preserved evidence; a retry is NOT a diagnosis)..." >&2
    done
    if [[ $passed -ne 1 ]]; then
        echo "    FAIL: $name -- all $attempts attempts failed (deterministic = a real regression, not a flake)" >&2
        echo "    --- steps ($steps; last attempt) ---" >&2
        cat "$steps" >&2 2>/dev/null || true
        echo "    --- transcript tail ($transcript; last attempt) ---" >&2
        tr -d '\r' < "$transcript" 2>/dev/null | tail -40 >&2 || true
        echo "    every attempt's evidence: $BUILD_DIR/ls-ci-$name.attempt*.{log,steps}" >&2
        echo "    --------------------------------------" >&2
        fails=$((fails + 1))
    fi
done

if [[ $fails -gt 0 ]]; then
    echo "==> LS-CI: FAIL -- $fails/${#scenarios[@]} scenario(s) failed." >&2
    exit 1
fi
echo "==> LS-CI: PASS -- all ${#scenarios[@]} scenario(s)."
exit 0
