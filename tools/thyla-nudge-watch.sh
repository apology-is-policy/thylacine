#!/usr/bin/env bash
# Press enter on the far side of a self-compaction.
#
#   thyla-nudge-watch.sh <pane>        watch, then type the nudge
#
# WHY THIS IS A SEPARATE, DETACHED PROCESS
#
# A SessionStart hook's additionalContext is PASSIVE: it seeds a context, it
# does not submit a turn. So the resumed session wakes holding a note that says
# "no one is waiting on you" -- and waits. Something has to press enter, and
# nothing inside either session can: the old one is gone, the new one is the
# thing being woken.
#
# THE TIMING IS THE WHOLE PROBLEM, and it is not what it looks like. The first
# fix queued the nudge directly behind the /compact, on the theory that the
# client's input queue would hold both. IT DOES hold both -- measured, with a
# /copy standing in for the /compact: the second message arrived intact. What
# it does NOT survive is the compaction itself. A message queued BEFORE the
# rebuild belongs to the session being torn down and goes with it; a message
# typed DURING the rebuild lands in the one being built and submits when it
# finishes. That is a two-second difference in when the keystrokes are sent,
# and it is the entire difference between waking and not.
#
# So this waits for the client to actually enter the compacting state, then
# types. Keyed on the observable transition, NOT on a guessed delay -- the
# compaction's duration varies with the summary, and a sleep long enough to be
# safe is also long enough to miss the window on a fast one.
#
# It must be DETACHED (nohup, stdin closed) for the obvious reason that it has
# to outlive the context that starts it, and for a less obvious one: the harness
# stops its own background tasks, and did so three times in one day.
#
# NOT setsid -- macOS does not ship it. The first attempt used it, so the
# watcher died instantly with "setsid: No such file or directory", and the
# negative control (nothing fired) PASSED, because a process that never starts
# also never fires. Only the second leg of that control -- is it still alive? --
# caught it. A negative assertion is satisfied by a broken fixture.
set -uo pipefail

PANE="${1:?usage: thyla-nudge-watch.sh <tmux-pane>}"

# The markers are DEFAULTS HERE ON PURPOSE, never arguments. The pane renders
# the command line that launched this, so a marker passed as an argument would
# appear in the very text being searched and match itself instantly -- the
# same self-match trap the compact script's header documents. Keeping the
# literal inside the file keeps it out of the pane.
#
# Matched case-insensitively on the STEM rather than the full phrase, because
# neither string has ever been observed in the pane -- they are read off the
# client's behaviour, and the only way to see the real one is to run a real
# compaction. A stem survives the wording being "Compacting conversation...",
# "Compacting...", or sentence-cased differently; the full phrase does not. The
# two stems stay distinct ("compacted" does not contain "compacting"), so the
# running and finished states cannot be confused for one another.
MARK_RUNNING="${THYLA_NUDGE_MARKER:-compacting}"
MARK_DONE="${THYLA_NUDGE_DONE_MARKER:-compacted}"

NUDGE="${THYLA_NUDGE:-[auto-nudge] Continue from your resume note.}"
BOUND="${THYLA_NUDGE_BOUND:-900}"     # give up after this; a bound, not a timer
SETTLE="${THYLA_NUDGE_SETTLE:-2}"     # let the rebuild take the input box first
POLL="${THYLA_NUDGE_POLL:-0.5}"
# Only the bottom of the screen counts. The marker is a STATUS-AREA string, but
# the same words in ordinary prose match just as well -- and the message written
# immediately before a compaction is overwhelmingly likely to be ABOUT the
# compaction, so a whole-pane search is close to guaranteed to match the agent
# talking rather than the client working. Scoping to the chrome at the bottom
# costs nothing and removes the whole class.
TAILN="${THYLA_NUDGE_TAIL:-12}"
STATE_DIR="${THYLA_SELFCOMPACT_DIR:-$HOME/.claude/thyla-selfcompact}"

log() {
    mkdir -p "$STATE_DIR" 2>/dev/null
    printf '%s\t%s\t%s\t%s\t%s\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "nudge" "$PANE" "$1" "${2:-}" \
        >> "$STATE_DIR/log.tsv" 2>/dev/null
}

start=$(date +%s)
trigger=""
while :; do
    elapsed=$(( $(date +%s) - start ))
    if [ "$elapsed" -ge "$BOUND" ]; then
        # Deliberately does NOT send. Never having seen the compaction means
        # the turn is still running or the compact never fired, and a stray
        # message injected into a live session is worse than a missed wake --
        # it arrives looking like the operator asking for something.
        log nudge-timeout "no compaction seen in ${BOUND}s; sent nothing"
        exit 1
    fi

    pane=$(tmux capture-pane -p -t "$PANE" 2>/dev/null | tail -n "$TAILN" || true)
    if printf '%s' "$pane" | grep -qiF "$MARK_RUNNING"; then
        trigger="running"; break
    fi
    # The fallback, and it is a real one rather than a consolation: if polling
    # missed the compacting window entirely (a short summary, a slow poll), the
    # finished state is just as good a moment to type -- the session is up and
    # idle. Catching either state means the window cannot be missed by being
    # slightly late, only by being absent.
    if printf '%s' "$pane" | grep -qiF "$MARK_DONE"; then
        trigger="done"; break
    fi
    sleep "$POLL"
done

sleep "$SETTLE"

# No C-u. The box is empty here by construction (the Enter that submitted
# /compact emptied it), and C-u would silently destroy anything the operator
# had started typing -- which is exactly what an operator does during a
# compaction, since typing-during-compaction is the manual version of this.
tmux send-keys -t "$PANE" "$NUDGE"
tmux send-keys -t "$PANE" C-m
log nudge-sent "trigger=$trigger after ${elapsed}s"
