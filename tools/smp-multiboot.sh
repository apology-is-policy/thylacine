#!/usr/bin/env bash
# SMP multi-boot soundness gate.
#
# Single boots lie: the #788/#806/#860 SMP context-corruption races are
# layout-/timing-sensitive and pass a single boot most of the time. This re-runs
# tools/test.sh N times against the SAME built kernel (host scheduling jitter
# varies the timing each boot) and CLASSIFIES failures, because a nonzero
# test.sh is NOT automatically a scheduler bug:
#
#   CORRUPTION  -- a real SMP soundness failure (the thing we hunt): the
#                  extinction message matches a ctx/stack-corruption signature
#                  (invalid prev state, stack canary, kernel stack overflow,
#                  wild PC, #860, double-run). These FAIL the gate.
#   EXTERNAL-KILL -- something OUTSIDE the harness signalled QEMU. TWO arms,
#                  because they witness genuinely different things:
#                    (1) CATCHABLE signals (TERM/INT/HUP): the boot log
#                        carries QEMU's own 'terminating on signal N from
#                        pid M' report (#88), which also names the sender.
#                    (2) SIGKILL: uncatchable, so QEMU never gets to print
#                        that report -- the one signal that most needs this
#                        bucket was structurally the one signature that
#                        could not reach it, and a real SIGKILL therefore
#                        landed in OTHER (#222, the #200 sightings). The
#                        only witness is the SHELL's job notification in the
#                        harness stream ('line N: PID Killed: 9').
#                  Arm (2) requires test.sh's `qemu_alive_at_teardown=0` as
#                  well as the notification. That conjunct is load-bearing,
#                  not belt-and-braces: bash emits the notification whenever
#                  a job died by signal and a command boundary elapsed
#                  before the shell reaped it, so the harness's OWN teardown
#                  kill prints it too (measured -- see test-smp-classify.sh
#                  case G). alive=0 means that kill hit an already-dead
#                  process, so the signal death cannot have originated here.
#                  Reported with the signal, the sender pid where arm (1)
#                  supplies one (ps'd at classify time, seconds after the
#                  kill, while the sender may still be alive), and the
#                  guest's health at the kill. Ladder position: after
#                  CORRUPTION (a real corruption signature is never masked
#                  by a subsequent kill), before INJECT-MISS (a killed-but-
#                  green boot must not be absorbed into the non-failing
#                  class). Still FAILS the gate: not a guest defect, but
#                  silently passing would let real external interference
#                  hide.
#   INJECT-MISS -- the harness's QMP key-injector failed to deliver the
#                  virtio-input event (#362): the guest reached AWAITING_QMP_
#                  KEY, SKIPped cleanly, and the boot is PROVEN green (banner
#                  present, no extinction, 0 test FAILs) -- test.sh then fails
#                  the boot at its injection-enforcement gate. A host-side
#                  delivery artifact, NOTED, does not fail the gate. The
#                  classification requires the full green-guest proof; a boot
#                  that merely also missed injection stays CORRUPTION/OTHER.
#   TIMING      -- a benign host-fragility soft-warn: anchored on EMITTED warn
#                  strings ('[SOFT-WARN]', the irq-bench CI-budget text) --
#                  NEVER on test names. The pre-#362 regex contained
#                  'stalk.*lifetime', which matched the PASSING test-name line
#                  '[test] stalk.lifetime_no_leak ... PASS' present in every
#                  boot log, making TIMING a catch-all that absorbed ANY
#                  nonzero exit (23/40 inject-misses were buried here -- and a
#                  real unclassified failure would have been too).
#   OTHER       -- an unclassified nonzero exit; surfaced for investigation
#                  and FAILS the gate (feedback_no_host_load: an unexplained
#                  red is surfaced, never absorbed).
#
# Usage:  tools/smp-multiboot.sh <label> <cpus> <N> [undefined]
#   e.g.  tools/smp-multiboot.sh ubsan-smp4 4 15 undefined
#         tools/smp-multiboot.sh default-smp8 8 15
#
# Exit 0 iff 0 CORRUPTION, 0 EXTERNAL-KILL, and 0 OTHER across all N boots.
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LABEL="${1:-default-smp4}"
CPUS="${2:-4}"
N="${3:-10}"
SAN="${4:-}"
sanflag=""
[[ -n "$SAN" ]] && sanflag="--sanitize=$SAN"

