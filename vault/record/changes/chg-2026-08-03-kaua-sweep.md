---
id: chg-2026-08-03-kaua-sweep
type: chg
title: "kaua — and the shape found three times: in the code, in the vault's own record, and in my memory index"
date: 2026-08-03
arc: arc-vault
commits: []
touched:
  - sub-kaua
  - moc-userspace-shell-tui
established:
  - sub-kaua
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-03
---
Batch 41: kaua, the console TUI substrate — term, source, query, input, encode,
buffer, event, style, rect, layout, widget, lib. 12 files, 3830 lines. Main
unchanged at `9b994f2d` (already an ancestor). L-1 absent on the TWENTY-NINTH
check.

**#94 SPLIT THREE WAYS BEFORE STARTING, IN DEPENDENCY ORDER** — the #93
precedent. kaua (3830) and parley (3919) are independent; nora (11491) consumes
both, so writing it third means both wikilinks resolve. Now #115/#116/#117.

**THE SUBJECT IS STRUCTURALLY TWO CRATES.** Nine modules are pure values and pure
functions with no I/O; three (`term`, `source`, `query`) sit behind a `backend`
feature because they alone need libthyla-rs. `audit: light`, and the split is the
reason: the capability story is that kaua touches fd 0 and fd 1 and **nothing
else** — never consctl, never console-attach — so a kaua app is safe untrusted
and [[inv-i27]] is untouched. That property is preserved by OMISSION, which is
worth naming: no gate refuses a future module that reaches for consctl.

**F1 -- THE VAULT'S OWN RECORD WAS WRONG, AND kaua IS THE COUNTER-EXAMPLE
(#105 corrected).** Batch 38 recorded that ~878 `#[test]` functions across six
native crates cannot compile, and named kaua as having "unconditional
`#![no_std]`" and failing "on both counts". kaua carries
`#![cfg_attr(not(test), no_std)]` — since its FIRST commit — plus an optional
libthyla-rs behind a default-on `backend` feature, and documents the exact
host-test command in its own Cargo.toml. Measured, by running every crate rather
than two:

    kaua      92  PASS       parley 73  PASS       libdriver 86  PASS
    libutopia 385 STRANDED   nora  238  STRANDED   tapestryd  4  STRANDED
    -> 251 run today; 627 stranded, not 878.

The per-crate COUNTS were all correct; only the verdicts were wrong. Batch 38 ran
netdev to confirm the pattern works and libutopia to confirm the failure, then
inferred the other four — so its claim's subject (six crates) was broader than
what it checked (two). **That is batch 28's own finding — "the claim's subject was
narrower than the claim" — committed by the arc itself, inverted.**

What survives, and it is still worth having: libutopia's 385 — the largest block,
including a parser whose header says "Pure logic; no I/O; host-testable" — have
never run, and nora's 238 with them. The fix is mechanical and now demonstrated
FOUR times inside this repo rather than once.

**F2 -- ONE FILE DESCRIBES ITS OWN ALGORITHM THREE TIMES, AND ONE DESCRIPTION IS
THE BUG IT FIXED.** `query.rs`'s CPR size handshake was fixed for HVF at
`60d8f775`: a TOTAL deadline re-polling the remaining budget per byte, so a
dribbled reply assembles while a slow peer still cannot multiply the budget. The
module header documents this AND explains why the old way was wrong — it "assumed
one-drain delivery... and gave up the instant the ring went empty mid-reply". The
inline comment at the loop says the same.

Both `///` doc comments still describe the OLD algorithm. `read_cpr`: "the first
poll on the full deadline and the rest non-blocking". `terminal_size`: "waiting up
to `timeout_ms` for the reply to START (then non-blocking; see the module header)"
— which points the reader at the header that refutes it.

The fix commit shows the mechanism: it rewrote the `//` header, added the `//`
inline comment, and touched neither `///`. **The author updated the prose they
were reading while working, and not the prose the reader receives** — rustdoc
renders the doc comments, not the header.

**F3 -- AND THE SAME SHAPE IN MY OWN MEMORY INDEX.** MEMORY.md carried "nora
unusable under HVF — OPEN" pointing at a file whose disposition line reads "**FIX
LANDED**". Seven weeks stale, against its own detail file. Corrected in this
batch.

The sharper part: that same file records "kaua host suite 73/73" passing on
2026-06-15. **The evidence against batch 38's kaua claim was sitting in my own
memory when the claim was written.**

**PATTERN, EIGHTEEN BATCHES — AND THIS TIME IT INCLUDES THE ARC ITSELF.** b40:
the refutation was in the direct caller. b41 finds the same shape at THREE levels
in one sitting — in the code (a fix that updated the header and not the docs), in
the vault's record (a census that checked two and asserted six), and in my memory
index (a one-line summary stale against the file it links to). One statement
covers all three: **an update lands where the work is, not where the claim is.**
The arc has been documenting that in the tree for eighteen batches and just found
it twice in its own bookkeeping.

The discoverability consequence is not hypothetical either: the erroneous census
lives in [[chg-2026-08-03-utopia-parser-sweep]] on the append-only Record plane,
which cannot gain a `superseded-by` field. So this correction is reachable only
from the correcting side — a live instance of the gap already recorded as a
quaestor task, now with a real correction stranded behind it.

**THE COUNTERWEIGHT IS A CRATE THAT STATES ITS OWN AUDIT INVARIANT AND MEANS IT.**
`input.rs` opens with "AUDIT INVARIANT (the load-bearing property of this file):
the parser holds O(1) state... NO input, however long, malformed, or adversarial,
grows its memory or makes it loop" — and delivers: a CSI parameter flood overflows
into a latched flag and the sequence is consumed to its final byte yielding no
event; an invalid UTF-8 lead consumes a bounded run and resets. Naming the property
that matters, in the file that owns it, is the thing most of this arc's findings
are the absence of.

Three more, traced sound: the drain loop refuses to FLUSH when it stops on the cap
(a half-assembled sequence survives to the next round rather than being mis-keyed);
the ESC holdoff, which pays 50 ms once per genuine lone-Escape press so a split
arrow key assembles; and the launch probe's losslessness — non-reply bytes are
handed to the steady-state parser through an explicit `with_pending` constructor,
and the read stops at `R` so later bytes stay in the kernel ring.

LEDGER, read off the rendered view — written after `render`, not before, per the
rule the last four entries have been sharpening. Corpus 836 -> **838**. Coverage
224 -> **236 owned of 421**, 53% -> **56%**; unswept lines 67158 -> **63128**
(-6.0%). `usr/lib` 3/38 -> **15/26**, the first movement in that area since the
libthyla-rs sweep.
