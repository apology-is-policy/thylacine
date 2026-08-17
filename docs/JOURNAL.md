# The autonomous-run journal

**What this is for.** After a long autonomous run the operator needs to
reconstruct what happened without stitching together `git log`, six phase-status
rows, and a memory directory. This is that single thread: what landed, in order,
why, what it cost, and what it left open.

**What it is NOT.** Not a changelog — `git log` already has the commits, and
duplicating them here would rot. Not a status doc — `docs/phaseN-status.md` owns
per-chunk rows. What lives here is the *narrative*: the reasoning, the wrong
turns, the findings that were not in anyone's plan, and the decisions that
needed the operator.

**Conventions.**

- Newest run first. Within a run, chronological.
- Every claim carries its evidence: a hash, a measured number, a file:line.
- **A wrong turn is worth more than a win** — record the ones that were caught
  and how, because those are the reusable part.
- **Say what is still open, and be exact about what "fixed" covers.** A half a
  defect closed is written as a half.

---

## 2026-08-16 — Warp-C C-1, the per-slot decision, and one third of the extinction tear

Resumed from a self-compaction at the 600k checkpoint. **The nudge fix worked
on its first live test** — the detached watcher fired behind `/compact` and the
far side woke itself, which is the loop the operator had been closing by hand at
every boundary.

### Warp-C C-1 — the composed present, modelled (`ee581fbd`, fixup `ae9a25df`)

