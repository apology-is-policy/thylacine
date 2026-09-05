---
id: abi-boot-banner
type: abi
kind: contract
stability: frozen
title: "The boot-banner contract — `Thylacine boot OK` and `EXTINCTION:`"
pinned-by:
  - "kernel/main.c (boot_mark_complete)"
  - "kernel/extinction.c"
  - "kernel/cons.c (cons_kernel_writer_begin/end -- the DELIVERY half)"
  - "docs/TOOLING.md §10"
mirrors:
  - "tools/test.sh"
  - "tools/smp-multiboot.sh"
  - "tools/test-cross-reboot.sh"
  - "tools/test-fault.sh (also the extinction MESSAGE bodies — see below)"
  - "tools/ci-idle-gate.sh"
  - "tools/np3-bench.sh"
  - "tools/verify-kaslr.sh (also `KASLR offset` — see below)"
  - "tools/warp/boot-probe.sh"
  - "tools/interactive/lib.exp"
  - "tools/interactive/dap-nora.exp"
  - "tools/interactive/flood-174.exp"
  - "tools/interactive/freeze-172.exp"
  - "tools/interactive/ls-gfx-font.exp"
  - "tools/warp/quarry-wedge.exp"
  - "tools/stall-watch.py (`kernel base:` — see below)"
  - "tools/check-arc-gates.sh"
  - "tools/display-modes/verify-console-mode.exp"
  - "tools/display-modes/verify-gpu-headless-1b.exp"
  - "tools/interactive/item10-ctrlc.exp"
  - "tools/interactive/ls-gfx-age.exp"
  - "tools/interactive/ls-gfx-restore.exp"
  - "tools/interactive/ls-gfx-session.exp"
  - "tools/interactive/ls-halcyon.exp"
  - "tools/interactive/pty-susp-pouch.exp"
  - "tools/interactive/r5f9-ash.exp"
  - "tools/test-smp-classify.sh (the classifier's own fixtures — both literals)"
  - "tools/testdata/smp-classify/real-pass-harness.log (a classifier input fixture)"
  - "tools/warp/composed-screen.exp"
literals:
  - "Thylacine boot OK"
  - "EXTINCTION:"
  - "kernel base:"
literal-scan:
  - "tools"
literal-mentions:
  - "tools/warp-host.sh (a usage comment)"
  - "tools/interactive/go8d.exp (a prose note)"
created: 2026-08-01
updated: 2026-09-05
---
## The surface

Exactly two strings on the UART are kernel ABI with the development tooling:

- **`Thylacine boot OK`** — boot success. Must appear on a line by itself.
- **`EXTINCTION:`** at start-of-line — catastrophic kernel failure (an
  Extinction Level Event; the thylacine's fate transposed onto a kernel that
  has lost the will to continue).

Everything else the banner prints — `arch:`, `cpus:`, `mem:`, `dtb:`,
`hardening:`, `features:` — is **informational** and free to evolve.

**`kernel base:` is not, and this note said it was.** Two tools parse it:
`tools/verify-kaslr.sh` greps `KASLR offset 0x[0-9a-fA-F]+` out of it, and
`tools/stall-watch.py`'s `KASLR_RE` compiles a regex over the whole line
including the parenthetical. Their failure modes differ in the way that
matters — the first
fails **loud** (an unparsed offset makes every boot's offset the empty string,
so the distinct-offset count collapses to 1 and misses the `>= floor(N*0.7)`
bar), the second fails **silent** (`if m:` with no `else`; `syms.slide` stays
`None`, so the watcher keeps running and simply stops symbolizing — losing
exactly the diagnostic it exists to give, at exactly the moment a guest has
stalled). And `verify-kaslr.sh` is the ROADMAP §4.2 exit-criterion gate for
[[inv-i16]], so a reformat of a line this note called free-to-evolve would
take an invariant's only runtime witness with it.

**The `EXTINCTION:` prefix is ABI; the message body after it is not — except
that one gate depends on seven of them.** `tools/test-fault.sh`'s
`expected_marker` case matches `EXTINCTION: stack canary mismatch`, `... PTE
violates W^X`, `... BTI fault`, `... kernel stack overflow` (three provokers),
and `... recursive kernel fault`. The comment directly above it says "Keep the
case below in sync with this" — an instruction to a person, inside the file,
which is the weakest form of the guarantee ([[dec-2026-08-15-cutover]]).
Reword one of those messages for clarity and the corresponding
fault-injection variant reports the protection did not fire.

