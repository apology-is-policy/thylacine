# Autonomy, self-compaction and yip — field report

**Not scripture.** A running log of how the operating machinery actually behaves
under use, with the evidence and the fixes it wants. Started 2026-08-16 at the
user's request. Append; do not rewrite.

---

## 1. Self-compaction

### What was proven, end to end

The first live self-compaction (`tools/thyla-selfcompact.sh`, head `804b13fd`)
worked in every link, and each link left evidence:

| Link | Evidence |
|---|---|
| role resolution | `main`, from the tmux `@thyla-role` option (not the git-basename fallback) |
| decision recorded BEFORE acting | `log.tsv` row written before the keystrokes |
| progress gate armed | `main.state` = `last_head=804b13fd`, `no_progress=0` |
| slot armed then consumed EXACTLY once | state dir left holding only `log.tsv` + `main.state` |
| far side took the slot, not the fallback | header carried head/reason/at, matching the log row to the second |

### What it found (bigger than the feature)

**`additionalContext` is passive.** The note arrived perfectly and then nothing
happened — a SessionStart hook seeds a context, it does not submit a turn. The
resumed session sat at an empty prompt holding a note saying "no one is waiting
on you." The whole mechanism was **one keystroke short of autonomous**, and that
gap is invisible in every component test because every component worked.

Fixed at `9387f308` by queueing a nudge behind the `/compact` through the same
keystroke channel.

### Still untested

- **The nudge.** Whether queued input survives the context rebuild is unknown;
  the next self-compaction answers it for free. If it does not survive, the
  fallback is `resume-note.py` sending it at SessionStart — which is why `pane=`
  now rides in the `.meta`.
- **The belay.** Needs two consecutive static-HEAD compactions. Never exercised.
  It is the most safety-critical path and the least tested.
- **The transcript fallback.** Only the slot path has run live.

### Open decision (user's)

`~/.claude/ctx-hook.sh` assumes `LIMIT=900000`; the real window is **1M**. So
`HARD=880k` announces "AT THE WRAP LINE" at 88%, not 98%, and every reported
percentage is wrong by that ratio. `CKPT=600k` survives (60% of 1M). User chose
to keep 900k — worth knowing it now reads *conservative*, not wrong.

### Suggested improvements

1. **Version `~/.claude/ctx-hook.sh` and `~/.claude/resume-note.py`.** All three
   agents depend on them and they exist only in one home directory, unbacked. A
   `tools/hooks/` copy plus an install step would make them reviewable and
   recoverable. (Offered twice, not yet taken.)
2. **The consume side logs now** (added 2026-08-16) — before that, a successful
   re-injection was invisible from the terminal and indistinguishable from the
   hook never firing, which is exactly the conclusion the operator drew on the
   first live run. **A success you cannot observe is one you have not verified.**
3. **Editing a shared hook is a live-fire operation.** An in-place `sed -i` on
   `ctx-hook.sh` crashed aux's running session mid-write (they executed the
   truncated prefix). Always write `.part` then `mv`. This is now habit but it
   is not enforced by anything.

---

## 2. Autonomy — the 600k contract

### It did not hold on behaviour alone. Twice.

Both times the run stopped at a chunk boundary well under budget (~160k and
~270k of 600k) and the user had to ask why.

**Root cause 1 — the summary IS the yield.** Final text ends the turn, and
nothing in this harness restarts a turn but the user. CLAUDE.md's
operational-summary section said "the response to a completed audit / chunk",
so scripture instructed a turn-ending ritual at exactly the moment the 600k rule
said not to stop. The concrete ritual beat the abstract rule. Fixed at
`d0350060`: the summary now belongs to STOPPING (600k / escalation / out of
moves / user asks), a pass-through checkpoint gets three lines and the next tool
call, and the tell is "if you are writing a Key table or an Ahead line, you are
handing back."

**Root cause 2 — landing a rule is not adopting it.** CLAUDE.md loads at SESSION
START. The fix above was written to CLAUDE.md mid-session and therefore was not
in the context of the session that wrote it — which promptly broke it again.
Fixed by also writing it to memory (`feedback_turn_end_is_a_decision.md`), which
IS injected and survives compaction.

**These are the same shape three times in one day**: a message arrives and
nothing presses enter (the resume note; the checkpoint instruction; the fix to
the checkpoint instruction). A channel that DELIVERS is not a channel that
CAUSES.

### The user's correction, now standing

