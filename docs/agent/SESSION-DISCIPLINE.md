# Session discipline: checkpoints, the journal, compaction

> Moved verbatim from `CLAUDE.md` on 2026-09-23 (the CLAUDE.md trim). The compaction line is now **400k** (was 600k); the text below is the rationale record and still says 600k where it was written.
> Section headings keep their original levels.

### The checkpoint contract (binding; every time you hand back)

A **checkpoint** is any **resting point** in the work — a landed chunk, a closed
audit, a surfaced fork, a stopping report. Note that a checkpoint is *not* by
itself a decision to hand back: whether you yield or roll straight into the next
chunk is governed by the 600k line in §"When to recommend `/compact`", and the
default under granted autonomy is to **keep going**. What follows is owed at
every checkpoint either way — including the ones you run straight through, where
it is the only thing keeping the tree pickup-ready. Do these three WITHOUT BEING
ASKED. They exist because the user cannot see what you can see, and the cost of
them guessing is real.

**1. Account for every attached shell, monitor, and background task.**

Enumerate what is still running and dispose of it explicitly:

- Kill anything whose work is finished or whose exit condition can no longer be
  met, then say so.
- For anything deliberately left alive, name it and why in one line
  ("`ci-smp-gate` running, ~20 min, this is the gate we're waiting on").
- If nothing is running, **say "nothing running"** — silence is not the same
  statement.

**Why this is binding, not tidiness:** an attached session reads to the user as
"Claude is still working, do not interrupt." A stray poller therefore does not
just waste a process — it silently converts a finished turn into an apparent
in-progress one and stalls the human. Verify with a real check (`ps` scoped to
the tree/session path), not from memory of what you launched; kill strays by
explicit **PID**, never by pattern (see the unscoped-`pkill` rule). Worked
failure, this project: two clusters in ONE session — three self-matching
`pgrep -f` loops, then six `until grep` waiters whose patterns were
unsatisfiable (one producer stopped, another's marker filtered away by the
author's own `| tail -N`) — each spun for over an hour while the user watched an
"active" session that was doing nothing. See
[[feedback-unbounded-until-waiters]].

**2. Leave the handoff already written, not offered.**

If the tree is in a compactable state (clean, green, no open audit round), the
handoff must ALREADY be current when you hand back — `project_active.md`,
`project_next_session.md`, the phase status doc, any open-finding notes — so the
user can type `/compact` or "keep going" without a preparatory round-trip. Do
not ask "shall I write the handoff?"; do not wait for the compaction to be
announced. The Handoff protocol above says what to write; this says *when*:
**at every checkpoint, in advance.** State the posture in one clause
("handoff current at `<tip>`") so the user knows the choice is free.

If the state is NOT compactable (uncommitted work, red tests, an audit round in
flight), say that instead and name the one thing that would make it compactable.

**3. Say what is next, and show the road.**

After the substantive report, always close with:

- **Next**: the single immediate next action.
- **Ahead**: a one-line progression of the queued chunks on the current arc, in
  order, ending at the arc's close (e.g.
  `#118 libunwind FDE segv -> gl_probe gate -> port patches -> CL-7b close`).

Keep it to one line. The purpose is orientation — the user should be able to see
where the current chunk sits in the arc without opening the tracker.

---

## The run journal (`docs/JOURNAL.md`) — binding, user-requested 2026-08-16

After a long autonomous run the user has to reconstruct what happened, and doing
that from `git log` + six status rows + a memory directory is work they should
not have to do. **`docs/JOURNAL.md` is the single narrative thread**: what
landed, in order, why, what it cost, what it left open.

**Append an entry per autonomous run**, newest run first, as part of the run —
not reconstructed at the end from memory, which is how the interesting parts get
lost. A checkpoint you run *through* still earns its paragraph.

What belongs there, and what does not:

- **NOT a changelog.** `git log` owns the commits; duplicating them here rots.
  **NOT a status doc** — `docs/phaseN-status.md` owns per-chunk rows.
- **The reasoning, the wrong turns, and the findings nobody planned.** A wrong
  turn that got caught is worth more than a win, because the catch is the
  reusable part. Record what caught it — a control, a sabotage, a measurement —
  not just that it was caught.
- **Evidence on every claim**: a hash, a measured number, a file:line.
- **Exactness about what "fixed" covers.** Half a defect closed is written as a
  half, with the other halves named. A run that reads as uniformly successful is
  usually a run that was written up carelessly.
- **Decisions that needed the user**, and what they chose — so the next session
  can tell a ratified decision from an assumed one.

This is the operator's window into an unattended run. Treat a missing entry the
same as a missing status row: the work is not finished without it.

## When to recommend `/compact`

### The 600k checkpoint line — run THROUGH checkpoints until it fires

