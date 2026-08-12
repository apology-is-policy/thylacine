#!/bin/bash
# sigwatch.sh -- witness the signal that kills a QEMU, and NAME ITS SENDER.
#
# #200: a QEMU in this tree vanishes rarely, mid-boot, with a perfectly healthy
# guest. SIGKILL is uncatchable, so QEMU never gets to print its "terminating on
# signal N from pid M" report -- which is why smp-multiboot.sh's arm-2 detector
# can establish THAT an external kill happened (#222) but is reduced to printing
# "sender| NOT RECOVERABLE". Every in-tree suspect has now been audited and
# cleared or refuted, so the remaining question is precisely the one the victim
# cannot answer: who sent it.
#
# macOS Endpoint Security answers it from OUTSIDE the victim. `eslogger signal`
# reports each signal delivery with both ends, so an uncatchable signal is still
# witnessed. Unlike dtrace this needs no SIP change -- SIP may stay enabled --
# but it does need root (ES_NEW_CLIENT_RESULT_ERR_NOT_PRIVILEGED otherwise).
#
# Usage:
#   sudo tools/sigwatch.sh --selftest   prove the capture CAN see a kill (do
#                                       this FIRST -- an unproven watcher that
#                                       records nothing is indistinguishable
#                                       from a quiet host)
#   sudo tools/sigwatch.sh              watch until Ctrl-C / SIGTERM
#
# EXPECT THE LOG TO BE NON-EMPTY ON EVERY GATE RUN. tools/test.sh ends each boot
# with an unconditional `kill -KILL "$QEMU_PID"` teardown, so a routine passing
# run deposits one entry per boot. That is the baseline, not the finding. THE
# FINDING IS AN ENTRY WHOSE SENDER IS NOT OUR OWN HARNESS -- read the sender
# field, never the mere presence of a record.
#
# The filter is a substring match on the RAW json rather than a jq field path:
# the ES event schema is not pinned by this script, and a field path guessed
# wrong would silently match nothing while looking like a quiet host. --selftest
# is what converts that assumption into a checked one, and it dumps a real
# matching event so the schema can be read off the artifact instead of assumed.
set -u
cd "$(dirname "$0")/.."

ESLOGGER=/usr/bin/eslogger
LOG="${SIGWATCH_LOG:-$PWD/build/sigwatch.jsonl}"
MATCH="${SIGWATCH_MATCH:-qemu-system-aarch64}"
MAX_BYTES="${SIGWATCH_MAX_BYTES:-33554432}"   # 32 MiB, then stop appending
# Watch mode CONSUMES this stamp; only a passing --selftest writes it. Without
# the gate the failure mode is silent and total: an unproven watcher logs
# nothing and reads exactly like a quiet host, which is the same fail-open that
# an unvalidated pattern check produces. The stamp records the OS build because
# the capture rides an OS-owned event schema -- a proof carried across an OS
# upgrade is a proof about different software.
STAMP="${SIGWATCH_STAMP:-$PWD/build/.sigwatch-selftest-ok}"

[[ -x "$ESLOGGER" ]] || { echo "sigwatch: no $ESLOGGER on this host" >&2; exit 2; }

# The proof gate precedes the privilege gate deliberately: both are refusals, so
# the order weakens neither, and this way the gate that protects against a
# silently-empty log can itself be exercised without root.
if [[ "${1:-}" != "--selftest" ]]; then
    if [[ ! -f "$STAMP" ]]; then
        echo "sigwatch: REFUSING to watch -- no passing selftest on record ($STAMP absent)." >&2
        echo "          An unproven watcher records nothing and reads as a quiet host." >&2
        echo "          Run: sudo tools/sigwatch.sh --selftest" >&2
        exit 2
    fi
    stamped_build="$(sed -n 's/^os_build=//p' "$STAMP" | head -1)"
    current_build="$(sw_vers -buildVersion 2>/dev/null)"
    if [[ -n "$current_build" && "$stamped_build" != "$current_build" ]]; then
        echo "sigwatch: REFUSING to watch -- selftest was proven on OS build '$stamped_build', host is now '$current_build'." >&2
        echo "          The event schema is the OS's, so that proof is about different software. Re-run --selftest." >&2
        exit 2
    fi
