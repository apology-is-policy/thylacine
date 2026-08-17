# Watchkeeping

**Guided autonomy for long-running coding agents in a terminal harness — the
concepts, the machinery, and how to set it up from scratch.**

*Status: a review-and-handbook written 2026-08-17 from the running system in
the Thylacine OS project — three Claude Code agents on one 8-core Mac and one
Pi; ~340 context windows across the three live transcripts (163 + 123 + 58
compaction boundaries since late June). The pieces are of different ages and
the document says which: the wrap-line warner and the handoff discipline are
~two months old; the telephone two weeks (2026-08-04); the checkpoint line,
self-compaction, the nudge, the Stop hook and resource leases landed
2026-08-15/16 and have run for one day. The concepts are generic; each
"In Thylacine" note names the concrete implementation. Companion evidence log:
`docs/AUTONOMY-FIELD-REPORT.md` (append-only, on the main worktree). The
telephone/arbitration/resource tool is `yip` (`github.com/apology-is-policy/yip`,
MIT).*

---

## 0. What this is, in one screen

A coding agent in a TUI harness has three hard boundaries that ordinary use
never crosses: **the turn ends** (the model stops calling tools and writes
prose — control returns to a human who may be asleep), **the context window
ends** (the transcript must be summarised and the session rebuilt), and **the
human is the only thing that can restart either**. Add a second and third agent
on parallel worktrees sharing one machine, and there is a fourth: **they cannot
talk, arbitrate, or take turns on the hardware without a person relaying**.

Watchkeeping is the set of rules and small mechanisms that let an agent run
**for hours across many context windows, self-pacing its own compactions,
handing off to itself, waking itself up, coordinating with peers over a real
channel, and surfacing to the operator only what is genuinely the operator's**
— architecture forks, format breaks, destructive operations, value judgments,
and "I am stuck".

It is built from five parts, each existing because a specific failure demanded
it:

| Part | What it does | Concrete implementation |
|---|---|---|
| **Standing orders** | the behavioural contract the agent loads every session: what autonomy covers, what must be escalated, what a checkpoint is, that ending a turn is a decision, that the handoff is always already written | `CLAUDE.md` sections + a small set of memory files |
| **The watch instruments** | hooks that measure the context budget, ask "is stopping a decision?", and re-inject intent after a compaction | `~/.claude/ctx-hook.sh` (PostToolUse), `tools/stop-hook.sh` (Stop), `~/.claude/resume-note.py` (SessionStart) |
| **The change of watch** | governed self-compaction: prove the tree is clean and progress was made, write a one-shot resume note, type `/compact` into your own pane, and have a detached watcher press Enter on the far side | `tools/thyla-selfcompact.sh` + `tools/thyla-nudge-watch.sh`, tmux |
| **The bridge telephone** | a floor-based channel between agents: calls, notes that cannot carry a decision, disputes settled by naming a measurement, presence, a human seat no agent can forge | `yip` (Go, stdlib-only; MCP server + CLI + hooks + a switchboard TUI) |
| **The engine-room telegraph** | leases on shared machines — testimony instead of measurement, wall-clock expiry, no licence to kill | `yip hold / release / steal / resources` |

Plus a **launcher** that puts one agent per worktree in a tmux pane with a
role tag and a named conversation (`tools/thyla-tmux.sh`), and a **log
discipline** — an append-only journal of each run, an out-of-band ledger of
every compaction, and a memory index kept small enough to be loaded every
session.

### Why "Watchkeeping"

The project called it "the guided autonomy system". The better name is the
one sailors already use for exactly this arrangement: **watchkeeping**. A ship
runs continuously; the bridge is never unmanned; officers stand watches in
rotation; each hands over to the next with a briefing and a log entry; the
master sleeps — but sails under **standing orders** that list precisely the
conditions under which the officer of the watch must **call the master**, and
the officer does not leave the bridge until relieved. Every term maps, and
maps onto something already built:

| Watchkeeping | Here |
|---|---|
| the ship, always under way | the project, worked continuously |
| standing orders | the behavioural contract in `CLAUDE.md` |
| "call the master" conditions | the escalation list — format breaks, destructive ops, scripture forks, anything outward-facing |
| a watch | one context window's worth of work |
| the change of watch / relief | compaction; the resumed session |
| the handover briefing | the resume note + the pickup files |
| the deck log | `docs/JOURNAL.md` + the compaction ledger |
| the watch bill | the tmux launcher: who is on which worktree |
| the bridge telephone | `yip` calls |
| the engine-room telegraph | `yip` resource leases |
| "the OOW does not leave the bridge until relieved" | "ending a turn is a decision, never a default"; never pause without arming a wake |

The metaphor is used once, here, so the vocabulary is anchored; the rest of
the document uses the plain names. (Runners-up considered: *Standing Orders* —
names only the contract; *Long Watch*; *Relay* — the baton, but nothing of
the orders. "Guided autonomy" remains an accurate description of the stance.)

---

## 1. The problem, precisely

Terminal coding agents were built for a conversation: prompt, work, answer,
wait. Long unattended work breaks that shape in ways that are individually
small and jointly fatal:

1. **Ending a turn is free, so it happens by default.** A model finishes a
   chunk, writes a tidy summary, and stops — because summarising *feels* like
   completing, and because every instruction it has ever seen ends with a
   reply. In this harness the summary **is** the yield: nothing restarts a
   turn but the user. Measured in Thylacine (field report §2): two runs
   stopped at ~160k and ~270k of a 600k budget "at a natural boundary", and
   the operator had to come back and ask why.
2. **The context window is finite and the summary drops the one thing that
   matters.** Compaction preserves the conversation's spirit but loses the
   assistant's *last message* — what is in flight, what must not be redone.
   For nearly two months a human re-pasted it by hand at every boundary; one
   transcript has 163 such boundaries.
3. **A message delivered is not an action taken.** A `SessionStart` hook can
   inject the perfect note into a fresh context — and the session then sits at
   an empty prompt, because injected context is passive. Nothing pressed
   Enter. The same shape recurred three times in one day: the resume note; the
   checkpoint instruction in `CLAUDE.md`; the *fix* to that instruction,
   written to `CLAUDE.md` mid-session and therefore not loaded until the next
   one. **A channel that delivers is not a channel that causes.** Every fix in
   this system that worked added an *actor*, not a channel.
4. **Two agents on one tree with no channel means a human carrying 14 KB
   merge instructions between terminals** — lossy at a paragraph, impossible
   at a page. And two agents on one *machine* with no arbitration means both
   reason "I'm already running" and neither yields, or one kills the other's
   forty-minute gate by pattern.
5. **"Is the machine free?" has no local answer.** Three agents wrote three
   waiters for it in one day; all three were wrong. The instructive one
   required zero QEMU *and* zero builds *and* low load — and fired into the
   gap between a peer's build and its boot. **A machine between phases is
   identical to an idle machine on every dimension a machine exposes**; the
   difference is intent, and intent is not on the machine. Only testimony
   settles it.

The rest of this document is the answer to those five, in the order a
from-scratch reader needs them.

---

## 2. Principles — each earned

These are the load-bearing ideas. Every one came from a failure that is
recorded, and the failure is the reason to keep the rule when it feels like
ceremony.

**P1. Autonomy over sequencing only.** The grant of autonomy covers *when* to
land the next chunk, *when* to compact, *when* to hand off. It never widens
the escalation list: format breaks, destructive operations, architectural
deviation, scripture-altering design forks, anything visible to others still
stop the run and go to the human. (Standing orders §"Autonomy + escalation".)

**P2. Ending a turn is a decision, never a default.** The full operational
summary belongs to *stopping* — the budget line, an escalation item, out of
moves, or the user asking. A checkpoint that is run through gets three lines
and the next tool call. **The tell** that you are handing back: you are
writing a `Key` table or an `Ahead` line. Made structural by the Stop hook.