**A checkpoint is not a stopping point.** Under granted autonomy, land a chunk,
report it, and **start the next one in the same run**. Do not yield after every
chunk waiting to be told to continue; do not compact "to be safe" at 300k. The
cost of stopping early is real and asymmetric — a fresh context has to re-derive
the subsystem knowledge the current one already holds, and the re-derivation is
where wrong turns come from.

**The signal that ends the run is the `ctx-hook` CHECKPOINT WINDOW line at
600k** (`~/.claude/ctx-hook.sh`, `CTX_CKPT`; ~66% of the 900k window, which is
the "~60-70%" the bullets below already named). That hook fires on every tool
call, so it sees the budget continuously — you do not have to estimate it, and
you should not try. Three levels, three different meanings:

| Level | Fires | Means |
|---|---|---|
| **600k CHECKPOINT WINDOW** | once per crossing | **The intended compaction point.** Carry to a clean boundary, write the resume note, then run `tools/thyla-selfcompact.sh "<reason>"`. |
| **750k** | every call | Wind down. Finish the step; do not open a new arc. |
| **880k** | every call | At the wrap line. Commit, hand off, yield. |

**At 600k you compact yourself; you do not ask.** `tools/thyla-selfcompact.sh`
types `/compact` into your own tmux pane, and `~/.claude/resume-note.py`
re-injects your last message on the far side — the two steps the user was
performing by hand at every boundary. Three things follow from that:

- **Your final message before invoking it IS the resume note.** Not a report to
  a reader who will answer — a note to yourself with no memory of writing it.
  It must say what is in flight and, more importantly, **what must NOT be
  redone**: gates already green, commits already pushed, measurements already
  taken. A fresh context that re-runs a two-hour bar has been failed by that
  message.
- **Invoking it is a request, not a decision.** It refuses on a dirty tree or
  outside tmux, and it **belays** — hands back to the user — when HEAD has not
  moved across two consecutive self-compactions. That gate exists because the
  dangerous failure is not a runaway but a *quiet loop*: hit a problem,
  compact, return with less context, fail the same way. Every turn looks like
  progress and none is, and an iteration cap cannot catch it because the
  pathological case sits under the cap. Only landed work distinguishes stuck
  from thinking, so only landed work re-arms the mechanism.
- **A belay is a stop, and it is the good outcome.** When it fires, hand back
  with what was attempted and what it needs. Do not clear the state file to get
  moving again; that is disabling the one guard standing between a long run and
  an expensive one.
