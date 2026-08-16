#!/usr/bin/env bash
# Self-compaction: type /compact into my own tmux pane, having first proved it
# is safe and earned.
#
#   thyla-selfcompact.sh --check [reason]   report the verdict, change nothing
#   thyla-selfcompact.sh [reason]           compact, if every precondition holds
#
# WHY THIS EXISTS: /compact is mechanical work a human was doing by hand at
# every context boundary, plus a manual re-paste of the last message. The paste
# is automated by ~/.claude/resume-note.py; this is the other half.
#
# EVERY LINK WAS MEASURED BEFORE THIS WAS WRITTEN, not assumed:
#   * tmux sets $TMUX_PANE to a unique pane id in every process it starts, so a
#     pane can address ITSELF with no naming scheme to keep in sync.
#   * send-keys reaches my own input box (probed with a runtime-computed value,
#     against a control proving it was absent beforehand -- grepping for a
#     literal self-matches, because the pane renders the command containing it).
#   * C-u retracts it cleanly.
#   * Enter submits, and a leading `/` executes as a COMMAND rather than
#     arriving as literal text -- proven with `/copy`, verified out-of-band by
#     the clipboard and by /tmp/claude-501/response.md carrying the marker.
#
# THE GOVERNOR IS THE POINT, NOT THE KEYSTROKES. An unattended compact/resume
# cycle can run forever, and the dangerous shape is not a runaway -- it is a
# QUIET one: hit a problem, compact, come back with less context, fail the same
# way, compact again. Each turn looks like progress and none is. A simple
# iteration cap cannot catch that, because the pathological case sits under it.
# So the gate is EVIDENCE OF PROGRESS: HEAD must have moved since the last
# self-compact. Two consecutive compactions with a static HEAD belay the
# mechanism and hand back to the operator, who is the only one who can tell
# "stuck" from "thinking".
#
# DEFAULT IS REFUSE. Every precondition failure prints why and exits non-zero.
set -uo pipefail

STATE_DIR="${THYLA_SELFCOMPACT_DIR:-$HOME/.claude/thyla-selfcompact}"   # overridable so the gate can be tested without touching real state
LOG="$STATE_DIR/log.tsv"
YIP="$HOME/.local/bin/yip"
BELAY_AT=2                      # consecutive no-progress compactions tolerated

CHECK=0
[ "${1:-}" = "--check" ] && { CHECK=1; shift; }
REASON="${*:-unspecified}"

say()  { printf '  %s\n' "$*"; }
deny() { printf 'thyla-selfcompact: REFUSED -- %s\n' "$*" >&2; exit 1; }

# --- who am I -------------------------------------------------------------
ROLE=$(tmux show-options -pv @thyla-role 2>/dev/null || true)
[ -n "$ROLE" ] || ROLE=$(basename "$(git rev-parse --show-toplevel 2>/dev/null || echo unknown)")
STATE="$STATE_DIR/$ROLE.state"

# --- preconditions --------------------------------------------------------
[ -n "${TMUX_PANE:-}" ] || deny "not inside tmux -- there is no pane to type into"
command -v tmux >/dev/null || deny "tmux is not installed"
git rev-parse --git-dir >/dev/null 2>&1 || deny "not in a git worktree; cannot prove progress"

HEAD=$(git rev-parse HEAD 2>/dev/null) || deny "cannot read HEAD"
DIRTY=$(git status --porcelain --untracked-files=no | wc -l | tr -d ' ')
[ "$DIRTY" = 0 ] || deny "$DIRTY uncommitted tracked change(s) -- commit first; a compaction is only free when the handoff is already current"

# --- the progress gate ----------------------------------------------------
LAST_HEAD=""; NOPROG=0
if [ -f "$STATE" ]; then
    # shellcheck disable=SC1090
    . "$STATE" 2>/dev/null || true
    LAST_HEAD="${last_head:-}"; NOPROG="${no_progress:-0}"
fi

if [ -z "$LAST_HEAD" ]; then
    VERDICT="allow"; WHY="first self-compaction for role '$ROLE'"; NEW_NOPROG=0
elif [ "$HEAD" != "$LAST_HEAD" ]; then
    VERDICT="allow"; WHY="HEAD moved ${LAST_HEAD:0:8} -> ${HEAD:0:8}"; NEW_NOPROG=0
else
    NEW_NOPROG=$((NOPROG + 1))
    if [ "$NEW_NOPROG" -ge "$BELAY_AT" ]; then
        VERDICT="belay"; WHY="HEAD has not moved (${HEAD:0:8}) across $NEW_NOPROG consecutive self-compactions"
    else
        VERDICT="allow"; WHY="HEAD static at ${HEAD:0:8} (${NEW_NOPROG}/${BELAY_AT} -- one more without progress belays)"
    fi
fi

# --- report ---------------------------------------------------------------
say "role     : $ROLE   pane $TMUX_PANE"
say "HEAD     : ${HEAD:0:8}   (last self-compact: ${LAST_HEAD:0:8}${LAST_HEAD:+ })"
say "tree     : clean"
say "reason   : $REASON"
say "verdict  : $VERDICT -- $WHY"

# Not a gate, but the operator should see it: a compaction does not stop these,
# and a detached job outlives the context that knows why it was started.
RUNNING=$(ps -eo pid=,comm= | awk '{n=$2; sub(/.*\//,"",n)} n=="qemu-system-aarch64"||n=="java"{print n}' | sort | uniq -c | tr '\n' ' ')
[ -n "$RUNNING" ] && say "running  : $RUNNING(survives the compaction -- make sure the note says so)"

if [ "$VERDICT" = "belay" ]; then
    say ""
    say "BELAYED. Two sessions of self-compaction with nothing landing is the"
    say "signature of a loop that looks busy, and only the operator can tell"
    say "'stuck' from 'thinking'. Hand back instead: say what was attempted and"
    say "what it needs. Clear by landing a commit, or reset with:"
    say "    rm $STATE"
    exit 3
fi

[ "$CHECK" = 1 ] && { say ""; say "(--check: nothing sent)"; exit 0; }

# --- commit the decision BEFORE acting on it ------------------------------
# Written first on purpose: if the compaction lands and this did not, the next
# invocation would see no history and re-allow forever.
mkdir -p "$STATE_DIR"
printf 'last_head=%s\nno_progress=%s\n' "$HEAD" "$NEW_NOPROG" > "$STATE"
printf '%s\t%s\t%s\t%s\t%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$ROLE" "${HEAD:0:8}" "$VERDICT" "$REASON" >> "$LOG"

# The peers' half: a compacted agent has lost the TEXTURE of recent exchanges
# even where the facts survive, so a peer should re-state rather than assume.
[ -x "$YIP" ] && "$YIP" busy "compacted at ${HEAD:0:8} -- context reset, re-state anything subtle" >/dev/null 2>&1

# --- do it ----------------------------------------------------------------
tmux send-keys -t "$TMUX_PANE" C-u          # never submit residue
tmux send-keys -t "$TMUX_PANE" "/compact"
tmux send-keys -t "$TMUX_PANE" C-m          # fires when this turn ends
say ""
say "/compact queued -- it submits when this turn ends."
say "Make this turn's final message the resume note: what is in flight, and"
say "what must NOT be redone. resume-note.py re-injects it on the far side."
