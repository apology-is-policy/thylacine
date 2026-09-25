---
name: hand-back
description: Use when actually STOPPING an autonomous run and handing back to the user (the compaction line, an escalation item, a genuine block, or the user asks for a report) -- the full end-of-run summary format with Focus, Arc state, Key, metrics, exit criteria, Running, Handoff, Next and Ahead. Not for checkpoints you run through.
---

# Hand-back summary

> Moved verbatim from `CLAUDE.md` on 2026-09-23 (the CLAUDE.md trim). CLAUDE.md keeps the three-line checkpoint rule.
> Section headings keep their original levels.

## Operational summary patterns

**FIRST, THE TRIGGER — because this section reads as "chunk done -> write this", and that instruction is what ends the run.** Emitting the full summary IS the yield: final text ends the turn, and in this harness nothing restarts you afterwards but the user. So a summary written at a completed chunk silently converts "running through checkpoints" into "handing back", no matter what CLAUDE.md's compaction rules say. Measured 2026-08-16, on the first autonomous run: the chunk landed, the summary got written because scripture said to, and the run stopped at ~160k of a 600k budget. The concrete ritual beats the abstract rule every time.

**So the full summary below belongs to STOPPING, not to finishing.** Write it when you are actually handing back: at the compaction line, on an item from §"Autonomy + escalation", when genuinely blocked, or when the user asks. At a checkpoint you are running THROUGH, the checkpoint contract is discharged in **three lines or fewer** — what landed (hash), what is running, what is next — and then you **open the next item in the same turn**, without final prose.

**The tell:** if you are writing a `Key` table, an `Arc state` field, or an `Ahead` line, you are writing a hand-back. Stop and ask whether you actually intend to stop. If you do not, delete it and make the next tool call instead.

**The `Stop` hook now exists** (`tools/stop-hook.sh`, user-requested
2026-08-16 after a run stopped at a checkpoint it should have run through --
having written the very `Ahead` line named above as the tell). This paragraph
used to say the hook was "deliberately not built"; it was built precisely
because behaviour that is only as good as remembering it was not good enough.

What it does, so you recognize it rather than argue with it: on a stop it
computes the same budget `ctx-hook.sh` does, and if you are **between 120k and
the checkpoint line** (`CKPT` in `.claude/ctx-thresholds`) and have taken **>= 6 assistant turns since the user
last spoke** -- i.e. an autonomous run, not a reply -- it blocks ONCE and asks
which of four cases applies. Three of them (an §"Autonomy + escalation" item, a
question you have now answered, a genuine block) make stopping CORRECT: name it
in a clause and stop, and it will not ask again. The fourth is the one it exists
for -- **open the chunk you just named on your own `Next` line instead of
yielding.**

It is a question, not a veto, because only you can tell an earned yield from an
unearned one. It stays silent below 120k (conversational), at/above the checkpoint line
(stopping is what scripture wants there, and a second voice contradicting
`ctx-hook.sh` would be worse than silence), and whenever `stop_hook_active` is
already set. **It fails OPEN on every error path** -- a Stop hook that failed
closed could trap a session in a loop it cannot talk its way out of, which is
far worse than a missed nudge. Discrimination-tested across all seven
conditions, including the two legs most likely to be silently wrong: a
tool-result and a system notification must NOT count as "the user spoke", or the
counter resets constantly and the hook goes quiet during exactly the runs it is
for.

It lives IN THE REPO rather than in `~/.claude/` (where `ctx-hook.sh` sits, and
whose absence CLAUDE.md's compaction rules already flag as leaving that rule
with no brake). One copy, no sync obligation, and it survives a fresh clone;
`~/.claude/settings.json` points its `Stop` event at this path. Install on a new
machine is that one settings entry.

End-of-iteration summaries (the response to a completed audit / chunk) follow a consistent structure for fast review.

**Order matters: orientation FIRST, detail after.** The user does not carry the
identifier map in their head and does not remember where the arc was paused.
Open with what this session was about and how it serves the arc; only then the
journal. (Ratified 2026-08-14 at the user's request.)

```
## <one-line title: what this session was about>

**Focus**: <1-2 sentences: what this session actually worked on>.
**Arc fit**: <how it serves the current arc's direction / why it was worth
doing now>.

**Arc state**: ON ARC — <the chunk being built>.
   | PAUSED: the arc is stopped at <exact position>; this session is a side
   quest on <the soundness / harness / instrument problem that preempted it>.
   **Resumption needs**: <the specific remaining items, in order>.
   (OMIT this field entirely when directly on arc — do not write "n/a".)

**Key** — every identifier used below, in words. No bare ids anywhere:
| Id | Is |
|---|---|
| <#N / C-x / P1a / I-nn> | <plain-language name, <=12 words> |

**Arc metrics** (current values; a metric with no measurement says so):
| Metric | Value | Measured | Source |
|---|---|---|---|

**Exit criteria** (the arc's ratified bar + how we move toward each):
| Criterion | Target | Now | Moving via |
|---|---|---|---|

**This iteration landed (N new commits, tip <hash>)**:
- <hash1> — <one-line scope>

<the detailed journal: what was found, what it means, what went wrong and how
it was caught. Keep this rich — it is the part worth reading.>

**Posture**: <suites> × (default + ASan + TSan) green. <spec count> specs
clean. test_<X> at <count>.

**Running**: <nothing running | what is alive and why, one line each>.

**Handoff**: current at <tip> — compactable. | NOT compactable: <the one
blocker>.

**Next**: <the single immediate next action>.
**Ahead**: <chunk> -> <chunk> -> <chunk> -> <arc close>.

**Memory**: <files updated>.
```

This structure lets the user (or a future session reading the conversation log) reconstruct state in under 30 seconds.

Field notes, each earned:

- **`Key` is not optional and not decorative.** Sessions accumulate dense
  identifier vocabularies (`C-0`, `P1a`, `#240`, `I-45`, `Warp-C`) that are
  perfectly legible in-session and opaque a day later. Expand every id used in
  the summary, including ones that feel obvious. A summary the reader must
  decode is a summary that does not work.
- **`Arc state` exists because side quests are the norm, not the exception.**
  The whole-system-stewardship rule guarantees that surfaced defects preempt
  chunk work — so the user is frequently reading a report about something
  other than the arc they last approved. Say where the arc is parked and what
  it is waiting on, or they cannot tell a detour from a change of direction.
- **`Arc metrics` + `Exit criteria` answer "are we winning?"** A chunk-by-chunk
  narrative can read as steady progress while the number that defines success
  has not moved. State the bar, the current standing against it, and the
  mechanism that closes the gap. **Never quote a metric without its
  provenance** — a figure from a different workload, lane, or host is a
  different number wearing the same units (#236 is the standing example: two
  lanes disagreed 2x on the same renderer at the same resolution).
- The last four fields are the checkpoint contract made concrete — `Running`
  answers "is Claude still working?", `Handoff` answers "can I compact right
  now?", and `Next`/`Ahead` answer "where are we in the arc?". Emit all four at
  every checkpoint even when the answer is boring; a missing field reads as an
  unknown, and the whole point is that the user should not have to ask.

---