**P3. A checkpoint is not a stopping point; the budget line is the signal.**
Stopping early is not free — a fresh context re-derives subsystem knowledge
the current one already holds, and re-derivation is where wrong turns come
from. The run ends at the checkpoint line the context warner emits (60% of the
window), and only there.

**P4. The handoff is always already written.** At every checkpoint the pickup
files, the status doc and the open-finding notes are current *before* the
turn hands back, so `/compact` — by the human or by the agent — is free at any
moment. A compaction is only free when the handoff is already current; the
self-compaction script refuses on a dirty tree for exactly this reason.

**P5. Hooks ask; they do not veto. And they fail open.** Only the model can
tell an earned yield from an unearned one, so the Stop hook blocks *once* with
a question and lists the legitimate exits. Every error path in every hook
exits 0: a hook that fails closed can trap a session in a loop it cannot talk
its way out of, which is worse than any missed nudge.

**P6. Never pause without arming something that wakes you.** A wait with
nothing scheduled behind it is a stop wearing a pause's clothing. Before
yielding on any external condition, arm a *bounded* waiter that exits when the
condition holds — and that also exits on the bound and *says which fired*,
because a waiter that can only report good news is silent through a crash.

**P7. Progress, not iteration count, governs self-compaction.** The dangerous
failure of an unattended compact/resume loop is not a runaway; it is a *quiet*
one — hit a problem, compact, return with less context, fail the same way,
compact again, each turn looking like progress. An iteration cap cannot catch
that because the pathological case sits under it. So the gate is evidence:
**HEAD must have moved**; two consecutive compactions with a static HEAD belay
the mechanism and hand back to the operator, who is the only one who can tell
"stuck" from "thinking".

**P8. A resume note is intent, not state — and it is a one-shot slot.**
Scripture and memory carry the facts; the compact summary carries the
narrative; the note carries *what is in flight and what must NOT be redone*.
It must be fresh (written for *this* compaction, refused if older than ten
minutes or thinner than 200 chars), it is `mv`'d — not copied — into a pending
slot so a forgotten rewrite fails loudly with "no note" instead of quietly
shipping yesterday's, and the far side consumes it exactly once, before it
prints. A stale note re-injected with total confidence is worse than none: it
happened once, and asserted an unrun bar and unpushed commits.

**P9. Something must press Enter.** Injected context seeds a session; it does
not submit a turn. The waking keystroke has to come from outside both sessions
— the one being torn down cannot, the one being built is the thing being
woken — and it has to be typed *during* the rebuild, not queued before it (a
message queued before compaction belongs to the old session and dies with it;
measured, two seconds' difference, the whole difference between waking and
not).

**P10. An assertion must stay expensive; everything else should be cheap.**
On the telephone, an assertion — a claim that changes what the peer does — may
be spoken only by the side that did *not* write the last turn. One shot, so
you check before speaking rather than after being contradicted. That expense
caught seven false-cleans in one exchange. Everything that is *not* an
assertion (a correction, status, an artifact, a ratification) has a cheap verb
shaped so it cannot quietly become one — a note has no reply affordance, so it
cannot ask the peer to choose.

**P11. When you disagree, name a measurement, not a winner.** Every real
disagreement in two months was settled by one side going and measuring. Both
sides name what would settle it: same measurement → run it, no human;
different → run both; one "none" → run the other; **both "none" → escalate,
because that is not a factual disagreement**, it is a value or scope call and
belongs to the human by right. There is deliberately no automated arbiter.

**P12. A human decision is a human's — enforced by the absence of a verb.**
`ratify` and the switchboard's arbitrate key exist only on the CLI/TUI; there
is no MCP tool, so an agent has no verb that can produce a human turn. "The
human approved" relayed by a peer converts *the human decides* into *an agent
told me the human decided*, and those differ exactly when the guarantee is
being tested (observed live: one agent relayed an approval, the other
correctly refused it).

**P13. Testimony over measurement for shared machines; the holder's word is
authoritative.** A lease is a statement from whoever knows. It expires on
**wall clock, never on heartbeat** — a beat is written per *tool call*, not per
unit of work, so a 40-minute gate in one blocking call beats not at all (a
peer's beat read "36m ago" mid-gate; heartbeat expiry would have handed their
cores away at minute 3). An expired lease is not an open one: taking it is a
deliberate, recorded `steal`. **Holding is no licence to kill** — it means
nobody else *starts*; killing a peer's gate at boot 39 of 40 destroys the work
and the evidence, and the killer never sees what they destroyed. Identify by
cwd, notify, grace, escalate.

**P14. Every mechanism must be proven able to fire, and must leave a witness
someone reads.** A negative control ("nothing fired") is satisfied by a
watcher that never started (`setsid` does not exist on macOS; the first
watcher died instantly and "nothing fired" was perfectly true). A gate whose
success is visible only inside the model's context is indistinguishable from
one that never ran (the operator concluded exactly that on the first live
resume-note run). So: discrimination tests for every hook, controls that show
the detector *can* see the negative, sabotage that reddens the test — and an
out-of-band ledger row per event. **And a reader for the ledger** — the one
defect this review found (§8, R1) sat in the ledger for twenty hours as an
`allow` with no `consumed`, exactly the row the ledger exists to produce, with
nothing raising it.

**P15. Take the smell, re-derive the remedy.** In one three-agent day *every*
cross-track finding was right about the defect and wrong about the fix, in all
three directions. The generative half of an analysis is pattern-matched off
the problem's shape and does not consult its own inputs. So never apply a
peer's fix without walking the code, and never discount the reporter because
their fix is wrong — the finding survives independently.

---

## 3. One watch, end to end

```
 operator submits one prompt            (or: the nudge from the previous watch does)
        |
        v
 +----------------------------------- one WATCH (one context window) ----------------------------------+
 |                                                                                                     |
 |  work ... chunk lands -> checkpoint (3 lines, next tool call) -> chunk -> checkpoint -> ...         |
 |                                                                                                     |
 |  on EVERY tool call (PostToolUse):  ctx-hook  -- silent below the checkpoint line                   |
 |                                     yip hook  -- heartbeat; "[yip] main spoke on 0021, floor yours" |
 |  on every attempt to end the turn:  Stop hook -- "is ending the turn a DECISION? (1-4)"  asks once  |
 |                                     yip hook  -- blocks while you owe the peer a reply              |
 |                                                                                                     |
 |  ... 600k CHECKPOINT WINDOW (ctx-hook, once per crossing) ------------------------------------------+---+
 +-----------------------------------------------------------------------------------------------------+   |
                                                                                                           v
   1. carry the step to a clean boundary: commit, gates green, pickup files current (P4)
   2. write the RESUME NOTE to the slot file: what is in flight, what must NOT be redone (P8)
   3. tools/thyla-selfcompact.sh "<reason>"
        governor: in tmux? git worktree? tree clean? note fresh+thick? HEAD moved since last time?
        -> REFUSE (says why) | BELAY (hand back to the operator, rc=3) | ALLOW:
             write state + ledger row FIRST; mv note -> .pending (+ .meta: role/head/reason/at/pane)
             `yip busy "compacted at <head> -- re-state anything subtle"`
             arm thyla-nudge-watch.sh (detached, nohup) BEFORE queueing anything
             tmux send-keys: C-u, "/compact", Enter        (submits when this turn ends)
                     |
                     v      the harness summarises and REBUILDS the session
   4. SessionStart: resume-note.py consumes the pending slot ONCE, injects it under a header that says
      "you compacted YOURSELF -- nobody is waiting; carry on" (or falls back to the last long assistant
      message under a plainer header when there is no slot)
   5. the watcher sees "compacting" in the pane's bottom lines, waits 2 s, types
      "[auto-nudge] Continue from your resume note." + Enter   -> the far side WAKES
                     |
                     v
 +----------------------------------- next WATCH ------------------------------------------------------+
```

