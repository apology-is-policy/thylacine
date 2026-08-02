#!/usr/bin/env bash
# tools/stall-amplify.sh -- #125: make the stall DETERMINISTIC instead of rare.
#
# Rate-hunting #125 is statistically hopeless at the observed ~1-in-33: even 40
# clean boots leave a ~29% chance the bug is untouched, so a null proves almost
# nothing in either direction. This script stops guessing and CREATES the
# precondition.
#
# The mechanism is documented by the relay itself (serial-bridge.py): if the
# host stops draining QEMU's serial socket, QEMU's send buffer fills, the guest
# UART TX FIFO fills, and the guest hits the kernel #75/#67 TX deadline. So:
# SIGSTOP the relay for a window, and the guest is back-pressured on demand.
#
# The window is aimed at dap-probe, because that is exactly where #125 died --
# its last bytes were `proc: orphan pid=1900 name="ambush-chil`, an orphan
# reparent truncated at uart_putc's drop signature.
#
# WHAT THE TWO ARMS SHOULD SHOW (this is the whole point):
#   pre-#126  proc_reparent_children emits ~90 bytes via uart_putc, each
#             spinning up to 20 ms, all under an irqsave g_proc_table_lock ->
#             ~1.8 s IRQ-masked per orphan -> the guest WEDGES.
#   post-#126 the same diagnostic goes through the TX ring and DROPS instead of
#             spinning -> bytes are lost, the guest SURVIVES.
#
# A wedge here is observed by stall-watch.py (register-digest liveness), not by
# the console -- the console is stalled by construction.
#
# usage: stall-amplify.sh [scenario] [stop-seconds] [trigger-marker]

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCEN="${1:-ls-ci}"
STOP_S="${2:-25}"
MARKER="${3:-dap-probe}"
OUT="$REPO_ROOT/build/stall-amplify"
LOG="$REPO_ROOT/build/ls-ci-$SCEN.log"

mkdir -p "$OUT"
rm -f "$LOG"

echo "==> amplify: scenario=$SCEN stop=${STOP_S}s trigger='$MARKER'"

python3 "$REPO_ROOT/tools/stall-watch.py" \
    --sock "$REPO_ROOT/build/qmp.sock" --log "$LOG" \
    --elf "$REPO_ROOT/build/kernel/thylacine.elf" \
    --out "$OUT/watch.log" --quiet-s 8 --sample-s 2 --deadline-s 400 \
    > "$OUT/watch-stdout.log" 2>&1 &
watcher=$!

LS_CI_ATTEMPTS=1 "$REPO_ROOT/tools/test-interactive.sh" "$SCEN" \
    > "$OUT/run.log" 2>&1 &
runner=$!

# Wait for the trigger, then freeze the relay. The pattern is anchored on THIS
# tree's path so it can never match a sibling worktree's relay (the #89 rule).
BRIDGE_PAT="$REPO_ROOT/tools/interactive/serial-bridge.py"
stopped=""
deadline=$(( $(date +%s) + 240 ))
while [[ $(date +%s) -lt $deadline ]]; do
    kill -0 "$runner" 2>/dev/null || break
    if LC_ALL=C grep -aq "$MARKER" "$LOG" 2>/dev/null; then
        stopped=$(pgrep -f "$BRIDGE_PAT" | head -1)
        if [[ -n "$stopped" ]]; then
            echo "==> trigger '$MARKER' seen; SIGSTOP relay pid=$stopped for ${STOP_S}s"
            kill -STOP "$stopped"
            sleep "$STOP_S"
            kill -CONT "$stopped" 2>/dev/null
            echo "==> relay resumed"
        else
            echo "==> trigger seen but NO relay matched '$BRIDGE_PAT' -- not amplified"
        fi
        break
    fi
    sleep 0.2
done
[[ -n "$stopped" ]] || echo "==> WARNING: relay never stopped -- this run is NOT a valid amplification"

wait "$runner"; rc=$?
kill "$watcher" 2>/dev/null; wait "$watcher" 2>/dev/null

echo "==> scenario rc=$rc  ($([[ $rc -eq 0 ]] && echo 'guest SURVIVED the stall' || echo 'guest did NOT complete'))"
echo "==> watcher verdict(s):"
grep -aoE 'H-[AB]: [^;]*' "$OUT/watch.log" 2>/dev/null | sort | uniq -c | sed 's/^/    /' \
    || echo "    (none -- console never went quiet)"
grep -aE 'QUIET|frozen|EXECUTING' "$OUT/watch.log" 2>/dev/null | tail -4 | sed 's/^/    /'
exit 0
