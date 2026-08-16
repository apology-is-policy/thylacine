---
id: abi-boot-banner
type: abi
kind: contract
stability: frozen
title: "The boot-banner contract — `Thylacine boot OK` and `EXTINCTION:`"
pinned-by:
  - "kernel/main.c (boot_mark_complete)"
  - "kernel/extinction.c"
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
created: 2026-08-01
updated: 2026-08-16
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
what that means concretely: **fourteen tools match one or both literals**, plus
`stall-watch.py` on `kernel base:`. Two more mention them in comments only
(`tools/warp-host.sh`, `tools/interactive/go8d.exp`) — they become wrong
rather than broken, so they are not mirrors. 14 + 2 = the 16 files under
`tools/` that carry either string.

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
misses fourteen that can. That is [[seam-boot-banner-coupdate-list]].

### What would make it safe-by-default

The program half of this is mechanically checkable and the document half is
not — `mirrors` above is the checkable list, and R6 already requires a change
touching this note to check it off. The gap is that nothing derives the list
from the tree. A lint rule that greps for the two literals and diffs the hit
set against `mirrors` would have failed on the day `dap-nora.exp` was written.

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
  states, which names one file that cannot break and omits fourteen that can.
- A change to the `kernel base:` line is an ABI break too, notwithstanding
  that this note called it informational for two weeks. `verify-kaslr.sh` is
  I-16's runtime witness.
- A reworded extinction **message** is an ABI break for `test-fault.sh`'s
  seven matched variants, which is not what "the prefix is the ABI" leads a
  reader to expect.
- Any new path that can print `Thylacine boot OK` outside
  `boot_mark_complete` breaks the one-shot console-attached gate, which is
  the only thing preventing a forged PASS.
- A consumer that matches the banner without an extinction pre-check, or
  without the grace window, will report a crashed boot as green.

## Referenced by

[[sub-substrate-gates]] · [[sub-substrate-machine]] ·
[[sub-substrate-interactive]] · [[moc-substrate]].