LOG="$REPO_ROOT/build/test-boot.log"
FAILDIR="$REPO_ROOT/build/multiboot-fails"
# Signatures of a real ctx/stack corruption (the SMP soundness bug class).
# Use the EXACT extinction strings -- bare "canary" would match the benign
# "canaries" hardening banner + "canary: initialized" boot line (false positive).
CORRUPT_RE='invalid prev state|stack canary mismatch|kernel stack overflow|already on_cpu|#860|not RUNNABLE-and-off-cpu|corrupted current|sched: deadlock'
# Benign host-timing fragility: EMITTED warn strings only, never test names
# (#362 -- see the header). The only current emitter is TEST_SOFT_WARN
# ('[SOFT-WARN] IRQ-to-userspace p99 exceeds CI sanity budget'), which alone
# cannot fail a boot, so this class should stay near-empty.
TIMING_RE='\[SOFT-WARN\]|exceeds CI sanity budget'
# External kill (#88): QEMU's own report when SIGTERM/SIGINT/SIGHUP'd. QEMU
# stderr is merged into $LOG (test.sh routes run-vm.sh '2>&1' into it; run-vm
# execs qemu). The pattern deliberately does not anchor the line end -- newer
# QEMUs append the sender name ' (comm)' / ' (<unknown process>)' after the
# pid, older captures stop at the pid; both forms match.
EXTKILL_RE='terminating on signal [0-9]+ from pid [0-9]+'
# EXTERNAL-KILL arm (2), the SIGKILL witness (#222). This is the shell's job
# notification, e.g.
#   tools/test.sh: line 259: 7655 Killed: 9    ...run-vm.sh...
# Anchored on the whole 'line N: PID Signal: M' shape rather than a bare
# 'Killed:' because the harness log EMBEDS the entire guest log on the
# qemu-exit path (test.sh cats it), so a bare token would be forgeable by
# guest output. Measured: 0 matches across 152 KB of real passing guest log
# and 0 in the real passing harness log; 1 in a real externally-SIGKILLed
# run. Deliberately NOT widened to 'Segmentation fault:' / 'Abort trap:' --
# those are QEMU crashing, which is a different class and must not be
# laundered as external interference.
HARNESS_SIGKILL_RE='line [0-9]+: +[0-9]+ +(Killed|Terminated): +[0-9]+'
TEARDOWN_DEAD='qemu_alive_at_teardown=0'
# The #362 inject-miss green-guest proof (ALL must hold to classify INJECT).
INJ_SENTINEL='virtio-input: AWAITING_QMP_KEY'
INJ_SKIP='virtio-input: SKIP'
BOOT_OK_RE='Thylacine boot OK'
SUITE_FAIL_RE='tests: [0-9]+/[0-9]+ (FAIL|fail)'
INJECT_LOG="$REPO_ROOT/build/test-inject.log"

# The guest is provably green AND the only defect is the missed key delivery.
inject_miss_green() {
    local glog="$1"
    grep -aqF "$INJ_SENTINEL" "$glog" \
        && grep -aqF "$INJ_SKIP" "$glog" \
        && grep -aq "$BOOT_OK_RE" "$glog" \
        && ! grep -aq '^EXTINCTION:' "$glog" \
        && ! grep -aqE "$SUITE_FAIL_RE" "$glog"
}

# A signal death that provably did NOT come from the harness's own teardown.
# BOTH conditions are required -- see the EXTERNAL-KILL header note: the
# notification alone does not mean "someone else did it".
harness_signal_kill() {
    local hlog="$1"
    [[ -f "$hlog" ]] || return 1
    grep -aqE "$HARNESS_SIGKILL_RE" "$hlog" || return 1
    grep -aqF "$TEARDOWN_DEAD" "$hlog"
}