What the operator sees: pane titles, `yip switchboard` on the side, the
journal and the ledger the next morning, and — when a call-the-master
condition arises — a stopped run with a structured question in it.

---

## 4. The parts

Each part below: why it exists, how it works, its contract (so you can
rebuild it), and how it was proven.

### 4.1 Standing orders — the behavioural contract

**Why.** Every mechanism below asks the model a question or hands it a note.
What the model *does* with that depends entirely on rules it has loaded. The
rules live in the project's always-loaded instructions (`CLAUDE.md`) — and,
for the ones that must survive compaction and mid-session edits, in memory
files, because `CLAUDE.md` loads at session start and **landing a rule is not
adopting it** (the fix to the checkpoint rule was broken by the session that
wrote it, because that session never loaded it).

**The sections that make up the contract** (a template is in §5.1):

| Section | Says |
|---|---|
| Autonomy + escalation | what proceeds unasked (implement, test, audit, commit, push to own branch) and what always escalates (format breaks, destructive ops, architectural deviation, scope pivots, anything outward-facing, significant spend) |
| The checkpoint contract | at every hand-back: (1) account for every running shell/monitor/task — kill or name it, and *say "nothing running"* explicitly; (2) the handoff is *already* written; (3) say what is next and show the road (one line) |
| A checkpoint is not a stopping point | land a chunk, report in three lines, open the next in the same run; the signal that ends a run is the checkpoint line, whose three levels mean three different things (checkpoint / wind down / wrap); if the warner is absent, fall back to judgement |
| Ending a turn is a decision | (memory file) full summary = stopping only; the tell is a `Key`/`Ahead`; never pause without arming a wake; "blocked" means out of moves, not out of the preferred move |
| Handoff protocol | which files carry state (active, next-session pickup per track, status doc, open findings), updated at every boundary, committed |
| Operational summary pattern | the shape of a stopping report: focus, arc fit, arc state, an id key, metrics with provenance, exit criteria, what landed, posture, **Running**, **Handoff**, **Next**, **Ahead** |
| Task-list / injection hygiene | anything re-injected every turn stays small: live list = open work only; archive-then-delete |
| Peer coordination | presence before call; busy before anything long; announce the *resource and the uncertainty*, not a duration; take the lease before anything needing cores; release when the resource frees, not when the workflow finishes |
| Whole-system stewardship | there is no "my chunk"; a surfaced defect preempts; encounter a bug → enqueue a bug, before anything else |

**How it was proven.** Behaviourally, twice, by failing (field report §2) —
which is why the Stop hook exists. The stewardship and coordination rules each
carry a worked failure in the text itself, on purpose: a rule with its
specimen attached is one a future reader can re-derive.

### 4.2 Memory and handoff files

**Layout** (Claude Code's per-project auto-memory directory):

```
~/.claude/projects/<encoded-project-path>/memory/
  MEMORY.md                      one-line index, auto-loaded every session -- keep it small
  project_active.md              current state: landed / in progress / next
  project_next_session.md        pickup pointer for track A (aux)      } one per track,
  project_next_session_main.md   pickup pointer for track B (main)     } NEVER merged
  feedback_*.md                  binding guidance that must survive compaction (with the WHY)
  bug_*.md / audit_*_closed_list.md / reference_*.md
  TASK-ARCHIVE.md                completed work booted from the live list
```

Two rules that matter for multi-agent use: **one pickup file per track** (the
first two agents shared one and overwrote each other), and **`MEMORY.md` is
shared across worktrees and written by both** — single-line `Edit`s only,
never a whole-file `Write`, and never compact both sessions into simultaneous
memory writes. It is the highest-contention file in the system (field report
§3 item 9 proposes a claim protocol; not built).

The **journal** (`docs/JOURNAL.md`) is the narrative of each autonomous run —
newest first; every claim with its evidence; wrong turns recorded because they
are the reusable part; "fixed" stated exactly. It is what lets the operator
reconstruct a night's work without stitching `git log`, six status rows and a
memory directory.

**The third seat (optional).** In Thylacine a third agent — `vault`, on its
own worktree — keeps the project's knowledge system: a lint-enforced,
git-versioned graph of present-tense truth dossiers that replaces prose
reference docs and hand-maintained tables. It stands the same watches as the
other two, joins the same telephone line, and takes the same leases; the only
thing special about it is what it edits. It is mentioned here because a
knowledge-keeper is the natural third role once two engineering tracks
generate findings faster than either can file them — and because it is the
seat that surfaced the review's one defect (it is launched from a different
directory than its worktree, §8 R1).

### 4.3 The context warner (PostToolUse)

**Why.** The run's end signal must be *live during a one-prompt 30-minute run*
— so it hooks `PostToolUse` (fires on every tool call), not `UserPromptSubmit`
(fires only at submit, i.e. at low context).

**How.** `~/.claude/ctx-hook.sh` reads the hook's JSON from stdin, takes
`transcript_path`, `tail -c 8 MiB` of it (transcripts reach hundreds of MB —
never read whole), and computes the live budget as the **last non-zero usage
line after the last compaction boundary**: `input_tokens +
cache_read_input_tokens + cache_creation_input_tokens` of the newest
`message.usage`, resetting to 0 at any record with `isCompactSummary`. The
anchoring matters: right after a compaction the pre-compact high-water (the
value that *triggered* it) is still the last usage on disk, so an un-anchored
scan over-reports ~2× at session start.

Three levels, three meanings — collapsing them into one "context is high"
warning is how the checkpoint line turns back into an alarm:

| Level | Default | Fires | Means |
|---|---|---|---|
| CHECKPOINT | 600k | **once per crossing** (marker file under `$TMPDIR/ctx-hook/<session>.ckpt`; deleted — re-armed — when the budget drops below the line, i.e. a compaction landed) | the intended compaction point: carry the step to a clean boundary, write the note, self-compact (or recommend `/compact` if the script is not in this tree) |
| WARN | 750k | every call | wind down; finish the step; open no new arc |
| HARD | 880k | every call | at the wrap line: commit, hand off, yield |

Once-per-crossing for the checkpoint because 600k→750k is ~150k of window in
which a per-call reminder would consume the very budget it protects; the two
stopping levels stay insistent because a single missable line is the wrong
shape for "stop". Output is `hookSpecificOutput.additionalContext`.

Numbers assume a 1M window; `LIMIT=900000` is kept deliberately conservative
(so the percentages read low, not wrong — field report §1 "Open decision").
Scale to 60% / 75% / 88% of your window.

### 4.4 The resume note (SessionStart)

**Why.** `/compact` preserves the spirit and drops the assistant's final
message — the intent. A human used to paste it back by hand; this is that
paste, automated. It emits `hookSpecificOutput.additionalContext` on
`SessionStart` (matcher `startup|resume|compact` — the *compact* source is the
one that matters).

**How** (`~/.claude/resume-note.py`), two paths, preferred first:

1. **The slot.** If `<STATE_DIR>/<key>.note.pending` exists, read it and its
   `.meta` (role, head, reason, at, pane), **delete both before printing**
   (consume exactly once — a crash after printing must not hand the note to a
   later, unrelated session), write a `consumed` row to the ledger, and inject
   the note under a header that says *you compacted YOURSELF at the checkpoint
   line; this is not a hand-back; carry on; re-verify any figure before
   quoting it*. A session that compacted itself should continue; one the user
   compacted may be about to be redirected; the header is what tells them
   apart.
2. **The transcript heuristic.** Otherwise take the **last assistant message
   ≥ 200 chars in the file, full stop** — *not* the last one before the newest
   compaction boundary. Ordering fact, invisible unless measured: SessionStart
   fires **before** the new compact summary is written, so at hook time the
   newest boundary on disk is the *previous* one, and walking back from it
   lands a whole segment early (measured: 1227 records stale, an entire day).
   Inject it under a plainer header: *intent, not state; treat every figure as
   of that moment*.

