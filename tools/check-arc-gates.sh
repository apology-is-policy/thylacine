#!/usr/bin/env bash
# tools/check-arc-gates.sh -- propagate the DISTRO D-5 / LINEAGE L-6c arc-gate
# state into a verdict (#212).
#
# WHY THIS EXISTS. Both arc gates SOFT-SKIP when their external Alpine bundle
# is absent -- the correct disposition (a missing input is not a broken kernel,
# and the tarball is deliberately untracked, so a fresh clone and any hermetic
# build have none). But `tools/test.sh` keys its verdict on the boot banner
# alone, so the skip never reached the exit status: a tree with a REGRESSED
# D-1..D-4 chain exited 0 identically to one where the whole arc ran. The
# arc's headline claim was opt-in and its absence invisible. "An assertion that
# never executed is an UNKNOWN, not a pass" -- and the UNKNOWN was reported as
# PASS.
#
# So joey now emits one structured line and this reads it:
#
#     joey: ARC-GATES l6c=<state> d5=<state>
#     joey: ARC-GATES not-built (THYLA_BOOT_PROBES=OFF)
#
# WHAT THIS DOES AND DOES NOT BUY. It makes a skip a VISIBLE, DELIBERATE,
# REPORTED state; it does not manufacture coverage. With the fixture absent the
# arc is still unverified -- the difference is that the report now says so
# instead of the exit status silently swallowing it. `--require` is for the
# shapes that DO have the fixture and want a skip to be fatal; it is opt-out of
# leniency rather than opt-in to checking, because a fresh clone must still be
# able to run the suite.
#
# The teeth in the DEFAULT mode are on the third state: an ABSENT report line,
# or a gate that reports MISSING, fails. That is the regression shape a green
# boot could otherwise hide -- a dropped gate call, a gate that stopped
# reporting, or a boot that never reached the arc at all.
#
# Exit: 0 ok (incl. a reported skip), 1 verdict failure, 2 usage/IO.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

REPORT_RE='joey: ARC-GATES '

# The prose corroboration. joey prints BOTH the structured state and its own
# human line from the same arm, so a disagreement between them means a state
# was recorded on a path that does not print it (or the reverse) -- a real
# defect in the reporting itself, which is the thing being trusted here.
#
# Matched on the full "GATE PASS" / "gate SKIPPED" tail, never on the gate name
# alone: the FAILURE prose contains the gate name too, so a looser pattern
# would let a failed gate corroborate a PASS claim (#186 -- never key a verdict
# on a string the failure path can also emit).
L6C_PASS_PROSE='joey: L-6c ALPINE SHELL GATE PASS'
L6C_SKIP_PROSE='joey: L-6c Alpine shell gate SKIPPED'
D5_PASS_PROSE='joey: D-5 STOCK DISTRO GATE PASS'
D5_SKIP_PROSE='joey: D-5 stock distro gate SKIPPED'

# THYLA_ARC_GATES=require is the env spelling of --require, so `tools/test.sh`
# (and anything that shells out to it -- the SMP gate, the interactive harness)
# can demand the gates ran without every caller learning a new flag.
require=0
[[ "${THYLA_ARC_GATES:-}" == "require" ]] && require=1
log=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --require)  require=1; shift ;;
        --selftest) selftest=1; shift ;;
        -h|--help)
            echo "usage: $0 [--require] <boot-log>"
            echo "       $0 --selftest"
            exit 0
            ;;
        -*) echo "check-arc-gates: unknown option: $1" >&2; exit 2 ;;
        *)  log="$1"; shift ;;
    esac
done