# test.sh's own verdict string -> its internal result token, so a capture's
# NAME says which failure it was. Nothing about the gate's pass/fail rests on
# this (it only labels), so an unrecognised verdict degrades to 'unknown'
# rather than to a wrong answer.
#
# THAT LAST SENTENCE IS FALSE FOR POST-BANNER GATES and the arm below says so:
# they run after '==> PASS' is already in the log, so a missing arm degrades to
# 'pass'. Left standing as the general case, corrected here rather than deleted
# because it is the reasoning that made #212 easy to reintroduce (#234).
#
# EVERY LITERAL BELOW IS PRODUCED BY tools/test.sh. The two files are checked
# against each other by tools/test-smp-classify.sh, which extracts these
# patterns and requires test.sh to still emit each one -- renaming a verdict
# string on one side silently voids an arm otherwise (#234, found exactly that
# way).
harness_result_token() {
    local hlog="$1"
    [[ -f "$hlog" ]] || { echo unknown; return; }
    if   grep -aqF '==> FAIL: QEMU exited without emitting boot marker' "$hlog"; then echo qemu-exit
    elif grep -aqF '==> FAIL: timeout'                                  "$hlog"; then echo timeout
    elif grep -aqF '==> FAIL: kernel extinction detected'               "$hlog"; then echo extinction
    elif grep -aqF '==> FAIL: G-4 console gate'                         "$hlog"; then echo gpu-gate
    elif grep -aqF '==> FAIL: virtio-input probe reached'               "$hlog"; then echo inject-enforce
    elif grep -aqE '==> FAIL: accel=.* delivers EL0 watchpoints'        "$hlog"; then echo hwwatch
    # #212. MUST precede the '==> PASS' arm: every post-banner gate failure
    # runs AFTER test.sh has already printed '==> PASS: boot banner observed',
    # so a missing arm here does not degrade to 'unknown' -- it degrades to
    # 'pass', which is worse, and would have labelled the capture OTHER-pass.
    elif grep -aqF '==> FAIL: arc/clade gate verdict'                   "$hlog"; then echo arc-gates
    elif grep -aqF '==> PASS'                                           "$hlog"; then echo pass
    else echo unknown
    fi
}

# Classify ONE non-PASS boot from its two logs. Echoes exactly one token:
# CORRUPT | EXTKILL | INJECT | TIMING | OTHER. Kept pure (no counters, no
# globals but the patterns) so tools/test-smp-classify.sh can drive the real
# ladder over real and synthetic fixtures -- a classifier nobody can exercise
# is one whose arms are assumed rather than known to fire.
classify_boot() {
    local glog="$1" hlog="$2"
    if grep -aqE "$CORRUPT_RE" "$glog" 2>/dev/null; then echo CORRUPT; return; fi
    if grep -aqE "$EXTKILL_RE" "$glog" 2>/dev/null || harness_signal_kill "$hlog"; then
        echo EXTKILL; return
    fi
    if inject_miss_green "$glog" 2>/dev/null; then echo INJECT; return; fi
    if grep -aqE "$TIMING_RE" "$glog" 2>/dev/null; then echo TIMING; return; fi
    echo OTHER
}

# Everything above is pure: patterns + classifiers, no side effects. Sourcing
# stops here so tools/test-smp-classify.sh can drive the REAL ladder over
# fixtures. A test that re-declared these patterns would only ever establish
# that the copy agrees with itself (#143), which is not evidence about the
# gate. Nothing below this line may be needed by the classifier.
if [[ "${BASH_SOURCE[0]}" != "${0}" ]]; then
    return 0
fi