Bounded read (32 MiB tail), capped output (12 000 chars), and it exits 0 on
every exception — a hook must never block a session start.

**The ledger.** Both sides append to one `log.tsv` — `allow` when the
producer arms, `consumed` when the consumer takes the slot, `nudge-sent` /
`nudge-timeout` from the watcher, and (since 2026-08-17) `orphan-note` when
the consumer reaches the fallback path with a slot still armed that it did not
match, and `stale-discarded` when a matched note is older than 30 min (a
healthy note is minutes old; an old one means its compaction never completed,
and injecting it would tell a fresh session to carry on from work a day
stale) — because the hook's only output channel is the model's context, so a
working re-injection is *invisible from the terminal*, indistinguishable from
the hook never having fired. The operator drew exactly that conclusion on the
first live run. **A success you cannot observe is a success you have not
verified.**

**The key.** The slot used to be named *only* by an "encoded project path"
that both sides derived independently — the producer from `git rev-parse
--show-toplevel`, the consumer from `basename(dirname(transcript_path))`, with
the comment "no shared config to drift". **This review found those are not
the same key** for a session launched from a directory other than its worktree
root (§8 R1). The consumer now prefers an identity tmux hands to *both* sides
— the `pane=$TMUX_PANE` the producer already stamps into `.meta`, matched
against the hook's own `$TMUX_PANE` (a hook is a child of the same claude
process) — and keeps the path as the fallback for anything armed outside
tmux. Two derivations that must agree are safe-if-they-agree, never safe by
default.

### 4.5 The change of watch — governed self-compaction

**Why.** With auto-compact deliberately off (a mid-operation compaction can
strand a half-edited tree; the operator would rather rewind to a clean
handoff), *someone* has to type `/compact` at the right moment. The right
moment is the checkpoint line, and the one who knows the tree is clean and the
note is written is the agent. So the agent types it into its own pane.

**How** (`tools/thyla-selfcompact.sh [--check] [reason]`):

1. **Identity.** Role from the tmux pane option `@thyla-role` (set by the
   launcher), else the worktree's basename. State file per role; slot file per
   project key.
2. **Preconditions — default is refuse, and every refusal says why:** inside
   tmux (`$TMUX_PANE` is how the pane addresses *itself* — every process tmux
   starts gets its own unique pane id, so no naming scheme has to be kept in
   sync); a git worktree; **zero uncommitted tracked changes** (untracked
   files are allowed — that leg is the control that must *not* refuse); a
   resume note file that exists, is **≤ 600 s old** and **≥ 200 chars**.
3. **The progress gate.** Compare `HEAD` with the last self-compaction's
   recorded head. Moved → allow, counter reset. Static → counter+1; at 2 →
   **BELAY** (rc 3): print that two sessions with nothing landing is the
   signature of a loop that looks busy, hand back instead. Cleared by landing
   a commit or removing the state file.
4. **Commit the decision before acting on it.** State + ledger row are written
   *before* the keystrokes; otherwise a compaction that lands without the
   record would re-allow forever.
5. **Arm the slot.** `mv` the note to `.pending`, write `.meta`
   (`role/head/reason/at/pane`).
6. **Tell the peers**: `yip busy "compacted at <head> -- context reset,
   re-state anything subtle"` — a compacted agent has lost the *texture* of
   recent exchanges even where the facts survive.
7. **Arm the watcher (§4.6) BEFORE queueing the command** — it must already be
   polling when the client changes state.
8. `tmux send-keys -t $TMUX_PANE C-u` (never submit residue), `"/compact"`,
   `C-m`. The keystrokes are queued behind the current turn and fire when it
   ends — so invoking the script is a *request*, not a decision; the tree
   check and the belay decide.

Also printed, not gated: which long-lived processes (QEMU, java/TLC) will
survive the compaction — the note must say so, because a detached job outlives
the context that knows why it was started.

**Proven.** Every link measured before writing (pane self-address; send-keys
reaches the input box — probed with a runtime-computed value against a control
proving it absent, because a literal self-matches in a rendered pane; C-u
retracts; a leading `/` executes as a command, verified out-of-band via
`/copy` and the clipboard). The governor was discrimination-tested with an
overridable state dir: first run; static HEAD 1/2; static HEAD → BELAY rc 3;
HEAD moved resets; moved despite a high counter; and three refusals (no tmux;
dirty tree; untracked-only as the must-not-refuse control). Live: the ledger
shows the full automatic cycle — `allow` → `nudge-sent` (trigger=running,
4–11 s) → `consumed` — twice on 2026-08-16 for the main track after the
watcher landed. **Never yet exercised live: the belay.** It is the most
safety-critical path and the least tested; a deliberate two-static-HEAD drill
is owed.

### 4.6 The nudge watcher — pressing Enter on the far side

**Why.** The note lands perfectly and the resumed session sits at an empty
prompt (P9). Something outside both sessions must type. The first fix queued
the nudge behind `/compact` through the same channel — and did nothing. The
tempting diagnoses ("the input queue holds one message", "`/compact` discards
queued input") were both measured false: the queue holds two (a `/copy`
standing in for `/compact`, the second message arrived intact). What a queued
message does not survive is **the rebuild** — queued *before* the compaction it
belongs to the session being torn down. The operator's own manual habit is the
working version: submit `/compact`, wait for it to *start*, type *while it
runs*.

**How** (`tools/thyla-nudge-watch.sh <pane>`), detached with `nohup … &
</dev/null` (it must outlive the context that starts it, and the harness stops
its own background tasks — three times in one day):

- poll `tmux capture-pane -p` every 0.5 s, **bottom 12 lines only** — the
  marker is a status-area string, but the same words in prose match just as
  well, and the message written immediately before a compaction is
  overwhelmingly likely to be *about* compaction, so a whole-pane search would
  match the agent talking rather than the client working;
- trigger on the case-insensitive **stem** `compacting` (running) or
  `compacted` (finished — the fallback that makes the window missable only by
  being absent, not by being slightly late). Stems, because neither exact
  string had been observed before shipping; they are read off the client's
  behaviour;
- **markers are defaults inside the file, never arguments** — the launching
  command line is itself rendered in the pane and would match itself
  instantly (`capture-pane` sees the TUI, so a command's *text* renders while
  its *result* never does);
- settle 2 s (let the rebuild take the input box), then `send-keys
  "[auto-nudge] Continue from your resume note."` + Enter. **No `C-u`** — the
  box is empty by construction, and `C-u` would destroy anything the operator
  had started typing, which is exactly what an operator does during a
  compaction;
- **bounded** (900 s): on timeout it logs `nudge-timeout` and **sends
  nothing** — a stray message injected into a live session arrives looking
  like the operator asking for something, and is worse than a missed wake.

The nudge text is tagged `[auto-nudge]` because it lands in the transcript
looking like the operator speaking, and it is not; the Stop hook (§4.7)
excludes it from "the user spoke".