> "there should always be some mechanism to wake you when you pause — be sure to
> always set up monitors, crons, etc."

This is the missing half and it is now recorded as a rule. Applied the same
hour: blocked on the host being busy, armed a bounded background waiter that
exits when the host frees. **It immediately proved its own design** — the pid it
watched died, but a second QEMU had started, and because the waiter required
BOTH "pid gone" AND "zero qemu" it correctly did not fire a false host-free.
Keying on the pid alone would have handed me the machine mid-gate.

### Suggested improvements

4. **The `Stop` hook is the only structural fix.** Everything above is
   behavioural and therefore only as good as remembering it. The design was
   worked out (default-STOP; a one-shot "baton" file the agent must re-arm each
   turn; escalation honoured by construction because an escalation simply never
   arms it; a cap on consecutive resumes reset by HEAD moving; a ledger row per
   resume). The user chose behaviour-first deliberately, to keep a cheap
   override — a turn boundary is where they can step in. That trade is real and
   worth revisiting only if the behavioural version keeps failing.
5. **The cap value is the one number to set** if the Stop hook is ever built.
   Suggested 8 consecutive auto-resumes without a commit.

---

## 3. yip and agent coordination

### It is working, and the value is not marginal

Concrete wins in one day, none of which either track produces alone:

- **aux found three files my sweep could not see.** I grepped the specific
  `usr/apps/*` paths I already knew; they grepped the prefix. Different blind
  spots, so two independent sweeps caught different subsets of one defect.
- **aux supplied the warrant** that turned a stale-doc cleanup into a
  demonstrated defect: the same false sentence produced the same wrong
  operational call in BOTH readers on the same day, including the agent the
  sentence was about.
- **aux's A/B closed the contention question** with a measurement (5 scenarios /
  76 min / 4 burned retries contended vs 8 / 10 min / 0 quiet) — and that
  measurement FALSIFIED a line I had just written, which is the most valuable
  thing a peer can do.
- **A two-hour quiet-host hold was negotiated and honoured** cleanly, twice.
- **The stale-HEAD banner on every turn** ("written when your HEAD was X; you are
  now at Y") is excellent and has prevented acting on stale computation.

### Problems found, with fixes

6. **Bare `#NNN` is ambiguous across tracks and it has already cost real
   damage.** Main and aux number findings independently. `main#237` is a closed
   GPU research task; `aux#237` is the aux-track scripture defect. I cited
   "main#237" in six documents and pushed them. aux warned about exactly this in
   August ("your #200 is the QEMU-vanishes bug; ours is #108 — same digits").
   **Fix: never write a bare `#N` across the boundary; always `main#N` / `aux#N`.
   Better, have yip reject or flag an unprefixed `#N` in a call body.**
7. **`presence` reports "idle" while the peer's QEMU is burning two cores.**
   Presence shows declared state, not processes, and aux had not set `busy`. I
   nearly took the host on that reading; `ps` is what saved it. **Fix: make
   `presence` surface observed owned pids, or at minimum print "declared state
   only — check `ps`" in its own output.** `busy` already accepts a `pids` array;
   nothing enforces using it.
8. **`busy` text goes stale silently.** Mine still read "compacted at 804b13fd"
   hours later. **Fix: age the declaration and show "(set 3h ago)" so a reader
   discounts it.**
9. **The shared `memory/` directory has no claim protocol.** aux and I
   independently wrote the SAME lesson three minutes apart under two numbers
   (`bug_237` and `bug_257`), and separately aux had to back out of MEMORY.md
   edits and ask for a handback because I was compacting it. It resolved
   politely both times, but only because both sides were paying attention.
   **Fix: either per-track index files merged on demand, or a `yip claim <path>`
   advisory lock.** MEMORY.md is the highest-contention file in the system and
   the one whose corruption costs the most.
10. **The `bye`-both-sides close works well** and the floor model is right. Two
    barges today were both legitimate (correcting a false claim in an unread
    message; releasing a host hold). Barges being permanently marked in the
    transcript is the correct disincentive.
11. **"Announce the RESOURCE and the UNCERTAINTY, not the duration"** (aux's
    protocol, adopted into CLAUDE.md). "unknown, 6 cores" tells the peer to
    serialize; "30 min" implies a bound nobody can honour. This is the single
    best coordination rule to come out of the day.

---

## 4. The one-line summary

The machinery works. Every failure so far has had the same shape — **a message
was delivered and nothing acted on it** — and every fix has been to add the
actor, not the channel.
