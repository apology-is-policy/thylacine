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

- ~~**The nudge.**~~ **ANSWERED, and not the way this predicted** — see §1b.
- **The belay.** Needs two consecutive static-HEAD compactions. Never exercised.
  It is the most safety-critical path and the least tested.
- **The transcript fallback.** Only the slot path has run live.

## 1b. The nudge — measured, and the obvious diagnosis was wrong

The queued nudge did nothing. The tempting conclusion was "the input queue only
holds one message" or "`/compact` discards queued input". **Both are false, and
the operator supplied the correction that made the real mechanism visible.**

What was actually measured:

| Probe | Result |
|---|---|
| `/copy` + Enter, then a plain message + Enter, no delay | **both arrived** — the queue holds two, and a slash command followed by a message is exactly the failing shape |
| the same two behind `/compact` | the second never appeared, and the operator confirms the text was never even visible |
| the pane afterwards | 45 lines — **`/compact` clears the scrollback**, so nothing of the attempt survives to inspect |

So the queue is fine. What a queued message does not survive is **the rebuild**:
queued *before* the compaction it belongs to the session being torn down and
goes with it. The operator's manual habit is the working version and the thing
worth copying — submit `/compact`, wait for it to *start*, and type **while it
runs**, so the message lands in the session being *built*. **The difference is
about two seconds in when the keystrokes are sent, and it is the whole
difference between waking and not.**

Fixed by `tools/thyla-nudge-watch.sh`: a detached watcher armed *before* the
`/compact`, polling the pane for the compacting state, then typing. Keyed on the
observable transition rather than a guessed delay, because the compaction's
duration varies with the summary length — a sleep long enough to be safe is also
long enough to miss a fast one. A finished-state stem is the second trigger, so
the window cannot be missed by being slightly *late*, only by being absent.

**Two lessons, both about controls, both cheap and both nearly missed:**

- **The negative control passed because the watcher was never running.** The
  first version used `setsid`, which **macOS does not ship**; it died instantly
  and "nothing fired" was perfectly true. Only the second leg — *is the process
  still alive?* — caught it. A negative assertion is satisfied by a broken
  fixture ([[bug-215-negative-assert-satisfied-by-broken-fixture]]).
- **`capture-pane` sees the TUI, not tool output.** A call's *command text*
  renders; its *result* never does. The first positive control tried to publish
  a marker by `echo`ing it and silently could not work. This also means the
  marker must never be passed as an argument — the launching command line is
  itself rendered, so it would match itself instantly.

**Residual risk, named rather than papered over:** neither marker has ever been
observed in a real pane; they are inferred from the client's behaviour, and the
only way to see the real string is to run a real compaction. Mitigated by
matching the case-insensitive *stem* (surviving "Compacting conversation…" vs
"Compacting…") and by the timeout logging `nudge-timeout` — so a wrong guess
fails **loudly in the ledger** rather than silently as a session that just sat
there. The next real compaction settles it either way.

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

## 2b. Long gates vs the harness — the detached-gate pattern

**The harness stops its own background tasks, and it did it twice in one hour**
(status `killed` / "was stopped", which is the #128 discriminator for
harness-side vs a real external kill). The first casualty was the LS-CI run
itself; ~35 minutes of gate would have been lost silently.

The shape that survived it, and the one to use for anything long:

1. **Detach the WORK from the harness.** `nohup … > log 2>&1 < /dev/null &`
   launched from an ordinary Bash call. It reparents, so a harness stop cannot
   reach it. (Do NOT combine `&` with `run_in_background` —
   [[feedback-background-double-detach]]: the `&` detaches and the completion
   notification then describes the LAUNCHER, not the work.)
2. **Make the WAITER disposable.** A separate bounded `run_in_background`
   waiter provides the wake. When the harness killed *that*, the gate kept
   running underneath and re-arming cost one call. Verified live: waiter dead,
   `test-interactive.sh` and six qemu/expect processes still up.