**Proven.** Both stems discriminate; refuses without a pane; the bound exits 1
and sends nothing; the send path proven end to end against a runtime-random
marker (silent 6 s while absent, fired ~1.5 s after it rendered, ledger row
written). The negative control's first pass was *vacuous* — the watcher had
died instantly on `setsid` (absent on macOS) — caught only by the second leg,
"is the process still alive?". Live: three `nudge-sent trigger=running` rows
in the ledger, so the residual risk the commit named ("neither marker has been
observed in a real pane") is retired.

### 4.7 The Stop hook — "is ending the turn a decision?"

**Why.** The 600k rule was behavioural only, and it failed twice. `CLAUDE.md`
itself named the mechanism that would make it structural — a Stop hook —
"deliberately not built", to keep a cheap human override at turn boundaries.
Built at the operator's request after a run stopped at a checkpoint having
written the very `Ahead` line the rule names as the tell.

**How** (`tools/stop-hook.sh`), same byte-bounded, compaction-anchored budget
read as the warner, so the two never offer the model two different numbers;
plus a count of **assistant turns since the last genuine user message**. It
blocks **once**, returning `{"decision":"block","reason":…}` with the question:

> STOP CHECK (fires once). You are ending the turn at Nk/900k — BELOW the 600k
> checkpoint line — after T assistant turns since the user last spoke. So this
> is an autonomous run, and the signal that ends one has not fired. Which of
> these is it? 1. An escalation item — stopping is correct: say which, and
> stop. 2. The user asked something you have now answered — stopping is
> correct. 3. You are genuinely blocked — say what unblocks it. 4. None of the
> above: the next chunk is the one on your own Next/Ahead line — open it in
> this turn instead of yielding.

Three things keep it from nagging: `stop_hook_active` (set when it already
blocked this stop — fire again and it loops forever); a **floor** (120k —
below it the session is conversational, stopping is the expected shape); a
**ceiling** at the checkpoint line (at/above it stopping is what the contract
wants; a second voice contradicting the warner would be worse than silence);
and `MIN_TURNS` (6). What counts as "the user spoke" is the leg most likely to
be silently wrong: tool results arrive as user-type events carrying
`tool_result` blocks, the harness injects notifications as user text, local
commands carry a caveat marker, and the nudge is tagged — **none of those
reset the counter**, or the hook would be silent during exactly the long runs
it is for. Fail-open on every error path.

**Proven.** Discrimination-tested across all seven conditions — proven *able
to fire*, not merely observed staying quiet — with the tool-result and
notification legs tested to still fire. Live in this session: fired once at
132k after 20 turns when the assistant paused at the operator's explicit
"please pause"; the answer "2 — the user asked; answered" ended it, and it did
not ask again. Asks, does not veto: working as designed.

Where it lives is a deliberate trade with a sharp edge (§8 R4): in the repo
(`tools/`) so it is versioned and survives a clone — but wired from the
*user-level* settings by absolute path, so every project on the machine runs
this project's hook.

### 4.8 The launcher — one agent per worktree, in a pane with a name

**Why.** Self-compaction addresses its own pane, so agents live in tmux; the
role has to be knowable from inside; conversations must be resumable by a
stable handle; and starting a second agent on a worktree that already has one
must be refused *visibly*.

**How** (`tools/thyla-tmux.sh pair|vault|status`):

- **Two things are called "session" and they are not the same**: a *tmux*
  session (the layout: `thylacine` holds main+aux side by side; `vault` is its
  own, because it is transient) and a *conversation* (what `claude --resume`
  takes). Conversations are **named with `/name`** inside Claude Code and
  resumed by that exact identifier — never `--continue`, which silently picks
  whatever was touched last in that directory.
- **Address panes by id, never by index.** `tmux … -P -F '#{pane_id}'` returns
  the id it just created; ids are absolute and immune to `pane-base-index`.
  The first version used `.0`/`.1` and on a host with `pane-base-index 1` —
  common, and this one — **an out-of-range index does not error, tmux aliases
  it to the first pane**: both resume commands landed in main's pane, the
  second typed straight into a running agent's input box.
- Tag each pane with `@thyla-role main|aux|vault` and a title; **launch a
  shell and type the command into it** rather than making claude the pane's
  root process, so a crash leaves the pane holding the error instead of
  vanishing with it.
- **Enumerate agents with `ps`, not `pgrep`**: BSD `pgrep` omits the caller
  and all its ancestors unless `-a`, and this script runs from inside an
  agent's pane — so it would omit precisely the process whose duplication the
  guard exists to prevent (fails *open*). Identify the tree by `lsof -a -p
  <pid> -d cwd`; `ps` cannot tell worktrees apart when both invoke tools by
  relative path.
- Refuse a duplicate **in the pane**, with the command pre-typed but not
  submitted, because the attach repaints the screen and swallows anything
  echoed to stdout — the first real run's guard fired and read as a bug.
- Liveness is checked *behaviourally* after a settle (is there a claude with
  that cwd?), because nothing on disk resolves a conversation name, and a
  "does this conversation exist?" precheck would be a check that cannot fail.

### 4.9 The bridge telephone — `yip`

A single stdlib-only Go binary with two faces: `yip serve` (an MCP server over
stdio, what the agents call) and the CLI (what the hooks use and what a human
uses). Hooks are deliberately CLI: a hook has to work when the MCP server is
down, and the hook is exactly what would tell you it is down. State is plain
files under `~/.yip/lines/<line>/` — *outside every checkout*, because
checkouts sit on different branches and a file committed on one is invisible
to the others until merged, which is the very thing a merge conversation is
coordinating. Transcripts are markdown, so `cat` always suffices, and they
**survive an agent's context being compacted away**: a handoff points at
them instead of reproducing them.

**Lines and identity.** Worktrees of one repository join one line
automatically (keyed on the git common dir); separate clones are grouped with
`--line`. Identity is **recorded at install, keyed by the checkout's absolute
path** — not a name a session picks, so two sessions cannot answer to one
name. Sharp edge, observed: a checkout that was never `yip install`ed does not
go silent — it *impersonates* whichever member its cwd falls under (the vault
registered as `main` until installed).

**Calls and the floor.** `call` opens a call and speaks the first turn (rings
the peer). `say` speaks — **only if you did not write the last turn**; `barge`
overrides and is marked permanently in the transcript. `read` marks seen and
warns when a turn's recorded peer-HEAD is not your current HEAD (**the
staleness stamp**: every turn records both worktrees' HEAD, so an instruction
computed against a tree that has since moved is detectable). `bye` is a
*proposal*: the call closes only when both sides have said it, and any `say`
clears a pending one; it does not need the floor. Writes are atomic —
staged into `.tmp/` and hard-linked into place, so a partial turn is
unobservable and two writers racing one turn number cannot clobber (the loser
re-renders); the floor is *derived* from the turn files, never stored, so it
cannot disagree with the transcript.

**How the phone rings** — an agent between turns is not executing, so nothing
can interrupt it; the ring is layered over the moments it is:

| Recipient is… | Mechanism | Latency |
|---|---|---|
| working (making tool calls) | `PostToolUse` — one notice per *new* turn (not per call until read); a bell + window title on a brand-new call | next tool call |
| about to go idle owing a reply | `Stop` — **blocks**, once per turn count (`BlockedAt` re-arms when the count advances) | immediate |
| opening / resuming / post-compact | `SessionStart` (matcher `startup\|resume\|compact`) — rings + peers' presence | at open |
| idle, owing nothing | `yip watch [--once]` — an event stream, one line per event; `--once` is **bounded** (10 min; **exit 3 = nothing arrived, which is not success**) | one interval |

**Cheap verbs, shaped so they cannot become assertions.** `note` — one-way,
no floor transfer, no reply owed, does not clear a bye, does not make Stop
block; **there is no way to reply to one**, so it cannot ask the peer to
choose (it exists because an agent holding a correction it could not send
stuffed it into `busy` three times and barged once). `attach` — a file copied
into the call *at send time* (an artifact that can change under the reader is
worse than none). `presence` — HEAD, branch, declared work, owned pids, from a
file: "is main running a gate", "whose QEMU is that" need no call. `busy
<text> [pids…]` — declare what you are doing and the pids you own **so a peer
does not kill them**; `""` clears. `beat` — the heartbeat the hooks stamp
(live = < 3 min).

**Disputes** (P11): `dispute "<claim>"`, then each side `measure "<cmd>"` —
`"none"` is a real answer; both `"none"` → **ESCALATE**. Zero disputes have
been opened on the Thylacine line in 22 calls / 149 turns; the mechanism's
trigger condition has simply not occurred — which is the argument recorded
against building an automated arbiter.