fi

if [[ "$(id -u)" != "0" ]]; then
    echo "sigwatch: must run as root -- Endpoint Security refuses an unprivileged client." >&2
    echo "          try: sudo tools/sigwatch.sh ${1:-}" >&2
    exit 2
fi

mkdir -p "$(dirname "$LOG")"

# ---------------------------------------------------------------- selftest
# The control has to prove DISCRIMINATION, not merely that eslogger ran: it
# spawns a victim, kills it, and requires that exact victim to appear in the
# capture. A watcher that cannot see a kill it performed itself will not see
# the one we are hunting.
#
# The victim is `perl -e 'sleep 600'` and NOT `bash -c 'sleep 600' fake-argv`:
# bash EXECs a lone simple command, so the wrapper's argv evaporates and the
# fake never carries the name it was given -- a sabotage that quietly passes.
if [[ "${1:-}" == "--selftest" ]]; then
    cap="$(mktemp /tmp/sigwatch-selftest.XXXXXX.jsonl)"
    "$ESLOGGER" signal > "$cap" 2>"$cap.err" &
    espid=$!
    trap 'kill -TERM "$espid" 2>/dev/null; wait "$espid" 2>/dev/null' EXIT

    # Let the ES client attach before generating the event it must catch.
    for _ in $(seq 1 40); do
        [[ -s "$cap" || -s "$cap.err" ]] && break
        sleep 0.25
    done
    if grep -q 'Failed to create ES client' "$cap.err" 2>/dev/null; then
        echo "sigwatch selftest: FAIL -- $(cat "$cap.err")" >&2
        exit 1
    fi
    sleep 1

    /usr/bin/perl -e 'sleep 600' &
    victim=$!
    sleep 1
    kill -KILL "$victim" 2>/dev/null
    wait "$victim" 2>/dev/null
    sleep 2

    hits="$(grep -c "\"$victim\"\|:$victim," "$cap" 2>/dev/null || true)"
    perlhits="$(grep -c '/usr/bin/perl' "$cap" 2>/dev/null || true)"
    echo "sigwatch selftest: captured $(wc -l < "$cap" | tr -d ' ') events; victim pid $victim referenced in $hits; /usr/bin/perl in $perlhits"
    if [[ "${perlhits:-0}" -gt 0 ]]; then
        echo "sigwatch selftest: PASS -- a SIGKILL we sent ourselves was witnessed, and the target's executable path IS present in the event text (so the '$MATCH' substring filter can match a qemu victim)."
        { echo "os_build=$(sw_vers -buildVersion 2>/dev/null)"
          echo "date=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
          echo "victim_pid=$victim"
          echo "--- the matching event, verbatim ---"
          grep -m1 '/usr/bin/perl' "$cap"
        } > "$STAMP"
        echo "sigwatch selftest: wrote $STAMP -- watch mode will now run."
        echo "--- one matching event, verbatim -- read the real field names off THIS, do not assume them ---"
        grep -m1 '/usr/bin/perl' "$cap"
        exit 0
    fi
    echo "sigwatch selftest: FAIL -- the kill was not witnessed, or the target path is absent from the event text." >&2
    echo "  Do NOT deploy the watcher on this result: it would record nothing and read as a quiet host." >&2
    echo "  Capture retained for inspection: $cap" >&2
    trap - EXIT
    kill -TERM "$espid" 2>/dev/null
    exit 1
fi

# ---------------------------------------------------------------- watch
echo "sigwatch: watching signals to '$MATCH' -> $LOG  (Ctrl-C to stop)"
echo "sigwatch: routine teardown kills WILL appear; the finding is a sender that is not ours."
"$ESLOGGER" signal 2>/dev/null | while IFS= read -r line; do
    case "$line" in *"$MATCH"*) ;; *) continue ;; esac
    if [[ -f "$LOG" ]]; then
        sz=$(wc -c < "$LOG" | tr -d ' ')
        [[ "$sz" -ge "$MAX_BYTES" ]] && continue
    fi
    printf '%s\n' "$line" >> "$LOG"
done