## Why it is frozen

It is the whole agentic loop's success signal, and the mirror set above is
what that means concretely. Since the 2026-09 resync grew the set to
**twenty-eight** (it added thirteen consumer gates — see "The resync grew the
set to twenty-eight" below): **twenty-seven mirrors match one or both of
`Thylacine boot OK` / `EXTINCTION:`** — one of those twenty-seven,
`real-pass-harness.log`, is a captured-log fixture, data not a program — plus
`stall-watch.py` on `kernel base:`. Two more mention the literals in comments
only (`tools/warp-host.sh`, `tools/interactive/go8d.exp`) — they become wrong
rather than broken, so they are not mirrors. 28 mirrors + 2 mentions = the 30
files under `tools/` that carry a literal.

**Reading the counts below.** The dated measurements further down (the 2026-08-18
main#245 census, the delivery classification) describe the **fifteen-member set
as it then stood**; they are kept as the historical record. The current totals
are the twenty-eight above, the delivery table is updated to twenty-eight, and
the resync's thirteen gates are classified in their own subsection.

### The resync grew the set to twenty-eight (2026-09)

The 2026-09 vault resync added thirteen consumer gates to `mirrors` — the
`.exp`/`.sh` gates that `expect` or watch a literal and break if it changes,
which had accumulated on `main` while the vault branch was behind. All thirteen
match a literal (verified by grep); by which:

- **`Thylacine boot OK`** (5): `check-arc-gates.sh`, `verify-console-mode.exp`,
  `verify-gpu-headless-1b.exp`, `test-smp-classify.sh`, `real-pass-harness.log`.
- **`EXTINCTION:`** (10): `verify-gpu-headless-1b.exp`, `item10-ctrlc.exp`,
  `ls-gfx-age.exp`, `ls-gfx-restore.exp`, `ls-gfx-session.exp`, `ls-halcyon.exp`,
  `pty-susp-pouch.exp`, `r5f9-ash.exp`, `test-smp-classify.sh`,
  `composed-screen.exp`.
- **`kernel base:`** — none; `stall-watch.py` remains the sole matcher.

By the four-class taxonomy above (program / document / inert / phantom), eleven
are **programs that deliver**: the ten interactive/display/warp `.exp` gates
boot a real guest through `lib.exp`, so a reworded literal fails to match real
boot output and is caught, and `check-arc-gates.sh` reads real boot output for
its verdict. **Two do not deliver**: `test-smp-classify.sh` is the classifier's
own unit test — explicitly "no boots", over fixtures — and
`real-pass-harness.log` is one of those fixtures (data). For those two the
literal is self-consistent within a crafted input; unlike the 2026-08-18
`test-fault.sh`/`verify-kaslr.sh` finding, these two are no-delivery **by
design** (a classifier unit test must not boot), so their obligation is
co-update only, never detection.

This grew the delivery table's `Thylacine boot OK` matchers 8 -> 13 and
`EXTINCTION:` 14 -> 24. The growth is recorded by
[[chg-2026-09-05-boot-banner-mirror-recount]], which carries `mirrors-checked`
for all twenty-eight — the R6 grandfather fix ([[chg-2026-09-05-r6-grandfather]])
is what lets a mirror-growing chg carry the full current set cleanly.

### The co-update list has never described that population

`TOOLING.md §10` and `CLAUDE.md` state a **four-file lockstep**. Until
2026-08-15 it read `tools/run-vm.sh`, `tools/agent-protocol.md`, `CLAUDE.md`,
`TOOLING.md`. Every member is worth stating plainly:

- **`tools/agent-protocol.md` never existed.** Planned in Phase 1, never
  written, named as a mandatory co-update target for the project's life.
  Retired 2026-08-15 on the user's vote (main#244) in favour of pointing at
  `TOOLING.md §10`, which *is* the agent-side protocol. **This note carried the
  phantom too**, in this section, until the sweep below.
- **`tools/run-vm.sh` consumes neither string** — zero matches; it is a 496-line
  QEMU *launcher* that builds a command line and hands over an interactive
  UART. It never reads boot output. It is still the first member of the
  corrected list. A reader who changes the banner and dutifully opens it finds
  nothing to change, which is the same lesson main#244 drew about the phantom:
  an unfollowable member teaches the reader the list is advisory, and the
  members beside it are real.
- **`CLAUDE.md` and `TOOLING.md` are documents, not matchers.** They go stale;
  nothing breaks. Putting them in one undifferentiated list with programs is
  why the phantom and the inert member could both sit there unremarked: the
  list has no property that any member is checked against, because its members
  do not share one.

So the corrected four-file list still names one file that cannot break and
misses twenty-seven that can (the boot-OK/EXTINCTION matchers; fourteen when
this was written, twenty-seven since the resync). That is
[[seam-boot-banner-coupdate-list]].

### A fourth class the taxonomy above does not have: the mirror nothing runs

The three failure classes enumerated above are *phantom* (named, never
existed), *inert* (exists, matches nothing), and *document* (matches, but only
goes stale). The implied fourth is the healthy one — the **program**, which
"breaks silently and immediately".

Measured 2026-08-18 (main#245): **two of the fifteen mirrors were programs that
nothing invoked.** `tools/test-fault.sh` and `tools/verify-kaslr.sh` had no
Makefile target, no gate, no CI step, and no caller anywhere in `tools/` — the
only references to either were two *comments* in sibling scripts. Established
by a census over `Makefile` + `tools/` + `.github` with a control at each end
(`ci-smp-gate.sh` resolved to a target; `test-fault.sh` resolved to prose).

So they hold a program's **update obligation** — reword a literal and they must
be co-updated, exactly as `mirrors` says — while having **no failure behaviour
at all**. They do not break loudly, and unlike a document they do not even
become visibly wrong to a reader: nothing executes them, so the mismatch is
never evaluated. That is strictly worse than the document class, which at least
misleads someone who reads it.

**Why this matters to THIS note specifically.** The mirror rule is sound and
unaffected: it answers "who must be co-updated", and an unrun program must
still be co-updated. But it is easy to read a fifteen-member derived mirror set
as *defence in depth* — fifteen consumers that would catch a botched reword.
For two of those fifteen that was false, and would have stayed false
indefinitely. **A mirror set bounds the co-update obligation; it does not
bound the detection latency, and only the members something actually runs
contribute to detection at all.**

This is the same shape as [[seam-extinction-line-unserialized]] one level up: a
contract on a *value* is silent about its *delivery*, and here a contract on the
*set of readers* is silent about whether any of them ever *reads*.

Closed for these two on 2026-08-18 by main `55c5d2f8`, which gave each a
Makefile target and — the load-bearing half — an entry in `CLAUDE.md`'s
"Build + test commands" block. Both were *already* named in `CLAUDE.md`, twice
each, the same count as `test-a72` and `check-v80-floor`, which did not rot; the
difference was that the orphans appeared only in this surface's own prose,
listed as things that would BREAK, never as commands to run. Presence in the
auto-loaded file is not the anti-rot property; presence in its **command block**
is.

### The mirror set is now DERIVED, not declared

The program half of this is mechanically checkable and the document half is
not, so that is exactly where the line was drawn. `literals` /
`literal-scan` / `literal-mentions` in this note's frontmatter are the
declaration; quaestor sweeps the scan roots and diffs the hit set against
`mirrors` ∪ `pinned-by` ∪ `literal-mentions`. **It fails, it does not warn.**

Two directions, because they catch different failures:

- **undeclared** — a file matches a literal and this note does not name it.
  The new-consumer case: someone writes another `.exp` and it silently joins
  the ABI's blast radius. This would have fired the day `dap-nora.exp` was
  written.
- **unmatched** — this note names a file containing none of the literals. The
  rename-or-retire case, and the one that turns a mirror list into fiction an
  entry at a time. It is how `tools/agent-protocol.md` survived: nothing ever
  asked whether the named file matched anything.

Plus a **positive control**: an empty hit set is a FAIL, never a pass. A
declared ABI whose literals appear nowhere means the scan is broken or the
literals are wrong. That control is not decoration — the first implementation
borrowed a tree-walker whose filter admits only C-family kernel sources, so
scanning `tools` returned no files and the check reported all fifteen mirrors
as unmatched: fifteen confident findings measured against no data.

**It also runs inside `quaestor owner`**, which is the half that matters, and
the condition main set when voting for this shape: a lint that only runs under
`vault lint` is safe-if-remembered wearing a check's clothing. The instance
rewording a banner string is on another track and never runs the vault's lint
suite — but it does run `owner` at the mandatory doc-update step, on the very
paths it is changing. A file matching a literal and absent from `mirrors` is
named to the person creating it.

**What it cannot judge is comment-versus-code.** `tools/warp-host.sh` names the
banner in a usage comment and `tools/interactive/go8d.exp` in a prose note;
both go stale rather than break, so neither is a mirror. That call is made once
per file and recorded in `literal-mentions`. If a file listed there later grows
a genuine matcher, the check stays quiet — from the outside the two look
identical. What it does guarantee is that **no file joins the hit set
unnoticed**: the mention list is a set of decisions somebody made, not a set of
files somebody skipped.

**And it cannot cover the document half at all**, by construction. `docs/` is
full of prose quoting these strings, so a scan there would drown. That is not a
gap being hidden — it is the diagnosis above, honoured: the co-update list
conflates programs (which break) with documents (which merely go wrong), and
only the first kind is derivable. The other half is closed differently:
`TOOLING.md` and `CLAUDE.md` now **repoint at this note's `mirrors`** instead of
carrying a competing transcription. Neither half would have sufficed alone.

## The emission rule, and what changed at A-5a

The banner is **not** printed by `boot_main` at the end of bring-up, and it
is no longer tied to init's exit. joey signals `SYS_BOOT_COMPLETE` after its
boot-test asserts pass and just before it becomes the persistent session
supervisor (it getty-loops `/sbin/login`), and `boot_mark_complete()` prints
the line. joey is long-running init and does not exit on success, so there
is no exit to ride.

Three properties the emission must keep:

1. It appears **only after** init's boot-test asserts have passed — a
   pre-completion failure exits joey non-zero, which extincts in `joey_run`,
   so the banner never prints.
2. It does not appear if the kernel extincted, or if init failed, before
   signalling.
3. `SYS_BOOT_COMPLETE` is **one-shot and gated on the caller being
   console-attached** (joey, the boot console-trust anchor) — so a spawned
   child cannot emit a premature banner and manufacture a false PASS.

## The delivery hazard: an unchanged string that does not arrive

Everything above is about the string's **value**. None of it covers the string
**arriving intact**, and that is a separate ABI surface which was open for the
project's life.

The banner was emitted through the lock-free byte-at-a-time UART path, holding
none of the console's writer role. The compositor writes the console *through*
that role. A role serializes only against writers who take it — so on a boot
where both wrote at once the result was

    Thylacine tboapot esOKtr
    yd: scanout pending-direct 0 (1280x800)

the banner woven byte-wise through the compositor's line. **The string was
byte-correct, emitted by the right code, at the right moment, and did not
match.** The consumer timed out reporting no boot marker on a provably healthy
guest — login reached, both users authenticated, zero extinctions.

**"Must appear on a line by itself" reads as a property of the emitter and is
not one.** It is a joint property of the emitter and of every concurrent writer
of the same device, which means it cannot be established by inspecting the code
that prints it. This note stated the requirement in its first paragraph and
carried no obligation that would produce it.

Closed by enrolling the kernel's own emitters in the console writer role
([[sub-kernel-cons]]). Two properties of that fix belong here because they bound
what the guarantee is worth:

- **It excludes the extinction path deliberately.** Those emitters run on a
  dying machine and must stay lock-free and bounded, so `EXTINCTION:` has the
  *old* delivery guarantee — none. That is the right trade (a torn crash line
  still contains the anchored prefix far more often than a parked crash
  handler prints anything at all), but it means the two ABI strings do not have
  the same integrity.
- **It prefers a torn line to a dropped one.** If the park is interrupted by a
  death unwind, the emitter proceeds *unserialized* rather than losing the
  line.

### The protected string has the narrower readership

Forced to enumerate by the mirror rule, and the answer inverts the fix's value.
Classifying all twenty-eight mirrors by which literal each actually matches
(a mirror matching two literals is counted in both rows):

| Literal | Delivery | Mirrors matching |
|---|---|---|
| `Thylacine boot OK` | **serialized** (writer role) | 13 |
| `EXTINCTION:` | **unserialized** — lock-free, no role | **24** |
| `kernel base:` | unserialized | 1 |

**Almost every consumer of this ABI matches an unserialized string** — 24 of
the 28 match `EXTINCTION:` and one matches `kernel base:`, both emitted without
the writer role — while the one string that got a delivery guarantee, the
banner, is matched by under half of them. The crash path emits through the same lock-free byte-at-a-time put the
banner used, does **not** stop peer processors first, and its pre-emit flush is
a bounded try-lock that *skips* when a peer holds the ring — so a peer mid-write
is precisely the case it declines to handle.

Two costs, and they differ in kind:

- **A torn prefix loses a corruption verdict.** Consumers check `^EXTINCTION:`
  first on every poll, and the multi-boot classifier keys its corruption class
  on it. A tear demotes a real corruption to the unclassified bucket — the
  classifier's own worst outcome, arrived at from outside it.
- **A torn message body inverts a fault-injection result.** The fault gate
  matches seven full message strings (seventeen matches in that file alone); a
  torn one reports that a protection **did not fire** when it fired correctly.

Recorded as [[seam-extinction-line-unserialized]] rather than fixed here. The
exclusion is deliberate and the reasoning behind it is sound — a primitive that
parks must never run on a dying machine — so the lift is a *try*-acquire of the
role rather than a park, which is the shape the pre-emit flush already uses.
That is the implementation track's call.

## Consumer obligations

- **Extinction outranks the banner.** A crash is a FAIL whether or not the
  banner also printed. Every consumer checks `^EXTINCTION:` first on every
  poll.
- **The banner is not the end of the boot.** Because joey persists, a getty
  or login fault can crash *after* a green banner. Consumers watch a grace
  window (`BANNER_GRACE`, default 3 s) before declaring PASS.
- **Match with `grep -a`.** Boot logs carry binary spill; without it grep
  declares the file binary and reports "binary file matches" — which a `-q`
  test reads as a match and a negated test reads as its opposite.
- **Anchor `EXTINCTION:` at start-of-line** (`^`). It appears mid-line in
  quoted context (log slices, this note) and an unanchored match reports a
  crash that did not happen.

## Prosecution

- Any change to either string is an ABI break requiring the **full `mirrors`
  set** to move in the same commit — not the four-file list the scripture
  states, which names one file that cannot break and omits the twenty-seven
  boot-OK/EXTINCTION matchers that can.
- A change to the `kernel base:` line is an ABI break too, notwithstanding
  that this note called it informational for two weeks. `verify-kaslr.sh` is
  I-16's runtime witness.
- A reworded extinction **message** is an ABI break for `test-fault.sh`'s
  seven matched variants, which is not what "the prefix is the ABI" leads a
  reader to expect.
- **Owed** (deferred at the 2026-09 recount): whether the `el1_sync_runaway`
  extinction-message body joins the pinned message-body set (as `test-fault.sh`'s
  seven are) is an OPEN question tied to [[seam-extinction-line-unserialized]]
  and #246. Its original context (yip 0026) is purged; deciding it needs the
  #246 el1_sync_runaway test's ground truth, so it is left open rather than
  guessed.
- Any new path that can print `Thylacine boot OK` outside
  `boot_mark_complete` breaks the one-shot console-attached gate, which is
  the only thing preventing a forged PASS.
- A consumer that matches the banner without an extinction pre-check, or
  without the grace window, will report a crashed boot as green.
- **A new kernel emitter of either literal must take the console writer role.**
  This is a delivery obligation, not a content one, and it does not appear
  anywhere in the co-update list or the derived mirror check — both of which
  reason entirely about *who reads the string*. Nothing here can detect an
  emitter that prints the right bytes unserialized; the failure surfaces only
  as a consumer timeout on a healthy guest.
- **Do not "fix" a tear by flushing before the emit.** A flush drains what is
  already queued and does nothing about a peer that starts writing during the
  emit. That half-fix was in place, with a comment naming this exact tear, and
  did not prevent it.

## Referenced by

[[sub-substrate-gates]] · [[sub-substrate-machine]] ·
[[sub-substrate-interactive]] · [[moc-substrate]] · [[sub-kernel-cons]] (the
delivery half).