# check_log <path> -- prints the one-line report on stdout, returns 0/1.
# Every failure arm prints WHY on stderr; the caller needs no log of its own.
check_log() {
    local path="$1"
    local line l6c d5 ran=0 skipped=0 blocked=0

    if [[ ! -r "$path" ]]; then
        echo "check-arc-gates: cannot read boot log: $path" >&2
        return 2
    fi

    # LAST occurrence: a log that somehow holds more than one boot is answered
    # for by its most recent one, never by the friendliest one.
    line="$(grep -a "$REPORT_RE" "$path" | tail -1 || true)"

    if [[ -z "$line" ]]; then
        echo "check-arc-gates: FAIL -- the boot emitted no 'joey: ARC-GATES' report line." >&2
        echo "    Every build shape emits one: the probe ladder reports the two gates'" >&2
        echo "    state, and a --production build reports 'not-built'. Its absence means" >&2
        echo "    the boot never reached the arc gates, or the report was removed --" >&2
        echo "    which is exactly the silent-skip this check exists to end." >&2
        return 1
    fi

    if [[ "$line" == *"not-built"* ]]; then
        echo "arc gates: NOT BUILT (THYLA_BOOT_PROBES=OFF -- the lean production shape; NOT coverage)"
        if (( require )); then
            echo "check-arc-gates: FAIL -- --require given, but this build has no arc gates." >&2
            return 1
        fi
        return 0
    fi

    l6c="$(sed -n 's/.*ARC-GATES l6c=\([A-Za-z-]*\).*/\1/p' <<<"$line")"
    d5="$(sed -n 's/.*ARC-GATES .*d5=\([A-Za-z-]*\).*/\1/p' <<<"$line")"
    if [[ -z "$l6c" || -z "$d5" ]]; then
        echo "check-arc-gates: FAIL -- unparseable report line: $line" >&2
        return 1
    fi

    # one_gate <label> <state> <pass-prose> <skip-prose>
    # Unknown states FAIL. A checker that shrugs at a token it does not know is
    # a checker that fails open the first time the producer grows a state.
    one_gate() {
        local label="$1" state="$2" pass_prose="$3" skip_prose="$4"
        case "$state" in
        PASS)
            if ! grep -aqF "$pass_prose" "$path"; then
                echo "check-arc-gates: FAIL -- $label reports PASS but the log has no '$pass_prose'." >&2
                echo "    The structured state and joey's own prose disagree; one of the two lies." >&2
                return 1
            fi
            ran=$((ran+1)) ;;
        SKIPPED)
            if ! grep -aqF "$skip_prose" "$path"; then
                echo "check-arc-gates: FAIL -- $label reports SKIPPED but the log has no '$skip_prose'." >&2
                return 1
            fi
            skipped=$((skipped+1)) ;;
        KNOWN-BLOCKED)
            blocked=$((blocked+1)) ;;
        MISSING)
            echo "check-arc-gates: FAIL -- $label state is MISSING: the gate was never called," >&2
            echo "    or returned without recording a disposition. A dropped gate reads as a" >&2
            echo "    green boot otherwise." >&2
            return 1 ;;
        *)
            echo "check-arc-gates: FAIL -- $label has unrecognised state '$state'." >&2
            return 1 ;;
        esac
        return 0
    }

    one_gate "L-6c" "$l6c" "$L6C_PASS_PROSE" "$L6C_SKIP_PROSE" || return 1
    one_gate "D-5"  "$d5"  "$D5_PASS_PROSE"  "$D5_SKIP_PROSE"  || return 1

    local report="arc gates: $ran/2 ran -- L-6c=$l6c D-5=$d5"
    if (( skipped )); then
        report="$report; $skipped SKIPPED (external Alpine fixture absent -- NOT a guest result, and NOT coverage)"
    fi
    if (( blocked )); then
        report="$report; $blocked KNOWN-BLOCKED (deliberately non-fatal -- NOT coverage)"
    fi
    echo "$report"

    if (( require && (skipped || blocked) )); then
        echo "check-arc-gates: FAIL -- --require given, but $((skipped+blocked)) gate(s) did not run." >&2
        return 1
    fi
    return 0
}

# --------------------------------------------------------------------------
# --selftest: drive the real check_log over synthetic logs.
#
# It proves DISCRIMINATION, not detection. The load-bearing case is D (a
# pre-#212 log, which is what every boot before this change looked like): if
# that passed, this check would be decorative. The other negatives exist
# because an arm that matches everything is not a detector -- case G feeds a
# state that CLAIMS pass with no prose to back it, and case J feeds the failure
# prose against a PASS claim, which is what a too-loose pattern would swallow.
# --------------------------------------------------------------------------
if [[ "${selftest:-0}" == "1" ]]; then
    TMP="$(mktemp -d)" || { echo "check-arc-gates: mktemp failed" >&2; exit 2; }
    trap 'rm -rf "$TMP"' EXIT
    fails=0
    checked=0
    # Pin the mode: the selftest drives BOTH settings explicitly below, so an
    # ambient THYLA_ARC_GATES must not decide what cases A..J mean.
    require=0

    # expect <name> <want-rc> <want-substring-in-stdout|-> <log-body>
    expect() {
        local name="$1" want_rc="$2" want_out="$3" body="$4"
        local f="$TMP/$name.log" out rc
        printf '%s\n' "$body" >"$f"
        out="$(check_log "$f" 2>/dev/null)" && rc=0 || rc=$?
        checked=$((checked+1))
        if [[ "$rc" != "$want_rc" ]]; then
            printf '  FAIL %-42s rc=%s (want %s)\n' "$name" "$rc" "$want_rc"
            fails=$((fails+1)); return
        fi
        if [[ "$want_out" != "-" && "$out" != *"$want_out"* ]]; then
            printf '  FAIL %-42s out=%q (want substring %q)\n' "$name" "$out" "$want_out"
            fails=$((fails+1)); return
        fi
        printf '  ok   %-42s rc=%s %s\n' "$name" "$rc" "${out:-<no report>}"
    }

    PASS_BOTH="$L6C_PASS_PROSE (busybox sh forked)
