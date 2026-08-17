#!/usr/bin/env bash
# Stop hook -- ask whether ending the turn is actually a decision.
#
# CLAUDE.md's guided-autonomy rule says a checkpoint is not a stopping point:
# under granted autonomy you land a chunk, report it, and open the next one in
# the same run, and the signal that ends the run is the 600k CHECKPOINT WINDOW.
# That rule was behavioural only -- CLAUDE.md itself noted the mechanism that
# would make it structural is a Stop hook, "deliberately not built". This is it,
# built at the user's request 2026-08-16 after a run stopped at a checkpoint it
# should have run through, having written the very `Ahead` line CLAUDE.md names
# as the tell.
#
# IT ASKS, IT DOES NOT VETO. There are real reasons to stop, and the model is
# the only thing here that can tell them apart from an unearned yield. So this
# blocks ONCE with a question and the legitimate exits listed; answering "yes,
# because X" and stopping is a correct outcome, not a defeat.
#
# FAIL-OPEN, EVERYWHERE. Every error path exits 0 (allow the stop). A Stop hook
# that fails closed can trap a session in a loop it cannot talk its way out of,
# which is far worse than a missed nudge -- so no parse failure, missing file,
# or unreadable transcript may ever produce a block.
#
# THE THREE THINGS THAT KEEP IT FROM NAGGING
#   1. stop_hook_active -- set when this hook already blocked and the model is
#      continuing. Fire again there and it loops forever. Exit 0 immediately.
#   2. A context FLOOR. Below it the session is conversational (the user asked
#      something, you answered), and stopping is the expected shape. The rule
#      targets long unattended runs, not replies.
#   3. A context CEILING at the checkpoint line. At or above it, stopping is
#      what CLAUDE.md actually wants -- ctx-hook.sh has already said so, and a
#      second voice contradicting it would be worse than silence.
#
# The turn-count test is what separates "answering the user" from "running":
# many assistant turns since the last GENUINE user message means autonomous
# work, where a stop is a decision. One or two means a reply.
set -u
FLOOR="${STOP_FLOOR:-120000}"        # below: conversational, stay silent
CKPT="${CTX_CKPT:-600000}"           # at/above: stopping is correct, stay silent
LIMIT="${CTX_LIMIT:-900000}"
MIN_TURNS="${STOP_MIN_TURNS:-6}"     # assistant turns since the user last spoke
# ONE source for the token, used to EMIT the block below and to DETECT it in the
# transcript above. Two copies would drift the moment the prose is reworded, and
# the detector would silently stop finding its own blocks.
STEM="STOP CHECK"
LEDGER="${THYLA_SELFCOMPACT_DIR:-$HOME/.claude/thyla-selfcompact}/log.tsv"