GPU-DESIGN §4.5.6 is binding here: `tapestry_present.tla` is model-first, so the
model is extended *before* the impl. Added the GPU-composed present behind
`ALLOW_COMPOSE` — `Attach`/`Detach` (P1b's authority-conferral point),
`ComposeBlit`/`ComposeComplete`, `DrainedOfBlits` on `ServerRelease` + `Free`,
and two invariants repeating T-1's own LIFETIME/CONTENT split: `NoTornCompose`
and `NoStaleCompose`. Eleven cfgs, gated by the new `specs/check-tapestry.sh`.

**The control was set before the work, which is the only reason it meant
anything.** I recorded every cfg's distinct-state count *before* touching the
module, so "this extension is additive" became checkable: with `ALLOW_COMPOSE =
FALSE` the six pre-existing cfgs must reproduce 5413 exactly. They do — and the
check earned its keep, catching that tracking `filled` unconditionally cost the
direct path 5413 → 10413 states.

**Two measurement traps, both mine, both caught by controls rather than by
reasoning:**

- My first comparison harness reported all six cfgs as DIFFERING. The harness
  was broken (`set --` inside the loop clobbered the positionals, lagging every
  expectation by one row). But under the bad labels the raw numbers still said
  something real, and chasing *that* was the right move.
- The buggy cfgs genuinely did differ — and it turned out **the metric was of
  the instrument**. A buggy cfg halts at the first violation, so with parallel
  workers "states explored before tripping" is scheduler noise: measured
  129/141/155 across three *identical* runs. Buggy cfgs are now judged on exit
  status plus the *name* of the invariant reported. (Never on TLC's prose — it
  writes both "is violated" and "was violated" depending on property kind.)

**Then TLC refuted my model, and the tree refuted the premise under it.** I had
carried the in-flight blit as the *slot* it reads, reasoning that a client
filling a *different* slot during a composition is legitimate pipelining — and I
wrote that justification into the module header as though it were established.
It is false. `usr/tapestryd/src/gpu.rs:1515-1518`: tapestryd allocates one 2D
resource per surface, attaches the whole weave as backing, and transfers at a
per-present *offset* that selects the slot. Guest-side slots buy **no** host-side
concurrency. The guard also had the shape of a known trap — `intransfer = 0` is
a gauge reading zero, equally true of "the fill landed" and "no fill was ever
issued" — now closed by an explicit `filled`.

The exclusion is symmetric, so it gets a sabotage *per direction*
(`buggy_blit_during_fill`, `buggy_fill_during_blit`) rather than one flag opening
both gates, which would only ever demonstrate whichever end TLC reached first.

Non-vacuity was measured, not assumed: coverage shows the composed actions fire
`0:0` with the switch off and `ComposeBlit` 2264 / `ComposeComplete` 7328 with it
on, so the green sits over a constructed state.

**Verification:** 32 spec modules green + the 11-cfg tapestry gate. `corvus` and
`handles` deliberately not re-run — 87 minutes, and nothing `EXTENDS`
`tapestry_present`, so they cannot be reached by this change. Zero build inputs
changed (proved by `git diff --name-only`), so the full bar's other legs carry
from `ca50a164` by construction rather than by assertion.

### The design fork it forced — and the operator's vote (`14f8c1ed`)

C-1 surfaced an obligation **the prose did not have**: the D1 recycle gate does
not survive the composed path unchanged. In the direct path a present's terminal
CQE genuinely means "the host has finished reading" — until the compositor
becomes a second, async reader of that one host resource, at which point the CQE
stops meaning the resource is free and nothing in the old rule notices.

Researched before posing it (Wayland `wl_buffer.release` + `drm_syncobj`, Android
BufferQueue acquire/release fences, Fuchsia buffer collections), which showed the
SOTA answer is *two* mechanisms, not one: buffer-release semantics for software
clients, explicit fences for GPU ones. Posed the fork with that attached.

**Operator chose one host resource per slot (3×).** Landed as a scripture commit
with no code, per the design-conversation pattern: GPU-DESIGN §4.5.8, with the
two rejected alternatives and their reasons, and the cost stated rather than
buried (3× host VRAM; ~100 MB at 4K, against a 64-MiB weave cap that already
cannot hold a triple-buffered 4K weave). The landed model does not change with
the vote — `NoStaleCompose` is whole-generation, correct today and merely
conservative once slots become distinct host objects.

### The extinction tear — one third of it (`44a8d53f`)

A surfaced soundness defect outranks the perf arc, so I stopped C-2 and took
this. The `EXTINCTION:` ABI line is emitted as four separate unlocked
`uart_puts` calls; every consumer anchors its match (`^EXTINCTION:` in
`tools/test-fault.sh`, and bare-token matchers elsewhere). A torn banner is
therefore not cosmetic — it is **a real extinction the harness cannot see**,
fail-open on the one channel the whole test discipline trusts.

**The vault already carried an adjacent seam, and I nearly conflated them.**
There are **three** tearing sources with confusingly close names:

1. extinction vs extinction — the re-entrancy guard is per-CPU *by design*, so
   two dying CPUs both print. **Fixed** (`extinction_claim_console`).
2. extinction vs a peer's *normal* console write — the vault's
   `seam-extinction-line-unserialized`. **Open.**
3. `IPI_HALT` — would subsume both. **Open**, a commented-out reservation.

The fix is one `__atomic_exchange_n`: a raw atomic rather than a kernel spinlock
(this runs on a dying machine, often inside a fault handler, and a primitive
carrying lock-order assertions could itself fault), try-once rather than spin
(the winner never releases, since every path ends in `_torpor`), and losers park
emitting nothing — because the failure modes are asymmetric: a torn line can be
read as a clean boot, a missing one leaves the guest visibly hung. Take the loud
failure.

**The fix introduces its own fail-open, and that is what most of the design
guards.** Nothing releases the console, so anything claiming it spuriously
silences every later extinction in the boot — the same defect from the other
side. Hence the deliberate interface split: the claim core is exported to be run
on a *caller-supplied* word, and nothing exports a way to claim the live one. A
test that took the real console would disable extinction reporting for every
test after it, silently.

**Both new tests were sabotage-verified** (1367/1367 → 1365/1367, each failing
on its own distinct assertion message). And the first one is documented for what
it does *not* cover: it is sequential and the property is a race, so a non-atomic
`if (*w) return 0; *w = 1; return 1;` passes it identically. Covering the real
regression needs a multi-CPU fault-injection arm with a **forced** interleaving —
without forcing it the pre-fix build garbles only sometimes, and a discriminator
that fails only sometimes is not a regression test. Tracked, not skipped quietly.

Also corrected a phantom that had propagated into two files: both
`kernel/extinction.c` and the header told readers to co-update
`tools/agent-protocol.md`, which was planned in Phase 1 and never written, and
`tools/run-vm.sh`, which matches neither literal because it only launches QEMU
and never reads boot output. Both now point at the vault's `abi-boot-banner`
mirror set instead of a transcribed list.

**Verification (the full bar, since this is a kernel change):** build clean;
suite 1367/1367 (was 1365; +2); SMP gate 40/40 with 0 corruption across
default-smp4/smp8 + ubsan-smp4/smp8; LS-CI 35/35 PASS; v8.0 floor OK.

**A killed gate is not a green gate.** The first LS-CI run was stopped by the
harness (`Terminated: 15` on its scenario subprocesses) after I ended a turn
while it ran; the SMP gate had survived the identical foreground → background
migration earlier in the same run, so what differed was ending the turn. Re-run
as a tracked background task, staying in-turn.

**And then I got the reasoning for that right conclusion wrong, twice, the same
way.** I first wrote that the killed run "recorded zero verdicts", inferring it
from a stdout log containing only `==> start:` lines. Then, waiting on the
re-run, I read the same channel and concluded it had produced no results after
eight minutes. Both readings were of the wrong channel:
`tools/test-interactive.sh` says so in its own comment — *"The verdict is a
FILE, not a counter"* — and writes results to per-slot `timings.tsv`, never to
stdout. The re-run was healthy the whole time (`go8d PASS` already on disk).

So: **a pattern that matches the wrong thing returns a confident wrong answer,
never an error** — a lesson already pinned in memory, re-learned twice in one
hour on one command. What makes it worth writing down again is that the wrong
instrument produced a *plausible* story both times (a killed gate really had
been killed; a slow gate really can be slow), which is precisely why it was not
self-correcting. The fix is to find where a tool actually writes its verdict
before reading any verdict from it.

### Before C-2 wrote a line: the composed path cannot run on the dev loop

Checked the precondition rather than assuming it, and it changed the arc. The
boot log of the very run I had just gated says
`tapestryd: gpu up -- 1280x800, pci intid=35, virgl=0 capsets=0`, and
`tools/run-vm.sh` defaults to `virtio-gpu-pci` — a device with no GL. So
`CTX_CREATE` / `RESOURCE_CREATE_3D` / `SUBMIT_3D` are unavailable on the primary
dev loop, and with them every mechanism §4.5 describes.

Three consequences, recorded as GPU-DESIGN §4.5.9. C-2/C-3 must be verified on
**thyla-pi**, not here. The composed path must be capability-gated on the
negotiated feature bit — a tapestryd that assumed GL would take the console dark
on the default device. And the third corrects the roadmap: **"C-4 retire the
readback path" cannot mean delete it.** That is forced twice over — by the plain
`virtio-gpu` that is the default here, and more fundamentally by bare metal,
where there is no virtio-gpu at all and virgl is a *virtualization* transport
with nothing to negotiate. The CPU path is the universal one; GPU composition is
the accelerated path where a GPU seam exists.

The cost is stated rather than left to be discovered: tapestryd carries **two
composition paths permanently**, and they must stay behaviourally identical from
the outside or the gate that proves one is silent about the other.

### The C-2 verification host, proven rather than assumed

Having established the dev loop *cannot* run the composed path, the next
question was whether anything can. Synced HEAD to thyla-pi (all 80 pool chunks
hash-verified, artifacts paired) and booted `virtio-gpu-gl-pci` under KVM on
real V3D:

```
tapestryd: gpu virgl -- num_scanouts=1 num_capsets=2
tapestryd: gpu capset[1] id=2 max_version=2 max_size=1384
tapestryd: gpu up -- 1280x800, pci intid=35, virgl=1 capsets=2
CAPSET GATE: VERIFIED
```

So C-2 has a working verification host, and the two figures — `virgl=0` here,
`virgl=1` there — are the whole argument for §4.5.9 in one line each. Worth
doing before the implementation rather than after: had C-2 been written first,
its first symptom on the dev loop would have been a dark console, which is a
long way from its cause.

### C-2a — the capability gate and the compositor context

The first landable piece of C-2: a reserved compositor virgl context
(`COMPOSITOR_CTX = 0x100`, far above the client `slot + 1` range so a client's
stream can never author against the screen), minted only where `virgl`
negotiated, and a startup line reporting which composition path the host can
actually take.

**The first cut reported nothing, and the boot passed anyway.** I had hung the
posture report off `ensure_screen`, beside the other display resources — but
`ensure_screen` runs only under `Scanout::Composed`, a state a normal boot never
enters, so the line sat behind an unconstructed state and printed on neither
host. The suite went 1367/1367 with the feature effectively absent. Which
composition path is *available* is a property of the HOST, fixed at feature
negotiation, so it now reports where the host is brought up.

**Verified on both arms, differing in exactly one variable** — a negative
assertion alone would have been satisfied by a broken fixture:

| Host | Negotiation | Posture |
|---|---|---|
| dev loop, `virtio-gpu-pci` | `virgl=0` | `composed path = CPU (virgl=0)` |
| thyla-pi, `virtio-gpu-gl-pci` | `virgl=1 capsets=2` | `compositor ctx 256 up` → `composed path = GPU` |

Getting the positive arm took one correction of its own: the `capset` verb
filters its output at the capset markers, so the Pi run *looked* like it lacked
the line when it had simply not been shown it — `boot-probe.sh` keeps the full
log on the host, and the line was there. A truncated capture and a missing
feature are the same reading until you check which one you have.

### C-2b — the 3D screen, landed gated and HONESTLY UNPROVEN on its own arm

The screen becomes a host-side 3D resource attached to the compositor context
where GL exists, falling back to the 2D resource everywhere else. Guest backing
stays on both paths, because at C-2b the screen is still CPU-filled — only its
host-side representation changes. `screen_push` grows a 3D arm, and there the
sync transfer moves the whole surface rather than the damage rect: a deliberate
trade, since C-3 deletes the CPU fill outright and building a rect path for a
mechanism already scheduled for removal is waste.

**What is verified, and what is not — stated because the gap is the finding.**
The FALLBACK arm is verified: suite 1367/1367, and LS-CI 35/35 where the
`ls-gfx` scenarios assert exact pixels via screendump and therefore cannot pass
without a working composed screen. **The 3D arm has never executed.**
`alloc_screen` runs only under `Scanout::Composed`, and neither the dev-loop
boot nor the Pi's `capset` boot enters it, so `screen res N 3D (compositor ctx)`
printed on neither host. `prove` produced no new boot log to grep.

So this lands **gated off on every host I could exercise** — dead on the dev
loop by capability, unproven on the Pi by opportunity — and the commit says so
rather than calling a clean boot a verification. Booting green proves the gate
did not fire, which is exactly what an `if (false)` would also prove.

**Then I found why, and it is a tooling gap rather than a code problem.** The
Pi logs say `tapestryd: scanout direct 0 (1280x800)`: every existing Pi verb
drives a SINGLE display-sized GL client, and that takes the **Direct** path —
scanning out the client's own resource and bypassing the compositor screen
entirely. §4.5.1 spells out the condition: Direct demands one visible surface
AND one visible leaf AND an exactly display-sized surface. So composed scanout
needs two surfaces, or one smaller than the display, and **no verb in
`warp-host.sh` produces either.** `capset` and `smoke` both land in Direct;
`tri` and `prove` left no new boot log at all.

That is worth more than a failed check: it says the composed path — the entire
subject of the Warp-C arc — has no driver on the only host that can run its GPU
half. Building one (two surfaces, or a mode change that un-sizes a single one,
which is what `ls-gfx-mode` does locally) is the next task, and it gates C-2b,
C-3, and the arc's exit criterion alike.

### The driver — C-2b's 3D arm finally executes, and my own note was wrong

The task I left myself was "build a Pi driver that forces Composed scanout."
Before building anything I checked the claim under it, and **it was false**. The
section above says "no verb in `warp-host.sh` produces either" — but
`glq-virgl.exp`, which `quake` runs, opens GLQuake in a window and its very
first assertion is `-re {scanout composed \((\d+)x(\d+)\)}` with the label
"composed entry (two leaves)". `decomp` and `wedge` split the layout too. What
was actually true is narrower and duller: the verbs I had *read the boot logs
of* — `capset`, `smoke` — boot with no client at all, so aurora alone is
display-sized and lands in Direct. I generalised from the two logs I had to a
claim about all ten verbs, and wrote it into two documents.

Worth noting how cheap the catch was: one grep for `composed` across
`tools/warp/*.exp`, run because the note asserted a negative over a set I had
not enumerated. **The evidence that a thing is absent has to come from the whole
set, not from the members that happened to be in front of me** — and a note
written confidently at a compaction boundary is exactly where that error
survives, because the far side inherits it as established fact.

I still did not use `quake`. It drags in the pool's `tyr-glquake`, S3TC quirks
(#216), the #198 storm, and 900-second timeouts — a lot of machinery that can
fail for reasons having nothing to do with C-2b. `/bin/tapestry-battery` brings
up two surfaces, lives in the ramfs, and needs no GL of its own, so **the only
GL object in the experiment is the compositor's own screen**. That isolation is
the reason to pick it, not availability.

`tools/warp/composed-screen.exp` boots, takes the posture line between boot and
login (it prints at bringup, which is where a host property belongs — a lesson
this arc already paid for), runs the battery, and asserts the screen mint. **The
control is the device**, which is why the scenario takes one as a parameter
instead of hardcoding the GL model: two legs, one host, one variable, each
asserting the other's outcome is wrong.

```
virtio-gpu-gl-pci -> composed path = GPU -> screen res 67 3D (compositor ctx) (1280x800)
virtio-gpu-pci    -> composed path = CPU -> screen res 67 2D (1280x800)
```

**C-2b's 3D arm has now executed**, on real V3D silicon through virgl. The
second line is what makes the first mean something: a GL-only leg would pass
identically against a tapestryd that ignored the negotiated bit and always
minted 3D. Two legs that *disagree* are stronger evidence than two that both
pass — the control produced a different answer rather than merely staying quiet.
Both legs minting `res 67` is a small corroboration on the side: everything
upstream of the branch is identical, so the arm is the only thing that moved.

The gate keeps two claims separate rather than collapsing them — posture matches
the device, screen arm matches the posture — so a host that had silently lost
its GL could not satisfy the second by making both sides equally wrong. And
`tools/warp-host.sh composed` requires each leg's scenario-completion line as
well as its screen line, because a leg that died immediately after printing its
screen line would otherwise still show the gate everything it greps for. That
term is not hypothetical caution: the `reject` verb in this same file shipped
grepping `C0-REJECT` while its producer printed `C0-DETECT`, and exited 0 on the
exact failure it existed to catch.

### Then C-2d refuted itself before it wrote a line (§4.5.8a, OPEN)

With the driver landed I went to implement §4.5.8 — the per-slot host resources
the operator voted for — and read the present path first. The decision does not
survive it, for a reason nobody had in view at the vote.

Three facts, each one grep:

1. Every client rotates slots on every present: `cur_slot = (cur_slot + 1) %
   nslots`, `libtapestry/src/lib.rs:525`, unconditional, both scanout modes.
2. Nothing copies content from slot *N* to slot *N+1*. `pixels()` hands back
   the raw current slot; there is no carry-forward anywhere.
3. **The single per-generation host resource is therefore doing a job nobody
   wrote down: it is the accumulation buffer.** A damage-only present transfers
   only its rect, so the host resource keeps the rest of the previous frame and
   the stale guest slots never reach the host.

Give each slot its own host resource and that job has no owner. A damage-only
present would render a three-frames-stale background around each fresh rect —
in Direct immediately, and in Composed at C-3. And the client this lands on is
**aurora**: it repaints only rows `r0..r1` and presents that rect
(`aurora/src/main.rs:1027-1038`), and it is the default Direct client on every
boot. The very line I have been reading all session, `scanout direct 0
(1280x800)`, is that client.

What makes this worth recording is not the catch but where the load was.
§4.5.8's analysis compared 3× / 2× / 1× VRAM and serialization — a complete
comparison of the properties anyone had *named*. The single resource's real
function was invisible because nothing declared it; it was an emergent
consequence of "transfer only the damage rect", and it had been load-bearing
for the console for as long as the console has existed. **A design comparison
can be sound over every property you listed and still miss the one the code is
actually relying on.** Only reading the path surfaces those.

I recorded it as **§4.5.8a** with four options rather than picking one, because
the vote is the operator's and this changes the terms they voted on. The
recommendation is buffer age — `EGL_EXT_buffer_age` and Wayland's
`wl_surface.damage_buffer` exist for this exact problem, Android's BufferQueue
exposes the same, and it keeps the per-slot vote intact at no VRAM cost while
retiring the latent hazard instead of routing around it. C-2c and C-3 both wait
on the answer: every option changes what gets attached and what gets blitted.

### The vote, and C-2d-a (`0a0e0fbb`, `931bf15a`)

The operator picked buffer age. Implementing it immediately hit a constraint the
option sketch had assumed away: I had written "present CQE now carries: age",
and it cannot. A present is a 9P write over the Loom ring, so its CQE is
**kernel-owned** — `result` is the write's byte count, `flags` is `LOOM_CQE_*`,
and `struct loom_cqe` is `_Static_assert`-pinned at 16 bytes. Putting a
compositor payload there is a kernel ABI break for a compositor convenience.

The way out was to notice who already owns the information. `libtapestry` owns
the rotation — `cur_slot` advances only after a present's own CQE — so it knows
exactly when each slot was last presented and can derive the age itself. A
`TEV_AGE` event was rejected (async to the present, so it races the rotation) and
a control word in the weave was rejected (a client-visible layout change for
something the client can compute).

**The interesting part is what the derivation costs, because it is the same
trap again.** A derived age is correct only if the client hears about every
server-side invalidation — which is exactly the kind of undeclared dependency
that produced §4.5.8a two hours earlier. So it is written down as a named
invariant this time rather than left to be rediscovered: tapestryd must not skip
a transfer without the client subsequently getting a redraw request, and a
redraw invalidates **every** slot, so the client repaints full for `nslots`
presents, not one. Both arms are wired in `libtapestry`.

Then aurora handed back independent corroboration of §4.5.8a. `main.rs:988`
already routes any OSD pass through the full-frame branch, with the comment
*"a partial rect could transfer stale panel pixels from an older slot"*. The
symptom had been understood locally, for one widget, and worked around — the
general statement just never got made. That is what an emergent load-bearing
property looks like from the inside: not unknown, merely un-generalized.

I split the chunk, because the halves are not symmetric: per-slot resources
without age break every accumulator, but age without per-slot resources is inert
and harmless. So the client half went first — and **its honest gate is that
nothing changed.** `ls-gfx` PASS, `ls-gfx-panes` PASS (exact pane-centre
pixels), suite 1367/1367. Its actual effect is unobservable until C-2d-b removes
the accumulator, and the commit says so rather than dressing a green boot up as
verification.

**Then I got the prerequisite list wrong, in the commit message, within twenty
minutes of writing the lesson that prevents it.** I swept for clients that
present partial damage with `grep 'present(Some\|present_rects'` and reported
three. That greps **API shape**, not the property that matters — *damage
smaller than the full surface*. Checked properly, it is one:

- `tapestry-battery` needs **nothing**. Every present is `present(None)`, and
  its one `present_rects` tiles the whole surface with two rects after writing
  every pixel. Its own header says so: *"presents FULL-FRAME only."* I had
  called it "the one with teeth."
- `tapestry-demo` is the real one, and is the sharpest example in the tree: it
  paints the quadrant background **into slot 0 only**, at frame 0, then draws
  just the plasma box into *rotating* slots forever after. Slots 1 and 2 never
  receive the background at all — they hold alloc-time zeros. Under per-slot
  resources, two frames in three would show black around the plasma.

"A pattern that matches the wrong thing returns a confident wrong answer, never
an error" is pinned at the top of my own memory index. It still went into a
commit message, a scripture section and the handoff, because a grep that
*returns results* feels like a sweep that *finished*. Corrected in §4.5.8b and
the handoff; the commit body stands as written, with this as its correction.

### The stop hook guarded the wrong stop, and the guard was never needed (`b3632942`, `cd0b3390`, `b61ca929`)

The operator noticed the Stop hook fired once in the long run and then went
quiet at a second stop it should have caught, and asked aux and me to work out
why. It is the third instance this week of the same family, and the sharpest.

**The measurement.** Replaying the hook's own parser over the real 805 MB
transcript: the silent stop sat at **530k / 73 turns** — inside the window on
both axes. So "it was correctly silent above the checkpoint" is dead. Isolating
the logic with synthetic input showed it behaves exactly as written. The cause
was upstream, and the pattern repeats: every firing is followed by silence for
the rest of the continuation, re-arming only when the user speaks or a
compaction lands.

**What I got wrong, and it was not the code.** `stop_hook_active` means "this
hook already triggered a continuation" — per-continuation by definition. I
exited early on it, which made the hook a once-per-*run* nudge guarding the
first stop and nothing after, i.e. the stop most likely to be earned and none
of the ones that follow. I kept that early exit because I believed it was the
loop guard.

**It never was.** aux fetched the contract: the harness overrides a Stop hook
after **eight consecutive blocks** (`CLAUDE_CODE_STOP_HOOK_BLOCK_CAP`). The
belay already existed one level up. So I had built a guard against a loop
something else was already preventing, and paid for it with the exact behaviour
the hook exists to provide. That is a different failure from a bug: **the code
did what I meant; what I meant rested on a contract I had not read.** No amount
of testing my own intent would have found it — only reading someone else's.

**The instrument came before the fix, and earned it twice.** The hook had nine
silent exits, so "correctly silent", "suppressed", and "crashed" were one
observation and any diagnosis could only be a guess — the same shape that had
just cost the vault a stranded day. So a ledger row on every path landed first.
Then it caught two things I would not have:

- Its own blind spot: the `stop_hook_active` parser printed `"1"` on exception,
  so a malformed stdin logged as `silent-stop-hook-active`. **The instrument
  built to separate those two causes could not separate those two causes.** The
  malformed-stdin test leg printed the wrong row, which is the only reason I
  looked.
- On its first *real* output: three rows in 24 seconds with incoherent context
  jumps, because the ledger is shared by main/aux/vault and I had dropped the
  session field from aux's spec. An interleaved log with no writer is worse
  than no log — it invites a confident reconstruction of one impossible session
  out of three real ones.

**And the fix validated itself in production before I finished writing it up:**
the reworded stem ("fires once per stop") came back in a live firing that
re-armed mid-continuation after real work — something the old version could not
do — with the ledger row `588458ctx/44t/27b/flag1` showing exactly why.

### C-2d-b landed, and the sabotage that proved it unverified (`f86177b6`)

The server half went in as voted: each generation mints `WEAVE_SLOTS` host
resources instead of one, backed per-slot instead of whole-weave. The
consequences were all followed rather than found later — `res_stale` becomes
per-slot; Direct binds the presented slot's resource and therefore rebinds every
frame (a KMS page flip, carrying the #57 post-bind flush); transfer offsets lose
their slot base, which the compiler confirmed by reporting `slot_stride` newly
unused; retire and `release_gen` unref all three or leak two per surface in the
process that IS the console.

`Held::Direct(Rect)` was the one that needed design rather than editing, and it
is why I stopped the first attempt at it. A rect union is well-defined only
while every held present lands on one resource; presents rotate slots, so two
held presents sit on different resources and `release` must flush each against
its own. Now `[Rect; WEAVE_SLOTS]` — bounded by construction, since a client
cannot hold more presents than it has slots.

**Then the sabotage passed, and that is the result worth the whole chunk.** I
disabled aurora's age handling with per-slot resources live — `stale_slot =
false`, `back = 0`, exactly the pre-C-2d-a client against a non-accumulating
server — and **`ls-gfx` still reported PASS.**

So the two gates I had been treating as verification are not. `ls-gfx` asserts
the frame *looks like* a console and that dumps *differ* after a command;
neither notices a stale background around fresh rows. `ls-gfx-panes` drives the
battery, which presents full-frame only and never exercises the accumulator path
at all. Between them they cover everything about the compositor **except the
property C-2d changes.**

That is the same trap as C-2b at the start of this run — a green result that
proves the gate did not fire — except this time I was the one about to be
fooled by it, having written the C-2b version into scripture that morning. The
difference between the two is not insight, it is that I ran the sabotage. Had I
not, this would have landed as "green on both pixel gates", which is *true* and
means nothing.

C-2d is therefore **implemented, not verified**, and the commit says so. §4.5.8c
records what the missing gate has to do: paint a region, damage a *different*
region, rotate all slots, sample the first region. `ls-gfx-panes` already has
the sampling machinery, so it is a scenario to write, not an instrument. The
focused audit is owed too — `usr/tapestryd` is an I-40 trigger surface and this
is the live scanout path — and could not run here because agent spawning is off.

### The self-compaction slot had two keys that did not agree (`7061115a`)

aux found this by reading the ledger nobody reads, and it is the best kind of
find: the mechanism had been quietly half-broken since it was built, and the
evidence had been sitting in a file the whole time.

`~/.claude/thyla-selfcompact/log.tsv` has vault's `allow` at 2026-08-16
10:44:32Z with **no `consumed` and no `nudge`**, and its `.note.pending` still
in the slot dir a day later. Every `main` row is paired; only vault's is
orphaned. That session compacted itself and was never handed its own resume
note — it sat at a prompt for the rest of the day.

The cause is a key mismatch, and **the comment is the interesting part.**
`tools/thyla-selfcompact.sh` said, in as many words: *"Two independent
derivations of one key, no shared config to drift."* The producer keys on `git
rev-parse --show-toplevel`; the consumer on `basename(dirname(transcript))`,
which is where the session was **launched**. Those coincide for main and aux
and do not for vault, which is launched from the thylacine tree and works in
thylacine-vault. So the comment **named the hazard and then asserted it away**,
and that assertion is what kept it unexamined for the mechanism's whole life.
It is every "keep these in sync" note that has ever rotted, except this one had
the confidence of sounding like an argument.

The fix needed no new identity, because one was already there and unused: the
arming script has always stamped `pane=$TMUX_PANE` into the meta, and a hook is
a child of the same claude, so it reads the same value. Pane match first, path
key as fallback.

**But the half that mattered was the silence.** The old failure was not doing
the wrong thing — it did *nothing*, and left no evidence, so `allow` without
`consumed` was the only trace. There is now an `orphan-note` row whenever a
pending slot goes unmatched, plus a 30-minute staleness discard.

**Then the test caught a bug in the fix that was worse than the bug.** The
first age check used `time.mktime` on a UTC stamp — `mktime` reads a
`struct_time` as *local* — so a note stamped that same second measured as an
hour old and was **discarded**. In any non-UTC zone that breaks every
legitimate resume: the repair would have converted a vault-only silent miss
into a universal one. I saw it only because leg 1 of the test printed
`stale-discarded` on a note written a moment earlier. Four legs, with legs 3–4
as the controls that make leg 1 mean anything — same note, same path-key
mismatch, only the pane varies:

```
1 pane matches, fresh    -> INJECTED,     consumed
2 pane matches, 25h old  -> not injected, stale-discarded
3 CONTROL no TMUX_PANE   -> not injected, orphan-note
4 CONTROL wrong pane     -> not injected, orphan-note
```

aux also retracted something in the same message, which is worth recording
because the retraction is worth more than the claim was: the "fourth
unregistered session" cited in the yip lease rationale **was aux itself** —
`ps -o ppid` on its own tool shell resolved to the process it had been reading
as a stranger. A census needs a control, and the control was its own identity.
Same family as `ps` matching its own command line, from the other end.

### Found in passing: `docs/REFERENCE.md`'s snapshot block died in Phase 5

The doc-update step sent me to `docs/REFERENCE.md` to refresh its Snapshot
block, which `CLAUDE.md` calls non-negotiable per chunk. **The newest "Tip"
bullet in it is a Phase 5 chunk** (`P5-stratumd-stub-bringup` audit close), and
there are 101 bullets behind it. The file's last commit of any kind is
`418688cf`, 2026-08-01. It contains **zero** occurrences of "Warp", "Tapestry",
"Clade" or "PTY-" — three whole arcs and a subsystem that do not exist as far as
the as-built technical reference is concerned.

So a binding per-PR obligation has been quietly unmet across roughly two phases,
including by me, several times this week. It is the "*a status field whose flip
is nobody's step stays unflipped*" shape: every chunk's author is told to
refresh it, no chunk's work makes them, and nothing fails when they do not.

**I deliberately did NOT patch my own bullet onto the top.** A dead list with
one fresh entry reads as maintained, which is worse than one that visibly
stopped — the reader trusts it again. The real question is what that block is
*for* now that `docs/phaseN-status.md` carries per-chunk rows and this journal
carries the narrative; answering it is a scripture-shaped decision, not a doc
edit to slip into a tooling commit. Enqueued rather than fixed in passing, and
enqueued in memory because the tracker is down this session.

### The gate that sees C-2d, red under both sabotages — and the defect building it found (after the self-compaction at `a733402e`)

Resumed from my own note with one instruction: build the §4.5.8c gate on aurora
in Direct, and validate it by re-running the sabotage that had passed `ls-gfx`
and requiring red. That is what happened, with two things the note did not
anticipate.

**The gate** (`tools/interactive/ls-gfx-age.exp` + `gfx_region.py`). Fill three
times with `yes … | head -n 200` so every slot carries glyphs; a POSITIVE
control — the same region assert, four keystroke-rotated dumps, each must show
text (a negative with no positive twin is satisfied by a broken fixture); then
`clear`, which blanks every cell in one all-rows present into ONE slot; then
eight rounds of keystrokes + dump, region exactly Bonfire, every pixel read.
The region is in cells (rows 6..rows-3, cols 2..cols/2) off aurora's own
`console up` line, so a font change moves it rather than breaks it.

**What the note left to the author, and how it was decided.** The detector is
slot-phased: the screen shows the slot presented LAST, so one dump samples one
slot. I had written "probabilistic — require N consecutive dumps". Working it
through, the honest model is *driven*, not sampled: each keystroke is a
row-0-only redraw, i.e. one present into the next slot, so the rounds advance
the phase deterministically plus whatever blink presents fall in the round.
That reframing exposed the real trap: **a broken client can have ONE stale
slot, not two** — an off-by-one in the union (`back = age-2`) leaves exactly one
— and the 1,2,3,1,2,3,… key pattern I first sketched (meant to break any
phase-lock with the blink) visits residues 1,0,0,1,0,0,1,0 under `b=0`: it never
reaches residue 2 and would pass an off-by-one every time. A plain one key per
round does reach it (1,2,0,1,2,0…) but is the pattern a 60 Hz blink can
phase-lock. So the negative leg types 1,1,2,1,1,2,1,1 keys, which visits all
three residues for *any* constant blink count per round (checked for b=0,1,2 in
the header); the
independence bounds — 3^-8 for the no-age class, (2/3)^8 = 3.9% for the
one-stale-slot class — are the fallback if the blink rate varies mid-leg, and
the header says which claim is load-bearing.

**Measured** (HVF, 128×36 cells, region 368 280 px). Fixed build: positive
63 882/368 280 non-bg on 4/4 dumps (identical counts — every slot holds the
same fill, as a correct client guarantees), negative **0/368 280 on 8/8**,
43 s. **S1** — the §4.5.8c sabotage, `stale_slot = false` + `back = 0`: **red
3/3 attempts**, at rounds 2, 1, 2 (63 882 stale px, i.e. the pre-clear fill
verbatim). **S2** — `back` off by one: **red 3/3**, at rounds 2, 5, 2. The
five-round attempt is the 1,1,2 pattern paying for itself: four dumps landed on
the two good slots before the fifth reached the one stale one. Restore green.
Both sabotages applied and reverted with `Edit`, and `grep SABOTAGE` empty
before the restore build.

**The defect the gate found — in C-2d-a, not C-2d-b.** Reading aurora's damage
branch to predict the sabotage outcomes, I traced what `931bf15a` records into
`dmg_hist`: **the WIDENED range** ("this is what actually reached the slot, and
the next union reads it"). That reasoning conflates *repaint* with *damage*.
The union answers "what changed since slot X was last presented"; what changed
between two presents is the dirty span, and the widening only says how much of
it THIS slot had to catch up on. Recording the widened range makes any
full-rows entry — every scroll — re-enter every later union, so every present
after it repaints all rows, forever. Aurora has been repainting the whole grid
on every cursor blink since C-2d-a landed: correct pixels, dead damage path.
Fixed to record the dirty span (`dirty0, dirty1` captured before the widening);
a full entry now falls out of the window after `nslots` presents. Two things
follow that are worth having in writing: S2 is a sabotage only against the
*fixed* recording — under the widened one an off-by-one is masked, since any
`back ≥ 1` propagates the full-rows entry (the old code had slack precisely
because it had no damage path); and the tight recording is guarded by the gate
that was built in the same chunk, which is the right order.

**Wrong turns, caught:** the first run failed on my own Tcl (`gfx_dump` takes
two args and I passed one) — three attempts, ~30 s each, all on the harness
side, before a pixel was read. And the resume note's "the sampling machinery is
in `ls-gfx-panes`" was true and unhelpful: `ppm-sample.py` reads one pixel; the
gate needs a region census with a positive control, which is a 40-line tool.

**Owed, unchanged:** the focused audit on `usr/tapestryd` (I-40; agent spawning
still off). The vault-owned prose (`sub-aurora`, `sub-libtapestry`,
`sub-tapestryd`) for C-2d and the recording fix goes over yip; the local
reference carries the gate.

### The device's OK was never the renderer's verdict — C-2b's "3D" word re-earned

Found while designing C-2c's gate, and by the one move that keeps saving this
arc: reading the source of the thing making the claim before repeating the
claim. My C-2c draft was about to say, for the third time in a week, that a
`CTX_ATTACH_RESOURCE` answered OK "attests the host accepted it". Before
writing that I fetched QEMU v10.0.0 `hw/display/virtio-gpu-virgl.c` (thyla-pi
runs 10.0.11) and read the handlers. **They ignore the `virgl_renderer_*` return
value** — for `CTX_CREATE`, `RESOURCE_CREATE_2D/3D`, `CTX_ATTACH/DETACH`,
`TRANSFER_TO_HOST_3D`, `SUBMIT_3D`, `CTX_DESTROY`; `ATTACH_BACKING` checks it
only to clean up the iov. `RESP_OK_NODATA` means "QEMU parsed it": nonzero,
non-duplicate id, valid iov. Only `SET_SCANOUT` (`resource_get_info_ext`) and
`RESOURCE_UNREF` (QEMU-side existence) consult anything.

**So three of my own documents were false in the same sentence.** C-2b's gate
header, `149-warp.md` and (by reference) the status row said the screen's "3D"
word was "the conjunction of four response-checked round trips the host
answered OK — a claim about the host accepting the object". Those four are
exactly the ignored ones. And it was not only prose: `alloc_screen`'s "a 3D
failure is NOT fatal — it falls back to 2D" was dead for a renderer-side
refusal — `is3d` reduced to `comp_ctx`, "3D" printed, and the failure landed
later, silently, as `INVALID_RESOURCE_ID` at the composed `SET_SCANOUT`, whose
result the code dropped after printing "scanout composed" *before* the bind.
The display would have kept the previous scanout, and the C-2b gate would have
said VERIFIED. #240 had measured this exact shape for `SUBMIT_3D` four days
earlier; the finding was filed against one command and never checked against
its family — the same lesson as the C-2d gate pattern that morning, one level
up.

**The repair is #240's own technique**: make the producer prove it with pixels.
`alloc_screen` writes 16 sentinel pixels into the fresh screen's backing,
`TRANSFER_TO_HOST_3D`s them through the compositor context, clobbers the
backing, `TRANSFER_FROM_HOST_3D`s back, compares, restores the zeros. Only a
resource the renderer holds, has attached to `COMPOSITOR_CTX`, and moves pixels
through can pass; a refused create or attach makes both transfers renderer-side
no-ops and the clobber survives. A refusal now falls back to 2D for real, the
screen line says why, the composed line prints after the bind with its verdict,
and `composed-screen.exp` grew a fifth term (the bound resource IS the minted
screen; the verb requires it on both legs).

**Measured on thyla-pi** (KVM, real V3D, boot-ms ~212 000), one variable —
the format the renderer will accept — two runs. *Sabotage*, `VIRGL_FORMAT`
`0x7FFF` in the 3D create: GL leg `screen res 71 2D (1280x800) -- 3D refused:
renderer round trip`, then `scanout composed (1280x800) res 71 bound` — so
`CREATE_3D`, `CTX_ATTACH_RESOURCE` and `ATTACH_BACKING` all came back OK from
the device under a format the renderer cannot accept (the reason would have
named the step otherwise), the renderer refused, the fallback was real and the
display got a working screen; the scenario went RED on the arm and the verb
reported three GATE FAIL terms; the non-GL leg was unaffected. *Clean*: GL leg
**`screen res 71 3D (compositor ctx) (1280x800)`** + `res 71 bound`, non-GL
`2D` + `res 71 bound`, all five terms, rc 0. The half that says the OLD code
would have printed 3D under the sabotage is inferred from the measured OKs and
the old boolean (`comp_ctx && create.is_ok() && attach.is_ok()`), not itself
measured — I chose not to spend a third Pi cycle on a one-line inference and
say so here.

**What this changes downstream**: `CTX_ATTACH_RESOURCE`'s response witnesses
nothing, so C-2c cannot be verified by its attach at all — its gate is P1b's two
arms in-guest (attach + one blit + readback; no-attach control red), which means
C-2c lands WITH the first blit witness. The C-2c design draft
(compositor-side import on host, bounded by hosting, no client verb — every
compositor in the prior art does it that way) is written and waits on that
correction; it goes into GPU-DESIGN as §4.5.10 with the next chunk.

### C-2c — the compositor imports what it composes, and the import is witnessed (after the self-compaction at `8c20b1f8`)

Resumed from the second self-compaction of the run (`8c20b1f8`, all pushed;
the note said "next is C-2c WITH its blit witness", and that is what this is).

**What C-2c is, in one line:** at `alloc_weave` tapestryd now
`CTX_ATTACH_RESOURCE`s every slot resource of a generation into
`COMPOSITOR_CTX`, and at `present-to` it imports the GL adoption's consented
BO — the client handing its buffer to the compositor is the whole grant, no
client verb (§4.5.10) — and every import is revoked BEFORE the resource's
unref on every death path (`release_gen`, `retire`, `wbo_retire`, `present-to
off`/replace, the consented surface's retire).

**The witness, and why it is not the one the design paragraph drew.**
§4.5.4c had already established that `CTX_ATTACH_RESOURCE`'s OK attests
nothing, so C-2c had to land with a pixel witness. The design said "blit a box
of the slot into the screen and read the screen back". Built instead: the
compositor context's own #240 mark/sentinel pair (`warp_probe_build
(COMPOSITOR_CTX)`, minted with the ctx), and per slot: seed tokens into the
slot's host copy through the present path's own `TRANSFER_TO_HOST_2D` (the
guest pixels are borrowed while NO client mapping of the weave exists yet —
`alloc_weave` runs before the Tweft that maps it is answered — then zeroed),
poison the sentinel, `RESOURCE_COPY_REGION` slot → sentinel inside
`COMPOSITOR_CTX`, read the sentinel back. A 1×1 compositor-owned target
instead of the screen: same claim (pixels through the compositor context or
nothing), the direction C-3 will use (the slot as SOURCE), no screen pixels
to save/restore, no question about the screen's coordinates — and it made
import time the natural site, since the reason the design gave for composed
entry ("the screen may not exist yet at import") no longer applied.

**A health copy runs before every witness, and the reason is the latch.** A
copy naming a resource the renderer does not hold in the context reports
`ILLEGAL_RESOURCE`, and vrend then refuses every later command buffer on that
context (§4.5.4a). So a genuinely refused import kills GPU composition for the
process lifetime, silently — which is (a) why `comp_attached` fails closed and
C-3 must never blit from a resource without it, (b) why the mark → sentinel
health copy runs first, so a REFUSED is attributable to THAT import and later
generations read `SKIPPED (compositor ctx unhealthy)` as a measured state, and
(c) why the witness runs at a rare structural moment (~16 controlq round trips
per generation) and never per frame.

**What the Pi taught before it answered the question it was asked** (six
`composed` cycles; the sixth is the one that counts). (1) The clean build read
`REFUSED (slot 0 copy did not land)` on its first run — the witness's own
seed was at guest row 0 and the compositor's copy of a y=0 box on a `Y_0_TOP`
source lands from texel row **h−1** (vrend's FBO copy path measures such boxes
from the bottom; the texel-exact copy-image path was not the one taken). The
instrument needed a control of its own: it now seeds rows 0 and h−1 with
distinct tokens and REPORTS which came back — `witnessed 3/3 (copy read texel
row 799)` — a measured convention C-3's blit boxes inherit rather than a
guess. (2) The posture anchor came out `ttaappeessttrryydd`: the kernel's
`proc: orphan` burst at warden's exit and tapestryd's SYS_PUTS interleaved
BYTE for BYTE — the console TX ring is byte-atomic, not line-atomic, and my
probe mint had moved the anchor into the burst. Not fixed here (LS-8 surface,
aux mid-change in `cons.c`, and it costs the kernel-byte-unchanged property);
the anchor is printed first again, the armed state moved to its own line, the
defect enqueued (`bug_console_tx_ring_byte_atomic.md`) and handed to aux on
yip. (3) The gate script then cost three cycles of its own: a say-line format
change under an anchored regexp; three `-re` arms — pattern ORDER beats buffer
position, so the arm listed first ate a later comp-attach line and discarded
the screen/composed pair before it; and one ordered pattern that matched
PARTIAL lines (serial arrives in chunks) — three GL-leg hangs ending on the
battery's own later FAIL, while an offline replay of the same log passed. The
anchored single-pattern form went green: `WARP-COMPOSED ATTACH: witnessed 2
surfaces (copy read texel rows: 799 797)`, both legs PASS, verb VERIFIED on
seven terms.

**The sabotage measured more than it was asked to.** Skipping the slot
attaches: the first import `REFUSED (slot 0 copy did not land)`, then every
later import `SKIPPED (compositor ctx unhealthy)` — the latch is now a
measurement, not a recollection of vrend — **and the screen's own 3D mint fell
back**: `screen res 73 2D (1280x800) -- 3D refused: renderer round trip`. The
§4.5.4c fallback, built two chunks ago against a hypothetical, ran for real:
the display kept working on the CPU/2D arm while GPU composition was loudly
gone. Verb RED, 2D leg unaffected.

**The quake gate found a C-2d-b leftover.** `glq-virgl.exp`'s eviction leg
waits for `scanout direct N (WxH)`; C-2d-b (`f86177b6`) changed that say line
to `scanout direct N slot S (WxH)` and the check made then enumerated the
`scanout composed` consumers and missed the `scanout direct` ones — five
patterns across `glq-virgl` / `glq-decomp` / `glq-wedge-probe`, all silently
broken since, all failing CLOSED (a false RED on the console-restore leg after
^C, the first time any of them ran after that commit). Fixed to take the
`slot S` token as optional. #230's lesson again: a mirror set is enumerated by
what its members MEAN, not by the substring one happened to grep.

**Gates.** `composed-screen.exp` grew a third claim (GL leg: ≥ 2 per-surface
`witnessed n/n` lines — the battery's two surfaces — none refused; 2D leg: the
import declared skipped, no per-surface line — the control), the `composed`
verb terms six/seven, and `glq-virgl.exp` gates the ctl census (`comp-attach
witnessed W refused R`: R must be 0) after the game dies — the BO import
through the SDL shim's real `present-to`.

**Coordination.** Aux held the mac all afternoon (its pty-4 root-cause fix:
builds + suite + LS-CI + the SMP halves); the C-2c cargo check/build ran at
`-j2` under an explicit yes on yip 0024, everything else waited for the
release; the Pi lease was mine (`hold pi`) for the whole verification.

### Still open leaving this run

- **Warp-C C-3** — blit composition + chrome-as-texture, growing from the C-2c
  witness (`comp_copy_px` is the encoding; the copy box on a `Y_0_TOP` source
  names texel row h−1−y on this host — measured; the 3D screen is minted with
  flags 0, so decide there whether it should be `Y_0_TOP` like the slots and
  the 2D screen); the fence wait lands in the SAME commit as the first blit
  (I-45's in-flight clause); its gate grows a QMP pixel arm and the C-2d
  hidden→visible redraw leg. C-2a/b/c/d are landed and each exercised on both
  capability arms.
- **The console TX ring is byte-atomic** (`bug_console_tx_ring_byte_atomic.md`)
  — kernel diagnostics and SYS_PUTS tear each other char by char; a per-message
  push under one ring lock, on the LS-8 surface (audit row + SMP gate). Handed
  to aux on yip 0024 (they were in `cons.c`); otherwise whoever next opens it.
- **The C-2c + C-2d-b audit round** on `usr/tapestryd` (I-40 + I-45's
  guest-exposure half: the import is a new cross-context authority path) —
  owed since `f86177b6`, wider now; needs agent spawning
  (`memory/audit_c2d_prosecutor_prompt.md`, extend its scope to the C-2c
  commit).
- **Two thirds of the extinction tear** (the vault seam, `IPI_HALT`), and a
  prosecutor round owed on the landed third.
- **`main#228`** — Fable rounds on C-0d and #243, quota-blocked. Deliberately
  *not* run on an Opus fallback: what is owed there is lineage independence, and
  a fallback round would spend the surface without buying it.
- **`docs/REFERENCE.md`'s snapshot block** — dead since Phase 5 (above). Needs a
  decision about what it is for, not a patch.