3. **Never pipe gate output** (aux, after four repeats): `cmd | tail -N`
   buffers, so a killed run yields an empty log, AND the pipeline's exit status
   is `tail`'s, so failure reads as success. I did exactly this to LS-CI one
   hour after adopting the rule. Redirect to a file and read the file.

The general principle: **the thing that does the work and the thing that
reports it should have independent lifetimes**, so losing the reporter never
loses the work.

## 2c. The recurring failure family, now at five instances in one day

Every one of these returned a **confident wrong answer instead of an error**,
and each was caught only by contradiction with some other observation:

| Query | Reported | Truth |
|---|---|---|
| `git rev-parse main:<missing>` | the path, echoed back — non-empty, so every truthiness test reads it as a HIT | absent |
| `git log --diff-filter=D` empty | "never deleted" -> "still present" | a rebase drops a file with no deletion record |
| zsh `$files` unquoted into a checker | "1 file checked, 1 finding" | 14 paths passed as ONE argument; the finding was the open() error |
| a failed zsh glob into a counter | `stageC-markers=0` — reads as "the probe never ran" | the glob matched nothing because the path was a file, not a dir |
| a token census over `notes.c` | "confirmed: nothing FP/SIMD is saved" | `fp_save_area(t->note_saved_fp)` contains NONE of the register-name tokens I grepped for |

The last is the sharpest and it was mine: I enumerated **register names** when
the thing I needed was a **mechanism name**, and a good fix does not mention
the registers it saves — it calls a routine. I was one step from opening a P1
soundness hunt into a bug fixed weeks ago.

**The rule, aux's formulation, which is larger than any of the individual
ones:** *a tool given a malformed or mis-scoped query reports a plausible
RESULT rather than an error.* So: **no census, existence claim, or gate result
without a control that has been shown to detect the negative.** In the FP case
the control was one line — "does this pattern match the known-present case?"

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

## 3b. The three-agent day — what cross-track review actually bought

2026-08-16 ran main + aux + vault concurrently for a full day. The headline:
**every single cross-track finding was RIGHT about the defect and WRONG about
the fix**, in all three directions. That is a pattern, not a coincidence, and it
is the strongest argument for the arrangement rather than against it.

| Reporter | The finding (correct) | The prescription (wrong) |
|---|---|---|
| vault -> main | `loom.c`'s sqpoll thread-ledger deref is unbackstopped while its page-ledger twin is validated — and it is the I-32-breaking direction | "route it through `loom_owner_live(l)`" — which returns NULL on every rollback path and would have **leaked a thread charge permanently** |
| aux -> main | `notes.c`'s no-handler stop arm is covered by nothing; deleting it reddens no test | "if unreachable it is dead code, delete it" — it is an unconstructed STATE, and deleting it removes I-20's stop leg |
| main -> aux | native programs install no note handler (`T_SYS_NOTIFY` appears once, as a constant) — correct and load-bearing | "so it is the default path all 51 coreutils take" — **false**, and aux's own message contained the datum falsifying it |

**So the rule to operate by: take the SMELL, re-derive the REMEDY.** Never apply
a peer's fix without walking the code it touches — and never discount the
reporter when their fix is wrong, because the finding survives independently. In
all three cases the defect was real and worth the round trip.

### The mechanism, which vault named better than anyone

The descriptive half of an analysis is read OUT of the evidence; the generative
half is pattern-matched off the SHAPE of the problem. **Generation never
consults the analysis, because prescribing does not present itself as a step
that HAS inputs — it arrives wearing the confidence the analysis earned.**

Three instances in one day, from three authors, including one (mine) twenty
minutes after having the mechanism explained. That rules out carelessness. The
adopted rule, cheap and the only one that would have caught all three:

> **Before asserting a claim or a fix, re-read the evidence you were GIVEN as if
> someone else wrote it, against your conclusion.** Not "is my conclusion
> sound?" but "does anything already in front of me contradict it?"