# EVERY EXIT PATH LEAVES A ROW. Without this the hook has NINE silent exits and
# they are one observation from outside: "correctly silent", "suppressed by
# stop_hook_active", and "crashed in the parser" are indistinguishable, so the
# only way to explain a missed firing is to guess. That is not hypothetical --
# it cost a full investigation on 2026-08-17 (a second stop at 530k/73 turns
# that should have fired and left no trace of why), and the same shape had just
# cost the self-compaction slot a day of a stranded vault session.
#
# The rule the two share: a guard whose failure mode is SILENCE cannot be
# debugged, only theorised about. So the instrument comes before the fix.
#
# Never fatal, never noisy on stderr: a logging failure must not change the
# decision, and this hook's whole contract is that it fails OPEN.
# The pane IS the session id. The ledger is SHARED by main/aux/vault, and the
# first real rows it produced were three stops in 24 seconds with incoherent
# context jumps -- two of them another track's, indistinguishable from mine.
# An interleaved log with no writer is not evidence, it is noise that looks
# like evidence; the self-compaction side already stamps `pane=` for exactly
# this reason, so this uses the same identity rather than inventing one.
led() {
    { printf '%s\tstop%s\t%s\t%s\t%s\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "${TMUX_PANE:+:$TMUX_PANE}" \
        "${2:-?}" "$1" "${3:-}" >> "$LEDGER"; } 2>/dev/null || true
}

IN=$(cat 2>/dev/null) || { led silent-no-stdin; exit 0; }

# 1. Already blocked once this stop -- never block twice.
ACTIVE=$(printf '%s' "$IN" | python3 -c 'import sys,json
try: print("1" if json.load(sys.stdin).get("stop_hook_active") else "0")
except Exception: print("E")' 2>/dev/null) || { led silent-active-parse-failed; exit 0; }
# "E", not "1", on exception. Both still fail OPEN -- an unparsable stdin must
# never block -- but they are now DIFFERENT ROWS. The first cut printed "1"
# here, which made a parse crash indistinguishable from a live flag in the very
# ledger built to tell causes apart; the test caught it, because the malformed
# -stdin leg logged `silent-stop-hook-active`. An instrument with a blind spot
# exactly where the ambiguity lives is the thing it was supposed to replace.
[ "$ACTIVE" = "E" ] && { led silent-stdin-unparsable; exit 0; }

# THE FLAG IS NO LONGER A VETO, and that is the fix (2026-08-17, with aux).
#
# `stop_hook_active` means "this hook already triggered a continuation" -- it is
# per-CONTINUATION by definition, so it stays true for every later stop in the
# same run. Exiting on it therefore made this a once-per-RUN nudge that guards
# the FIRST stop and nothing after, which is backwards: the first stop is the
# one most likely to be earned. Measured: a stop at 530k/73 turns, squarely
# in-window, silenced with no trace.
#
# I kept the early exit because I believed it was the loop guard. It is not.
# The harness overrides a Stop hook after EIGHT consecutive blocks without
# progress (`CLAUDE_CODE_STOP_HOOK_BLOCK_CAP`), so the belay already exists one
# level up -- which frees the flag to be one CONJUNCT instead of a gate.
#
# So: silent only for the IMMEDIATE re-stop after answering (few turns since
# the block), and asking again once real work has happened. See the decision
# below; `turns_block` is what makes it structural rather than remembered.

T=$(printf '%s' "$IN" | python3 -c 'import sys,json
try: print(json.load(sys.stdin).get("transcript_path",""))
except Exception: print("")' 2>/dev/null) || { led silent-path-parse-failed; exit 0; }
{ [ -z "$T" ] || [ ! -f "$T" ]; } && { led silent-no-transcript; exit 0; }

# Context used + assistant turns since the last genuine user message. Same
# byte-bounded tail + compaction anchoring as ctx-hook.sh, so the two agree on
# the budget rather than offering the model two different numbers.
READ=$(tail -c 8388608 "$T" 2>/dev/null | STOP_STEM="$STEM" python3 -c '
import sys, json
import os
STEM = os.environ.get("STOP_STEM", "STOP CHECK")
last = 0
turns = 0      # assistant turns since the last GENUINE user message
tblock = 0     # assistant turns since this hook last blocked
for line in sys.stdin:
    s = line.strip()
    if not s:
        continue
    try:
        o = json.loads(s)
    except Exception:
        continue
    if o.get("isCompactSummary") or o.get("compact"):
        last = 0
        turns = 0
        tblock = 0
        continue
    t = o.get("type")
    msg = o.get("message") or {}
    if t == "assistant":
        turns += 1
        tblock += 1
    elif t == "user":
        c = msg.get("content")
        # A GENUINE user turn, not a tool result and not a system notice. Tool
        # results arrive as user-type events carrying tool_result blocks; the
        # harness also injects notifications as user text. Counting either as
        # "the user spoke" would reset the turn counter constantly and silence
        # the hook exactly during the long autonomous runs it is for.
        if isinstance(c, list):
            if any((b or {}).get("type") == "tool_result" for b in c if isinstance(b, dict)):
                continue
            text = " ".join((b or {}).get("text", "") for b in c if isinstance(b, dict))
        elif isinstance(c, str):
            text = c
        else:
            continue
        # A TEXTLESS user record is not speech. Joined content of "" contains
        # none of the markers below, so it used to fall through to `turns = 0`
        # -- an image-only or reminder-only event silently zeroed the counter.
        if not text.strip():
            continue
        if "SYSTEM NOTIFICATION" in text or "<task-notification>" in text:
            continue
        if "local-command-caveat" in text or text.strip().startswith("[auto-nudge]"):
            continue
        # A BLOCK FROM THIS HOOK lands in the transcript as plain user text and
        # matched no filter, so it reset the "since the user spoke" counter --
        # making the number the message reports mean "since the HOOK spoke".
        # It resets the BLOCK counter instead, which is what it actually marks.
        if STEM in text:
            tblock = 0
            continue
        turns = 0
    u = msg.get("usage") or {}
    v = (u.get("input_tokens", 0) + u.get("cache_read_input_tokens", 0)
         + u.get("cache_creation_input_tokens", 0))
    if v > 0:
        last = v
print("%d %d %d" % (last or 0, turns, tblock))
' 2>/dev/null) || { led silent-replay-failed; exit 0; }

CTX=$(printf '%s' "$READ" | awk '{print $1}')
TURNS=$(printf '%s' "$READ" | awk '{print $2}')
TBLOCK=$(printf '%s' "$READ" | awk '{print $3}')
case "$CTX$TURNS$TBLOCK" in *[!0-9]*|"") led silent-nonnumeric; exit 0 ;; esac
STATE="${CTX}ctx/${TURNS}t/${TBLOCK}b/flag${ACTIVE}"

# 2/3. Outside the window this rule is about -- say nothing.
[ "$CTX" -lt "$FLOOR" ] && { led silent-below-floor "$STATE"; exit 0; }
[ "$CTX" -ge "$CKPT" ] && { led silent-at-checkpoint "$STATE"; exit 0; }
[ "$TURNS" -lt "$MIN_TURNS" ] && { led silent-few-turns "$STATE"; exit 0; }
# The one case the flag legitimately silences: an immediate re-stop after this
# hook just asked. Past that, real work has happened and the question is live
# again. If the block record has fallen out of the tail window, `tblock` reads
# as the whole window and this passes -- re-asking, bounded by the harness cap,
# and visibly, which is the safe direction to fail.
[ "$ACTIVE" = "1" ] && [ "$TBLOCK" -lt "$MIN_TURNS" ] && { led silent-same-stop "$STATE"; exit 0; }

K=$((CTX/1000)); LK=$((LIMIT/1000)); CK=$((CKPT/1000))
read -r -d '' REASON <<EOF
${STEM} (fires once per stop). You are ending the turn at ${K}k/${LK}k -- BELOW the
${CK}k checkpoint line -- after ${TURNS} assistant turns since the user last
spoke. So this is an autonomous run, and the signal that ends one has not
fired.

Ending a turn hands control to a human who may not be back for hours. Under
CLAUDE.md's guided autonomy that is a DECISION, never a default, and a
checkpoint is explicitly not a stopping point.

Which of these is it?

  1. An item from "Autonomy + escalation" -- a format break, a destructive
     operation, an architectural deviation, a scripture-altering design fork,
     or anything outward-facing. STOPPING IS CORRECT: say which, and stop.
  2. The user asked something you have now answered. STOPPING IS CORRECT.
  3. You are genuinely blocked -- needs a human decision, quota, hardware, or
     a long gate you cannot usefully wait on. STOPPING IS CORRECT: name what
     unblocks it.
  4. None of the above. Then the next chunk is the one you just named on your
     own "Next"/"Ahead" line -- OPEN IT in this turn instead of yielding.

If 1-3, say which in a clause and stop; this will not ask again. If 4, drop
the closing summary and make the next tool call -- per CLAUDE.md, writing an
"Ahead" line or a "Key" table IS the tell that you are handing back.
EOF

python3 -c 'import json,sys; print(json.dumps({"decision":"block","reason":sys.argv[1]}))' "$REASON" 2>/dev/null \
    || { led silent-emit-failed "$STATE"; exit 0; }
led block "$STATE"
exit 0
