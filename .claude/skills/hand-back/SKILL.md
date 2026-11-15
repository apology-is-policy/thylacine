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

**Nothing enforces this; it is yours to remember.** A `Stop` hook
(`tools/stop-hook.sh`, built 2026-08-16 after a run stopped at a checkpoint
having written the very `Ahead` line named above as the tell) used to ask the
question below once per mid-budget stop. The operator retired it on 2026-11-13
as obsolete, built for older models, and the script is gone. Its question
stands as the rule: before you end an autonomous run below the checkpoint line
(`CKPT` in `.claude/ctx-thresholds`), say which of four cases this is. Three
make stopping CORRECT -- an §"Autonomy + escalation" item, a question from the
user you have now answered, a genuine block -- so name it in a clause and stop.
The fourth is everything else: **open the chunk you just named on your own
`Next` line instead of yielding.**

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