Advance tell: **a prescription that argues for its own smallness has not been
checked against the thing that makes it large.**

## 3c. Host contention — the protocol worked, after I broke it

I launched a full bar at 10:18:18Z into a host aux had claimed at 10:14:27Z. I
had read their announcement and started anyway.

What made it recoverable was measuring instead of arguing: **load 7.32 on 8
cores with only ~115% total qemu** — my smp4 guest was getting 106% of the ~400%
it wants, so both gates were queueing rather than running. That is a number, not
an opinion, and it settled it in one command.

12. **Precedence is by announcement time, and it needs to be stated as a rule.**
    Whoever announced first owns the host; the second party stands down. Without
    that, both parties reason "I'm already running" and neither yields. Cost of
    yielding here was ~10 minutes of discarded gate; cost of not yielding is two
    ambiguous results, and by aux's own A/B a contended LS-CI is 76 min with 4
    burned retries against 10 min with 0.
13. **`presence` still cannot answer "is the host busy".** It reports declared
    state, and the declaration goes stale. `ps` + `uptime` is what actually
    answered it, both times. Making `presence` surface observed qemu/build pids
    would close this — it is the single highest-value yip change on the list.
14. **KILLING YOUR OWN GATE IS THE MOST DANGEROUS ROUTINE OPERATION IN A
    MULTI-AGENT SESSION, and a pattern kill is how you take out a peer.** Two
    `ci-smp-gate.sh` were live and one was aux's; `pkill -f qemu` or
    `pkill -f ci-smp-gate` would have destroyed a 40-boot run mid-flight. What
    worked: walk each qemu's **ppid chain** to its owning gate, kill only your
    own chain by explicit PID, then re-scan for **orphans reparented to init**
    (my dying gate launched one more boot on its way down) and identify those by
    **artifact path** — `/projects/thylacine/build` vs `/projects/thylacine-aux/build`.
    The tree in the command line is the ownership proof when the process tree is
    gone.
15. **Two greps that differ by three characters gave opposite answers about
    whether my own processes were dead** — `[l]oombar.sh` said 0, `[l]oombar`
    said 3, because the second matched the shell running my own grep. Self-match
    is not a curiosity; it is the default when you search a list your own
    command is in (the same trap as `capture-pane`, §1b).
16. **`ps` DOES NOT DISTINGUISH TREES — only cwd or an absolute artifact path
    does.** Both tracks invoke gates by *relative* path (`tools/test-interactive.sh`),
    so a `[t]est-interactive` grep run from either tree matches the other's
    processes identically. This is the same hazard as the pattern-kill, one
    layer up: it corrupts *accounting* rather than destroying work, so it fails
    quietly — I nearly reported "nothing running" while three of a peer's
    processes matched my own check. Discriminate with `lsof -a -p <pid> -d cwd`,
    or by an absolute path in the args. **Three agents reached this rule by
    three different routes in one day** — main by ancestry-tracing a kill, aux
    by `lsof` when killing a futile retry run, vault by identifying a peer's
    QEMU from its `-kernel` path. A rule that three independent parties derive
    the same day is one the environment is actively teaching.

### The payoff was measured, not assumed

Standing down was not merely polite. aux's LS-CI on the freed host: **33/35,
0 FAIL, 0 retries burned**, against their earlier contended measurement of 5
scenarios in 76 minutes with 4 retries burned. So the serialization protocol has
a number attached now, from both directions, and the cost of the courtesy was
~10 minutes of discarded gate.

That matters beyond etiquette: **a burned retry is not just slower, it is
evidence-destroying.** A gate that passes on attempt 2 cannot distinguish "the
host was busy" from "this is intermittently broken", which is precisely the
ambiguity the no-host-load rule exists to prevent. Serializing buys clean
evidence, not just wall clock.

---

## 4. The one-line summary

The machinery works. Every failure so far has had the same shape — **a message
was delivered and nothing acted on it** — and every fix has been to add the
actor, not the channel.