- **A queued self-compaction is NOT yours to cancel — only the operator's.**
  The script types `/compact` + Enter into the pane the instant you invoke it,
  so the submission is queued in the client immediately. You CANNOT retract it
  from inside your own turn: `tmux send-keys C-u` clears only the *live input
  box*, never an already-Enter-queued command — a `/compact` you "cancel" that
  way survives and fires later against whatever session is up. (Worked failure,
  2026-08-19: a self-compact invoked early at 560k, then countermanded by the
  Stop hook, was "cancelled" with `C-u`; the stray `/compact` rode the input
  queue for ~4 hours and submitted right after the *real* compaction — harmless
  only by luck, because a spurious `/compact` no-ops with "Not enough messages
  to compact.") Two rules follow. **(1) Invoke `thyla-selfcompact.sh` only on
  the real 600k signal, never in anticipation of it** — that is the one moment
  nothing will countermand you, so there is nothing to cancel. **(2) If you must
  abort a queued self-compaction anyway, you cannot do it yourself: raise a
  blocking question to the operator** (`AskUserQuestion` — it interrupts the
  turn without ending it) asking them to cancel it. The operator is the only
  actor who can clear the client's input queue; your keystrokes cannot.

Where the script is absent (a worktree that has not merged it), the hook says
"recommend `/compact`" instead and the old behaviour stands — the two arms are
discrimination-tested, not assumed.

So the rule is: **below 600k, keep working through as many checkpoints as the
work takes; at 600k, finish to a clean boundary and self-compact.**
The 600k line is advisory and deliberately fires ONCE — it is a "this is the
right moment," not an alarm. Reaching it mid-chunk does not mean stopping
mid-chunk: carry to the next clean boundary (committed, gates green, handoff
current) and recommend from there. If that boundary is genuinely far away, say
so and keep going — 750k is the level that means wind down.

**What does NOT change: the checkpoint contract still fires at every
checkpoint** (§"The checkpoint contract"). Account for running processes, keep
the handoff current, say what is next — at each one, whether or not you yield.
That is precisely what makes this rule safe: if the handoff is continuously
current, then compaction is free at *any* moment, so choosing to run on costs
nothing and the 600k line can be a recommendation rather than a scramble.

**What also does NOT change: the escalation list.** Running through checkpoints
is autonomy over *sequencing*, never over the items in §"Autonomy + escalation"
— a format break, a destructive operation, an architectural deviation, a
scripture-altering design fork still stops the run and asks, at 100k or 700k.

**If the hook is not installed, this rule has no brake.** `ctx-hook.sh` lives in
`~/.claude/`, outside this repo, so a fresh machine or a differently-configured
session may not have it — and then "run until the signal fires" means running
past the wrap line into a hard overflow, because a signal that never arrives is
indistinguishable from one that has not arrived *yet*. So: an autonomous run
that has passed roughly two thirds of its budget **without ever seeing a
CONTEXT line** should treat the hook as absent and fall back to the judgement
bullets below rather than keep waiting. Verify with
`ls -l ~/.claude/ctx-hook.sh` if in doubt; one command settles it.

When all of the following hold:

- Working tree is clean (everything committed).
- Test matrix is green (default + ASan + TSan if applicable).
- The most recent audit round (if any) is closed.
- The next chunk would benefit from fresh context — typically when:
  - Cumulative tokens consumed exceed ~60-70% of the model's context budget.
  - The next chunk involves a fresh subsystem (not the one currently in cache).
  - An audit roundtrip + fix loop is queued (audit agent output is dense).

Recommendation format: short, includes rationale. "Working tree clean at tip X; tests/specs green; next chunk Y would benefit from fresh context. Suggest `/compact` here. Handoff doc updated for clean pickup."

Do NOT recommend compaction mid-chunk or with uncommitted state.

### A checkpoint is not a stopping point (the 600k line)

**Under granted autonomy, land a chunk, report it, and start the next one IN
THE SAME RUN.** Do not yield after every chunk waiting to be told to continue;
do not compact "to be safe" at 300k. Stopping early is not free — a fresh
context re-derives subsystem knowledge the current one already holds, and the
re-derivation is where wrong turns come from.

**The signal that ends the run is the `600k CHECKPOINT WINDOW` line** emitted by
the user-level `~/.claude/ctx-hook.sh` PostToolUse hook. It has three levels
that mean three DIFFERENT things, and collapsing them into one "context is
getting high" warning is how the 600k line turns back into an alarm:

| Level | Fires | Means |
|---|---|---|
| 600k CHECKPOINT WINDOW | once per crossing | the intended compaction point — carry the current step to a clean boundary, then recommend `/compact` |
| 750k approaching | every call | wind down; start no new arc |
| 880k AT THE WRAP LINE | every call | commit, hand off, yield |

Two conditions make this safe rather than merely faster, and adopting the
run-longer half without them is **strictly worse than not adopting it**:

1. **The checkpoint contract still fires at every checkpoint**, including the
   ones you run straight through. Account for running processes, keep the
   handoff current, say what is next. A continuously-current handoff is what
   makes compaction free at any moment — which is what lets 600k be a
   recommendation instead of a scramble.
2. **If the hook is absent, the rule has no brake.** "Run until the signal
   fires" degrades into "run past the wrap line", because a signal that never
   arrives is indistinguishable from one that has not arrived YET. A run two
   thirds through its budget having seen NO `CONTEXT` line should treat the
   hook as absent and fall back to judgement.

   **The wiring is CONFIRMED for aux** (2026-08-15), which was worth checking
   because this worktree defines its own `PostToolUse` (the yip line-hook) in
   `.claude/settings.local.json` — so whether the user-level hook *also* runs
   was a question about hook merging, not something to assume. It does:
   **hooks MERGE across the user and project/local layers**, and aux runs
   both. Observed the awkward way — the hook announced itself by reporting a
   shell parse error mid-edit (`PostToolUse:Bash hook blocking error ...
   ctx-hook.sh: line 52`), and a hook that errors is still a hook that ran.
   Never having seen a `CONTEXT` line is explained by this context sitting far
   below 600k, not by an override.

   **A user-level hook is executed by every live session on every tool call**,
   so editing one in place opens a window where other sessions run a partial
   file. Write to a temp path and `mv` it over (a same-filesystem rename is
   atomic; an in-place write is not).

This is autonomy over SEQUENCING only. The escalation list is untouched —
format breaks, destructive operations and scripture forks still stop the run.

**Host holds are the cost, and they land on the other tracks.** Longer runs
mean longer exclusive holds on the one machine (an SMP gate is ~20 min, LS-CI
~30). Check `yip presence` before committing to a timed measurement, keep the
refuse-up-front gates (`849d85fc`), and say on the line when you want a quiet
host. Named here so a contention-shaped failure is never attributed to
something else later.

**The handoff is ready before you recommend anything.** Per the checkpoint
contract, a compactable state means the handoff docs are ALREADY written — so
this section is only about whether to *suggest* compacting, never about whether
the user *could*. The user decides when to compact; your job is to make sure
that decision never costs a preparatory round-trip. When the state is
compactable and you are not recommending it, still say so in a clause
("compactable if you want it; otherwise next is Y").

---