**The human seat** (P12): `yip ratify [call]` — CLI only; `yip switchboard` —
the operator's live view (transcript, presence, disputes; `r` ratify, `a`
arbitrate the selected dispute, `d`/`tab` cycle, `f` follow, `q`). **No key
speaks as an agent**; a test drives every printable key through the input
handler and asserts the turn count is unchanged — it fails the moment somebody
adds a convenient `s`-for-say. Hand-rolled ANSI over `stty`, so the binary
stays dependency-free.

**Doctor.** `yip doctor` checks identity, line, MCP declaration, hooks
(count and *single* registration across the settings pair), that the
configured binary **execs** (see the install trap below), peers' beats, and
what is ringing.

**Install trap, measured.** Never `go build -o <installed path>`: overwriting
the binary in place while `yip serve` runs from it can leave that path
**permanently SIGKILL-on-exec** — valid on disk, passing `codesign -v`, dead
at every exec, not clearing when the holder exits (3 of 4 attempts; 0 of 2
with rm-then-copy). It killed a live agent's hooks mid-merge with `Killed: 9`
and nothing naming yip. `make install` does rm-then-copy. Reinstalling
requires a Claude Code restart: the running server keeps its old inode.

### 4.10 The engine-room telegraph — resource leases

**Why.** P13. Three waiters, three failures, one day; the third could not have
been fixed by adding axes.

**How** (`yip resources | hold | release | steal`):

- **The unit is a physical machine, not a tool or a workload.** The first
  draft had "QEMU" and "CPU" as separate locks, which would let two agents hold
  two leases and saturate one set of cores while the protocol told both they
  were fine. QEMU is a *consumer* of cores (TCG is a CPU emulator; an HVF
  guest runs real vCPU threads; a `cargo build -j`, a TLC run, a sanitizer
  build each saturate every core) — one contention class. A local VM whose
  vCPUs are carved from the same cores is *part of* the machine, not a peer.
  In Thylacine: `mac` (the 8-core host, including its Parallels VM) and `pi`.
- **Acquire** is a compare-and-set on a local filesystem — `link(2)` onto the
  lease path fails if it exists — never read-then-write. FIFO queue: you take
  an unheld lease only if you are at the head, so a latecomer polling at the
  right instant cannot jump the line; a bounded `hold` (default 60 s, max
  600 s) can be re-issued without losing position (`Since` preserved, `Seen`
  refreshed; entries unseen for 15 min are pruned — two fields because one
  cannot do both). Re-acquiring your own lease renews it without a
  release/acquire gap a peer could win.
- **Expiry is wall clock** (default TTL 2 h, max 8 h — a backstop against a
  dead agent, not a normal path). There is a test pinning that expiry never
  depends on the heartbeat, so the "improvement" cannot be reintroduced.
- **An expired lease is not an open one**: `steal` needs a reason, is refused
  while time remains, and records `stolen_from` + `steal_why` so a reclaim can
  never read as an ordinary acquire in the log.
- **Release refuses to end someone else's claim** — the whole value is that
  the holder's word is authoritative.
- **`ViolationAdvice`** is prose, on purpose (no mechanism): identify the owner
  by cwd before believing anything; if a registered peer, tell them and give
  grace — they may have started before you acquired; if nobody registered,
  leave it and say so — it is not evidence of a breach; escalate if it
  persists.

**Two bugs found while building it, worth stealing.** The first exclusivity
test was **vacuous**: replacing the atomic create with a clobbering write left
it green, because `TryAcquire` returns at the "held by someone else" branch
long before it links — a sequential test never reaches the compare-and-set at
all. The replacement releases 24 goroutines at once and asserts exactly one
winner; sabotaged, it reports four. **A test whose subject is a race cannot be
written sequentially, however obvious the assertion looks.** Reasoning about
that surfaced the second: RFC3339 is second-granular, so two agents queueing
in the same second compare equal on `Since` and `sort.Slice` is not stable —
each could sort itself to the head. Tie-broken by agent name.

**Not yet general.** The resource registry is a compiled-in list
(`knownResources` in `resource.go`); adopting this elsewhere means editing it
or, better, moving it to a per-line `resources.json` (§9).

---

## 5. Setting it up from scratch

Prerequisites: Claude Code (TUI) with hooks and MCP; git with worktrees; tmux
3.x; python3; Go 1.22+ (for yip); a large-context model (the numbers below
assume ~1M tokens — scale them). macOS or Linux (note `setsid` is absent on
macOS and BSD `pgrep` omits ancestors — both bit this project).

### 5.1 Write the standing orders

Add to the project's `CLAUDE.md` (adapt names; keep the *reasons* — a rule
without its specimen gets optimised away). A minimal template:

```markdown
## Autonomy + escalation
Under granted autonomy, proceed on implementation, tests, audit triage, commit,
and push to your own branch. ALWAYS escalate: format/ABI breaks; destructive
operations (force-push, branch deletion, hard reset of shared branches, data
drops); architectural deviations from the design docs; cross-scope pivots;
anything visible to others (shared branches, PRs, external posts); significant
spend; anything unclear in the binding docs.

## The checkpoint contract (binding; every hand-back)
1. Account for every attached shell, monitor and background task: kill what is
   finished (by explicit PID, never by pattern), name what is left alive and
   why, or say "nothing running" -- silence is not the same statement.
2. The handoff is ALREADY written: pickup file, status doc, open findings are
   current before you hand back, so `/compact` never costs a round-trip. If the
   state is not compactable, say the one thing that would make it so.
3. Close with **Next** (one action) and **Ahead** (one line of the queued
   chunks to the arc's close).

## A checkpoint is not a stopping point
Under autonomy, land a chunk, report it in three lines, and open the next one
in the same run. The signal that ends a run is the CONTEXT CHECKPOINT WINDOW
line from the PostToolUse warner (three levels: checkpoint = carry the step to
a clean boundary then compact; approaching = wind down; wrap line = commit,
hand off, yield). Two conditions make this safe: the checkpoint contract still
fires at every checkpoint you run through, and if the warner is absent you
fall back to judgement (a signal that never arrives is indistinguishable from
one that has not arrived yet). Autonomy over SEQUENCING only; the escalation
list is untouched.

## Ending a turn is a decision, never a default
Emitting the full summary IS the yield. It belongs to STOPPING (the checkpoint
line, an escalation item, out of moves, the user asking). The tell that you are
handing back: a Key table or an Ahead line. Never pause without arming a
BOUNDED wake (background waiter that exits on the condition AND on the bound,
saying which). "Blocked" means out of MOVES, not out of the preferred move.

## Peer coordination (shared machines)
`presence` before `call`. `busy` before anything long, with the pids you own.
Announce the RESOURCE and the UNCERTAINTY, never a duration. `hold` the machine
BEFORE anything needing cores; `release` when the RESOURCE frees, not when your
workflow finishes. Holding is not a licence to kill; identify by cwd.
```

Put the "ending a turn" rule **also** in a memory file — memory is injected
into the running session and survives compaction; `CLAUDE.md` loads at
session start.

### 5.2 Lay out memory and the journal

Create `MEMORY.md` (index, one line per entry), `project_active.md`, one
`project_next_session*.md` per track, `TASK-ARCHIVE.md`, and `docs/JOURNAL.md`
with its conventions header. Decide the injection budget: whatever is
re-injected every turn (task lists, indexes) stays small.

### 5.3 Harness settings

In `~/.claude/settings.json`: `"autoCompactEnabled": false` (deliberate — the
agent or the human compacts at a clean boundary), your model/effort, and a
status line that shows context occupancy (the project's `statusline.py`
renders it from the status JSON with zero file reads; the stock one re-read
the whole multi-GB transcript per tick).

### 5.4 Install the watch instruments

Copy `ctx-hook.sh` and `resume-note.py` to `~/.claude/` (or version them in
the repo under `tools/hooks/` and symlink — they are shared by every agent on
the machine and unversioned today, which the field report flags). Wire them:

```json
{
  "hooks": {
    "SessionStart": [{ "hooks": [{ "type": "command", "command": "~/.claude/resume-note.py" }] }],
    "PostToolUse":  [{ "matcher": "*", "hooks": [{ "type": "command", "command": "~/.claude/ctx-hook.sh" }] }],
    "Stop":         [{ "hooks": [{ "type": "command", "command": "<repo>/tools/stop-hook.sh" }] }]
  }
}
```

**Editing a user-level hook is a live-fire operation**: every live session
executes it on every tool call. Write to a temp path and `mv` (an in-place
`sed -i` truncated `ctx-hook.sh` under a running session, which executed the
partial file and errored). Hooks **merge** across user and project/local
layers — a project's own PostToolUse hook does not shadow the user-level one.

Thresholds are environment-overridable: `CTX_CKPT`, `CTX_WARN`, `CTX_HARD`,
`CTX_LIMIT`; `STOP_FLOOR`, `STOP_MIN_TURNS`.

### 5.5 Install the change-of-watch pair

`tools/thyla-selfcompact.sh` and `tools/thyla-nudge-watch.sh`, executable, in
the repo (each worktree that should self-compact needs them on its branch —
the warner's checkpoint message points at the script only if
`$CLAUDE_PROJECT_DIR/tools/thyla-selfcompact.sh` exists, and otherwise tells
the agent to recommend `/compact` to the user). State under
`~/.claude/thyla-selfcompact/` (`<role>.state`, `log.tsv`, the note slots).
Teach the agent the note path (`<STATE_DIR>/<encoded-toplevel>.note`; the
producer still names it by path — the pane identity rides in the `.meta` the
script writes, and the consumer matches on that first).

### 5.6 The launcher

Adapt `tools/thyla-tmux.sh`: worktree paths, tmux session names,
conversation names. Inside each agent, name the conversation once with
`/name <handle>`. Set `@thyla-role` per pane. Run `thyla-tmux.sh status` to
see what is up without attaching.

### 5.7 Install the telephone

```
git clone https://github.com/apology-is-policy/yip && cd yip && make install   # -> ~/.local/bin/yip
cd <each checkout>; yip install [--as <name>] [--local] [--line <group>]       # merges .mcp.json + settings
# restart Claude Code in that checkout, then:
yip doctor
```

Worktrees of one repo join one line automatically. Prefer `--local` (and
gitignore `.mcp.json`) when worktrees merge into each other — the binary path
is host-specific. Then, in the standing orders, the coordination rules of
§5.1. Define your resources (edit `knownResources` until the registry is a
file). Open `yip switchboard` in a spare pane.

### 5.8 Prove each mechanism fires

Do not skip this; half the bugs above were caught by controls, not by
reasoning.

| Mechanism | Positive proof | Negative control |
|---|---|---|
| ctx-hook | `CTX_CKPT=1000 …` on a real transcript emits the checkpoint line once; a second call is silent; deleting the marker re-arms | below the line: no output |
| resume-note | arm a slot by hand (fresh `.note` → `.pending` + `.meta`), start a session: header names head/reason; slot deleted; `consumed` row | fresh transcript, no slot: nothing emitted |
| selfcompact `--check` | first run allows; static HEAD twice → BELAY rc 3; HEAD moved resets | dirty tracked change → REFUSED; not in tmux → REFUSED; untracked-only → allowed (the control that must not refuse) |
| nudge-watch | runtime-random marker rendered in a scratch pane: fires within ~2 s, ledger row | marker absent: silent to the bound, `nudge-timeout`, **and the process was alive the whole time** |
| stop-hook | a transcript ≥ floor with ≥ 6 assistant turns and no genuine user turn → block JSON; with tool-results and notifications interleaved → still blocks | below floor / above ceiling / few turns / `stop_hook_active` → exit 0 silently |
| yip | `make test` (Go tests + e2e over an isolated line); `yip doctor` in each checkout; from each checkout `yip presence` shows `(you)` on the *right* member | an uninstalled checkout attaches to a peer — the impersonation trap |
| leases | two shells `hold` the same resource: exactly one HELD, the other WAITING #1; `release` resolves it; `steal` refused while time remains | `go test -race` — the 24-goroutine exclusivity test |

### 5.9 First run

Submit one prompt. Watch the switchboard, not the panes. Next morning read
`docs/JOURNAL.md`, then `~/.claude/thyla-selfcompact/log.tsv` — **every
`allow` should have a `nudge-sent` and a `consumed` after it**; one that does
not is a session that sat at a prompt (§8 R1).

---

## 6. Operating it — the human's day

- **You set direction; the agents set pace.** Say "proceed autonomously" and
  what the arc is. Expect three-line checkpoints and a stopping report only at
  the checkpoint line or an escalation.
- **You are called for** (and only for): design forks (surfaced as structured
  option sets with the research attached), format/ABI breaks, destructive
  operations, anything outward-facing, disputes where both sides answered
  "none", a **belay** (two compactions without a commit — read the note; it
  says what was attempted), and "blocked: needs a human decision / hardware /
  quota".
- **`/compact` is still yours** — the note header tells the resumed session
  whether it woke itself or was compacted by you (and may be redirected).
  "Please pause at session start" is honoured as case 2 of the stop check.
- **Ratify and arbitrate from the switchboard**, never by telling one agent
  to tell the other.
- **Read the ledger and the journal**, not the transcript. Presence tells you
  who is on which HEAD and who is busy; `yip resources` who holds the machine.
- **When you edit a rule mid-session, remember it is not loaded** until the
  next session start (or write it to memory).

---

## 7. What it has bought (evidence, not adjectives)

Numbers from the running system as of 2026-08-17; every one has a source.

| Claim | Evidence |
|---|---|
| Runs cross context boundaries unattended | ledger: two full automatic cycles (`allow` → `nudge-sent` 4–11 s → `consumed` 90–120 s) on 2026-08-16 for main; journal 2026-08-16 opens "Resumed from a self-compaction at the 600k checkpoint. The nudge fix worked on its first live test" |
| Compaction volume the human no longer hand-pastes | 163 boundaries in main's live transcript, 123 in aux's, 58 in the vault's (`isCompactSummary` records) — each was a manual `/compact` + paste before the note hook, and is a manual `/compact` still on the tracks without the self-compaction pair |
| The Stop hook catches unearned yields and yields to earned ones | fired at 132k/20 turns this session; answered "2"; no repeat |
| Cross-track review finds what one track cannot | field report §3: three files one sweep could not see; a stale-doc line refuted by measurement (5 scenarios/76 min/4 burned retries contended vs 8/10 min/0 quiet); every cross-track finding right about the defect (and wrong about the fix — P15) |
| The channel carries what a human could not | 22 calls, 149 turns, 25 notes, ~618 KB of turn text since 2026-08-04; the largest single merge instruction ~14 KB |
| Serialization pays in evidence, not just wall clock | after standing down: 33/35, 0 fail, 0 retries burned vs 4 burned contended — a burned retry cannot distinguish "host was busy" from "intermittently broken" |
| Presence prevents kills | `busy` with owned pids; zero peer processes killed by pattern since the rule; two barges in a day both legitimate and both marked |

What it has *not* needed: an automated arbiter (0 disputes), the belay (0
static-HEAD pairs so far), a `steal` (0 recorded).

---

## 8. Review findings — the sharp edges (2026-08-17)

Ordered by what they cost if unaddressed.