$D5_PASS_PROSE (an unmodified Alpine)
joey: ARC-GATES l6c=PASS d5=PASS"
    SKIP_BOTH="$L6C_SKIP_PROSE (no bundle)
$D5_SKIP_PROSE (no bundle)
joey: ARC-GATES l6c=SKIPPED d5=SKIPPED"
    MIXED="$L6C_PASS_PROSE (busybox sh forked)
$D5_SKIP_PROSE (no bundle)
joey: ARC-GATES l6c=PASS d5=SKIPPED"

    echo "== check-arc-gates selftest =="
    expect A-both-pass        0 "2/2 ran"       "$PASS_BOTH"
    expect B-both-skipped     0 "0/2 ran"       "$SKIP_BOTH"
    expect B-skip-not-cov     0 "NOT coverage"  "$SKIP_BOTH"
    expect C-mixed            0 "1/2 ran"       "$MIXED"
    # D: a pre-#212 log -- the gates ran and said so in prose, but nothing
    # propagated it. This MUST fail, or the whole change is decorative.
    expect D-no-report-line   1 -               "$L6C_PASS_PROSE (busybox sh forked)
$D5_PASS_PROSE (an unmodified Alpine)
Thylacine boot OK"
    # E and I carry the L-6c PASS prose deliberately: without it they would go
    # red on L-6c's corroboration check and never reach the d5 state under
    # test -- a red result for the wrong reason, which is not a passing test.
    expect E-missing-state    1 -               "$L6C_PASS_PROSE (busybox sh forked)
joey: ARC-GATES l6c=PASS d5=MISSING"
    expect F-not-built        0 "NOT BUILT"     "joey: ARC-GATES not-built (THYLA_BOOT_PROBES=OFF)"
    # G: state claims PASS, no prose backs it.
    expect G-pass-no-prose    1 -               "joey: ARC-GATES l6c=PASS d5=PASS"
    expect H-known-blocked    0 "KNOWN-BLOCKED" "$D5_PASS_PROSE (an unmodified Alpine)
joey: ARC-GATES l6c=KNOWN-BLOCKED d5=PASS"
    expect I-unknown-token    1 -               "$L6C_PASS_PROSE (busybox sh forked)
joey: ARC-GATES l6c=PASS d5=WEDGED"
    # J: the FAILURE prose must not corroborate a PASS claim.
    expect J-failure-prose    1 -               "joey: D-5 STOCK DISTRO GATE FAILED first-missing=DISTRO-A-stock-sh status=1
$L6C_PASS_PROSE (busybox sh forked)
joey: ARC-GATES l6c=PASS d5=PASS"
    # K: --require turns a reported skip into a failure; the SAME log passes
    # without it (case B above), so this isolates the flag, not the log.
    require=1
    expect K-require-skipped  1 -               "$SKIP_BOTH"
    expect K-require-pass     0 "2/2 ran"       "$PASS_BOTH"
    expect K-require-notbuilt 1 -               "joey: ARC-GATES not-built (THYLA_BOOT_PROBES=OFF)"
    require=0
    # L: the last report wins -- a log holding two boots is answered for by
    # its most recent one, not by the friendliest.
    expect L-last-wins        1 -               "$PASS_BOTH
joey: ARC-GATES l6c=PASS d5=MISSING"

    echo "== $checked checked, $fails failed =="
    [[ "$fails" == "0" ]]
    exit $?
fi

if [[ -z "$log" ]]; then
    # Default to the harness's own boot log, so a bare invocation answers for
    # the run that just happened.
    log="$REPO_ROOT/build/test-boot.log"
fi

check_log "$log"
exit $?