mkdir -p "$FAILDIR"
# Clear THIS label's prior captures so the dir only ever reflects the latest
# run of each label -- stale fail logs from a previous (or since-fixed-buggy)
# run otherwise masquerade as current findings. Per-label (not whole-dir) so a
# gate running multiple labels back-to-back does not wipe a sibling's captures.
#
# ARCHIVE, never delete (#223). The clearing rationale above is sound, but the
# standard response to a RARE failure is to re-run that same label in
# isolation -- so a delete-on-start makes the diagnostic act itself destroy
# the only copy of what you were diagnosing. It already cost us the #200
# sighting-2 harness log: its SIGKILL line now survives only as a quotation
# inside a commit message. Moving is as effective as deleting for the
# masquerade hazard and costs a rename.
shopt -s nullglob
prior_captures=("$FAILDIR/$LABEL-"*.log)
shopt -u nullglob
if (( ${#prior_captures[@]} > 0 )); then
    ARCHDIR="$FAILDIR/archive/$LABEL-$(date -u +%Y%m%dT%H%M%SZ)"
    mkdir -p "$ARCHDIR"
    mv "${prior_captures[@]}" "$ARCHDIR"/ 2>/dev/null || true
    echo "  [$LABEL] archived ${#prior_captures[@]} prior capture(s) -> ${ARCHDIR#$REPO_ROOT/}"
fi

# Per-boot pool restore (#362): every boot's go4c probes write GOCACHE/$WORK
# into the Stratum pool with ~6x CoW amplification (#39 -- garbage only a
# commit sweeps), so N cumulative boots age the pool (later boots slow toward
# the timeout, and a long matrix would eventually ENOSPC -> false reds). Each
# boot starts from the baked snapshot instead -- also making per-boot timing
# comparable. cp -c is an APFS clonefile (instant CoW); falls back to a plain
# copy elsewhere. The key twin is validated coherent (the ramfs bakes the key,
# so ONLY the matching pool may be restored). SMP_GATE_POOL_RESTORE=0 opts out.
POOL_IMG="$REPO_ROOT/build/fixtures/pool.img"
POOL_SNAP="$POOL_IMG.baked-snapshot"
KEY_IMG="$REPO_ROOT/build/fixtures/system.key"
KEY_SNAP="$KEY_IMG.baked-snapshot"
pool_restore() {
    [[ "${SMP_GATE_POOL_RESTORE:-1}" == "0" ]] && return 0
    [[ -f "$POOL_SNAP" && -f "$POOL_IMG" ]] || return 0
    if ! cmp -s "$KEY_IMG" "$KEY_SNAP" 2>/dev/null; then
        echo "  [$LABEL] pool restore SKIPPED: system.key does not match its snapshot (stale twins?)" >&2
        return 0
    fi
    cp -c "$POOL_SNAP" "$POOL_IMG" 2>/dev/null || cp "$POOL_SNAP" "$POOL_IMG"
}

pass=0; corrupt=0; extkill=0; inject=0; timing=0; other=0
# The harness-side (test.sh stdout/stderr) capture. $LOG is the GUEST serial
# only; a post-banner verdict step (the -c console gate, the liveness
# compare) that fails leaves NO trace there -- the ubsan-smp8 OTHER of
# 2026-07-19 was undiagnosable because this stream went to /dev/null. Kept
# alongside the serial log on every non-PASS classification.
HARNESS_LOG="$REPO_ROOT/build/test-harness.log"

# Keep both logs for a non-PASS boot, under a name that says which class and
# (where it disambiguates) which test.sh result token produced it.
capture() {
    local tag="$1" idx="$2"
    cp "$LOG"         "$FAILDIR/$LABEL-$idx-$tag.log"         2>/dev/null || true
    cp "$HARNESS_LOG" "$FAILDIR/$LABEL-$idx-$tag-harness.log" 2>/dev/null || true
}

# Per-boot wall clock. Recorded on EVERY boot, not just failures, because the
# open question on #200 is whether the observed UBSan asymmetry is a sanitizer
# effect or merely an exposure-time one -- a host-side killer cannot care which
# sanitizer built the GUEST, but a UBSan boot runs longer and so presents a
# proportionally larger window. Answering that needs a per-unit-time hazard
# rate, which needs these numbers from ordinary gate runs rather than from a
# bespoke experiment nobody re-runs.
boot_secs_total=0
boot_secs_list=""

for i in $(seq 1 "$N"); do
    pool_restore
    boot_t0=$(date +%s)
    rc_ok=1
    THYLACINE_TEST_CPUS="$CPUS" "$REPO_ROOT/tools/test.sh" $sanflag >"$HARNESS_LOG" 2>&1 || rc_ok=0
    boot_secs=$(( $(date +%s) - boot_t0 ))
    boot_secs_total=$(( boot_secs_total + boot_secs ))
    boot_secs_list="$boot_secs_list $boot_secs"

    if (( rc_ok )); then
        # Belt-and-suspenders: even on exit 0, fail if a corruption marker leaked.
        if grep -aqE "$CORRUPT_RE" "$LOG"; then
            corrupt=$((corrupt+1)); capture CORRUPT "$i"
            echo "  [$LABEL $i/$N] CORRUPTION (despite exit 0): $(grep -aoE "$CORRUPT_RE" "$LOG" | head -1)  (${boot_secs}s)"
        else
            pass=$((pass+1)); echo "  [$LABEL $i/$N] PASS (${boot_secs}s)"
        fi
        continue
    fi

    msg="$(grep -aE 'EXTINCTION|tests: [0-9]+/[0-9]+ (FAIL|fail)' "$LOG" | head -1)"
    token="$(harness_result_token "$HARNESS_LOG")"
    case "$(classify_boot "$LOG" "$HARNESS_LOG")" in
    CORRUPT)
        corrupt=$((corrupt+1)); capture CORRUPT "$i"
        echo "  [$LABEL $i/$N] CORRUPTION: ${msg:-<no extinction line>}  (${boot_secs}s)"
        ;;
    EXTKILL)
        extkill=$((extkill+1)); capture "EXTKILL-$token" "$i"
        tline="$(grep -aoE 'tests: [0-9]+/[0-9]+ PASS' "$LOG" | head -1)"
        if grep -aq '^EXTINCTION:' "$LOG" || grep -aqE "$SUITE_FAIL_RE" "$LOG"; then
            health="guest had failed before the kill: ${msg:-?}"
        elif [[ -n "$tline" ]]; then
            health="guest was healthy: $tline, 0 EXTINCTION"
        else
            health="guest healthy so far, 0 EXTINCTION (suite not yet reached)"
        fi
        if grep -aqE "$EXTKILL_RE" "$LOG"; then
            # Arm 1 -- QEMU's own report, which names the signal AND the sender.
            kline="$(grep -aE "$EXTKILL_RE" "$LOG" | head -1)"
            kline="${kline#*terminating on }"
            kpid="$(printf '%s\n' "$kline" | grep -aoE 'pid [0-9]+' | head -1)"
            kpid="${kpid#pid }"
            echo "  [$LABEL $i/$N] EXTERNAL-KILL [qemu-report]: $kline ($health) (${boot_secs}s, result=$token)"
            # The sender may still be alive seconds after the kill; by the time
            # an operator investigates, the pid is long gone -- capture it NOW.
            sender="$(ps -o pid=,ppid=,comm=,args= -p "$kpid" 2>/dev/null | head -1)"
            [[ -z "$sender" ]] && sender="pid $kpid already exited at classify time"
            echo "      sender| $sender"
            echo "sender at classify time: $sender" >> "$FAILDIR/$LABEL-$i-EXTKILL-$token-harness.log" || true
        else
            # Arm 2 -- SIGKILL. State plainly that the sender is unrecoverable
            # rather than printing an empty ps and letting it read as "nobody".
            kline="$(grep -aoE "$HARNESS_SIGKILL_RE.*" "$HARNESS_LOG" | head -1)"
            echo "  [$LABEL $i/$N] EXTERNAL-KILL [sigkill]: ${kline:-<shell signal notification>} ($health) (${boot_secs}s, result=$token)"
            echo "      sender| NOT RECOVERABLE -- SIGKILL is uncatchable, so QEMU printed no 'from pid' report and the shell's notification carries no sender (#200)."
        fi
        ;;
    INJECT)
        inject=$((inject+1)); capture INJECT "$i"
        cp "$INJECT_LOG" "$FAILDIR/$LABEL-$i-INJECT-injector.log" 2>/dev/null || true
        echo "  [$LABEL $i/$N] inject-miss (harness delivery; guest green)  (${boot_secs}s)"
        ;;
    TIMING)
        timing=$((timing+1)); capture TIMING "$i"
        echo "  [$LABEL $i/$N] timing (benign host-fragility): ${msg:-?}  (${boot_secs}s)"
        ;;
    *)
        other=$((other+1)); capture "OTHER-$token" "$i"
        # The harness tail is usually the whole story for an OTHER (the
        # guest log ends healthy; the failed step is a post-banner gate).
        # result= separates "QEMU vanished" from "a post-banner gate failed",
        # which OTHER used to conflate.
        echo "  [$LABEL $i/$N] OTHER fail [result=$token]: ${msg:-<unclassified>}  (${boot_secs}s)"
        tail -3 "$HARNESS_LOG" 2>/dev/null | sed 's/^/      harness| /'
        ;;
    esac
done

echo "== $LABEL: $pass PASS / $corrupt CORRUPTION / $extkill external-kill / $inject inject-miss / $timing timing / $other other  (N=$N) =="
echo "== $LABEL: exposure ${boot_secs_total}s total, ~$(( boot_secs_total / (N > 0 ? N : 1) ))s mean/boot; per-boot:$boot_secs_list =="
[[ $corrupt -eq 0 && $extkill -eq 0 && $other -eq 0 ]]