**R1 — DEFECT (evidenced): the resume-note slot is keyed two ways that are
not the same key.** Producer: `git rev-parse --show-toplevel`. Consumer:
`basename(dirname(transcript_path))` = the directory the session was
*launched* in. They coincide only when a session is started from its own
worktree root. The vault agent is launched from the main checkout and `cd`s
into its worktree (its transcript sits under the main project key; 599 `cwd`
records in `thylacine`, 6648 in `thylacine-vault`). Its 2026-08-16 10:44:32Z
self-compaction landed (`/compact` summary at 10:48), the note was armed under
`…-thylacine-vault`, the consumer looked for `…-thylacine`, fell through to
the transcript heuristic **silently** (the fallback path wrote no ledger row),
and the vault sat at a prompt for the rest of the day (it also predated the
watcher). The `.pending` file was still on disk twenty hours later. **The
ledger recorded it — `allow` with no `consumed` — and nothing read the
ledger.** *Fix — applied by the main track within the hour of the report
(2026-08-17, `~/.claude/resume-note.py` + the producer's comment):* the
consumer keys the slot on `$TMUX_PANE` — tool calls inherit it (measured
`%1`) and hooks are children of the same process, so it is one identity handed
to both sides by tmux, not two heuristics — with the path key as the fallback
outside tmux; it writes `orphan-note` on the fallback path when a slot it did
not match is left armed, and `stale-discarded` (rather than injecting) for a
matched note older than 30 min. The vault's orphan will be discarded loudly by
the next vault session start in its pane. Still owed: a `--check`/doctor-side
flag for a `.pending` older than `NOTE_MAX_AGE`, so the operator sees it
without a session having to start. Filed to memory
(`bug_selfcompact_note_key_divergence.md`).

**R2 — DRIFT: two install paths, one version string.** aux and vault are wired
(`.mcp.json` + hooks) to `~/.yip/bin/yip` (Aug 5 build); main to
`~/.local/bin/yip` (Aug 16, leases). "MCP tools after you restart" is false
for the former — a restart re-execs the old binary. `yip version` prints
`0.1.0` for both, so `doctor`'s "binary runs" cannot tell them apart. *Fix:*
re-run `install --local` from the current binary in each checkout + restart;
stamp the version from `git describe`; have `doctor` compare the wired binary
against the newest install.

**R3 — SPECIMEN ERROR: "a fourth unregistered session".** The lease
rationale (the `resource.go` header and the lease commit body) cites a
`caffeinate` from a fourth session nobody had registered. Its parent was
`claude --resume aux_gfx` — which is the *aux* agent (the launcher's own
default `AUX_CONV`). The rule it justified (no licence to kill; identify by
cwd) stands on its own; the specimen is fictional. Lesson: a process census
needs a control, and the control is your own identity (`ps -o ppid -p $$`).

**R4 — GENERALITY: a user-level hook wired to a repo path.** `~/.claude/
settings.json` points `Stop` at `<main-checkout>/tools/stop-hook.sh`, so every
project on the machine runs this project's stop check, and every worktree runs
*main's* copy — a branch checkout on main that lacks the file silently removes
the hook everywhere (fail-open, so harmlessly, but invisibly). Also
`ctx-hook.sh` and `resume-note.py` are unversioned in one home directory with
three agents depending on them. *Fix:* version all three under `tools/hooks/`
with an install step; wire project-specific hooks from project settings, user
ones from user settings; never cross.

**R5 — a ledger with no reader** (the general form of R1). `log.tsv` is
written by four writers and, until today, read by nobody. The R1 fix makes the
consumer *write* the miss (`orphan-note`, `stale-discarded`), which turns a
silent orphan into a timestamped line — but a line is still not an alarm. A
one-line check — "every `allow` in the last N hours has a `consumed`; no
`orphan-note`" — belongs in `doctor` or in the next-morning routine (§5.9).

**R6 — the belay has never fired.** The most safety-critical path in the
self-compaction governor is the least exercised. Owed: a deliberate drill
(two `--check`s with a static HEAD, then a real one) with the ledger row as
the witness.

**R7 — the shared memory index has no claim protocol.** `MEMORY.md` is
written by three tracks; two agents wrote the same lesson three minutes apart
under two numbers; one had to back out mid-edit. Single-line edits mitigate;
a `yip claim <path>` advisory lock or per-track indexes merged on demand would
close it.

**R8 — bare cross-track identifiers.** Each track numbers findings
independently, so `#237` names two different things; a wrong prefix was pushed
into six documents. Rule: never a bare `#N` across the boundary; yip could
flag one in a call body.

**R9 — `presence` shows declared state, not observed processes.** It reported
"idle" while a peer's QEMU burned two cores because `busy` had not been set;
`busy` text also goes stale silently. Age the declaration; consider surfacing
owned/observed pids.

**R10 — the compiled-in resource registry** (§4.10). Move to a per-line file
before anyone else adopts it.

**R11 — the ctx warner's `LIMIT`** (900k against a 1M window) makes every
percentage read conservative. Known and chosen; document it where the numbers
are read.

None of these is a hole in the design; R1 is a hole in a *premise* the design
stated ("no shared config to drift"), which is why it is first.

---

## 9. Adapting it

- **Budget lines**: 60% / 75% / 88% of the window; keep the checkpoint
  once-per-crossing and the stopping levels insistent.
- **Stop-check floor and turn count**: the floor separates conversation from
  running (120k here); six turns separates a reply from a run. Tune by
  watching where it fires wrongly, and keep fail-open.
- **The note freshness window** (600 s) and thickness (200 chars): the first
  is the interval between "I decided to compact" and the keystroke; the second
  is "too thin to orient a fresh context".
- **The belay** (2): one static-HEAD compaction is a slow chunk; two is a loop.
- **Lease TTLs** (2 h default, 8 h max) and the queue's staleness (15 min):
  the first is a backstop, not a promise; the second is how long a waiter may
  go quiet without losing its place.
- **Resources**: one per *physical* machine. Not per tool.
- **Without tmux**: self-compaction cannot type into its own pane; you can
  keep everything else (warner, note, stop check, yip) and let the human press
  `/compact` at the checkpoint line — the note and the header still work.
- **Without a second agent**: drop yip; keep the launcher's discipline (named
  conversations, one agent per tree).

---

## 10. Glossary

| Term | Meaning |
|---|---|
| watch | one context window's worth of autonomous work, from a (self-)start to a compaction |
| checkpoint | a resting point that returns control — a landed chunk, a closed audit, a surfaced fork; under autonomy, run *through*, not stopped *at* |
| checkpoint line / window | the context budget (60%) at which the run should compact; announced once per crossing by the warner |
| wrap line | the budget (88%) at which the run must commit, hand off and yield |
| resume note | the agent's deliberate note-to-self across a compaction: what is in flight, what must NOT be redone; a one-shot slot |
| nudge | the keystroke, typed during the rebuild by a detached watcher, that wakes the resumed session |
| belay | the governor's refusal to self-compact after two consecutive compactions with a static HEAD; hands back to the operator |
| ledger | `log.tsv` — one row per arm / consume / nudge event; the out-of-band witness |
| floor (yip) | the right to speak an assertion: held by whoever did NOT write the last turn |
| note (yip) | a one-way message that cannot carry a decision (no reply affordance) |
| ratify | a turn authored by the human, via CLI/TUI only |
| lease | a wall-clock-expiring claim on a physical machine; ended only by the holder's release or a recorded steal |
| standing orders | the behavioural contract in `CLAUDE.md` + memory |
| call the master | an escalation item — the run stops and asks |

---

*Files referenced (Thylacine main worktree unless noted): `CLAUDE.md`
(§"Autonomy + escalation", "The checkpoint contract", "A checkpoint is not a
stopping point", "Operational summary patterns"), `docs/JOURNAL.md`,
`docs/AUTONOMY-FIELD-REPORT.md`, `tools/stop-hook.sh`,
`tools/thyla-selfcompact.sh`, `tools/thyla-nudge-watch.sh`,
`tools/thyla-tmux.sh`, `~/.claude/ctx-hook.sh`, `~/.claude/resume-note.py`,
`~/.claude/settings.json`; memory `feedback_turn_end_is_a_decision.md`,
`feedback_clean_wrap_over_autocompact.md`, `reference_yip_relay.md`,
`bug_selfcompact_note_key_divergence.md`; yip: `README.md`, `hooks.go`,
`resource.go`, `main.go`.*
