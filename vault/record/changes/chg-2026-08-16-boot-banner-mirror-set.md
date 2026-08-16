---
id: chg-2026-08-16-boot-banner-mirror-set
type: chg
title: "The boot-banner ABI note carried the phantom it was supposed to be the cure for"
date: 2026-08-16
arc: arc-vault
commits: []
touched: [abi-boot-banner]
established: []
closed: []
opened: [seam-boot-banner-coupdate-list]
mirrors-checked: [tools/test.sh, tools/smp-multiboot.sh, tools/test-cross-reboot.sh, tools/test-fault.sh, tools/ci-idle-gate.sh, tools/np3-bench.sh, tools/verify-kaslr.sh, tools/warp/boot-probe.sh, tools/interactive/lib.exp, tools/interactive/dap-nora.exp, tools/interactive/flood-174.exp, tools/interactive/freeze-172.exp, tools/interactive/ls-gfx-font.exp, tools/warp/quarry-wedge.exp, tools/stall-watch.py]
depth: rich
created: 2026-08-16
---
This is not a sweep of a churned surface. It is what fell out of the routine
`git merge main` before one, and the shape is worth as much as the content:
**a merge conflict is a diff against another branch's beliefs, and it is the
only moment those beliefs are forced into view.**

## How it surfaced

Main landed main#244 — retiring `tools/agent-protocol.md`, a file named as a
mandatory co-update target for the boot-banner ABI across four binding
documents, planned in Phase 1 and never written. The user voted to retire the
citation rather than write the doc, on the reasoning that a third copy of a
protocol already stated twice is a third thing to drift.

The merge conflicted on `docs/reference/01-boot.md`, because main edited the
sentence and the vault had replaced the whole file with an absorption stub.
Resolving it meant reading their correction — and their correction named a
string. Grepping the vault for that string returned one hit:
[[abi-boot-banner]], the note whose entire purpose is to be the authority on
this ABI.

**Main's census was complete on their branch and structurally blind to mine.**
`vault/` does not exist on `main`. Four sites there, a fifth here, and no
grep either of us could run would have found all five — the same shape as
[[chg-2026-08-15-syscall-abi-collision]]'s two branches drawing from one free
list, and the same remedy: the far branch's tip is part of the check.

## Four defects, of which three are the note's and one is the scripture's

**1. The phantom, in the note that exists to prevent phantoms.** "Why it is
frozen" listed `tools/agent-protocol.md` among the files that "all key on
these two strings." Fixed.

**2. `tools/run-vm.sh` consumes neither string** — zero matches, verified, and
the reason is structural rather than accidental: it is a 496-line QEMU
*launcher* that assembles a command line and hands over an interactive UART.
It never reads boot output. It is the **first** member of the four-file list in
both its pre- and post-#244 forms.

This is the finding that outlives the fix. main#244's own argument is that an
unfollowable member teaches the reader the whole list is advisory, and the
members beside it are real. An **inert** member does that identically to a
**fictional** one: a reader who changes the banner and dutifully opens
`run-vm.sh` finds nothing to change, and learns the same lesson. Removing the
provably-fictional member while leaving the inert one preserves the damage
main#244 set out to repair. [[seam-boot-banner-coupdate-list]].

**3. The negative claim was false, and it was the kind that is satisfiable by
not looking.** The note said the banner's informational fields — naming
`kernel base:` among them — are free to evolve, and "Nothing matches on it."
Two tools do:

- `tools/verify-kaslr.sh:55` greps `KASLR offset 0x[0-9a-fA-F]+`. It is the
  ROADMAP §4.2 exit-criterion gate for [[inv-i16]] — the invariant's only
  runtime witness. It fails **loud**: an unparsed offset makes every boot's
  offset the empty string, the distinct-offset set collapses to size 1, and
  the `>= floor(N*0.7)` bar rejects it.
- `tools/stall-watch.py:84` compiles a regex over the whole line including the
  `(KASLR offset 0x…` parenthetical. It fails **silent** — `if m:` with no
  `else`, so `syms.slide` stays `None`, the watcher keeps running, and it
  simply stops symbolizing. That is the diagnostic it exists to provide, lost
  at the moment a guest has stalled.

The asymmetry is the part worth keeping. The note told a reader this field was
free to evolve. One consumer would have told them otherwise within a run; the
other would have quietly stopped helping.

**4. The prefix is ABI and the message body is not — except where it is.**
`tools/test-fault.sh:53-59` matches seven specific extinction messages
(`stack canary mismatch`, `PTE violates W^X`, `BTI fault`, `kernel stack
overflow` ×3 provokers, `recursive kernel fault`). Reword one for clarity and
that fault-injection variant reports the protection did not fire — a
false-negative on a hardening gate. Its own `:48` comment says "Keep the case
below in sync with this", which is an instruction to a person inside the file,
the exact form [[dec-2026-08-15-cutover]] ratified as insufficient.

## The census, and its control

Fourteen files under `tools/` match one or both literals; two more mention
them in comments only (`warp-host.sh:8`, `interactive/go8d.exp:15`), which go
stale rather than break and are therefore not mirrors. **14 + 2 = 16**, the
total the unfiltered grep returned — `done + remaining == total` over the whole
partition, which a broken extraction cannot satisfy. The classification was
done by reading each hit, because a hit count cannot tell a `grep -q` pattern
from a usage comment, and the two failure modes are different in kind.

`mirrors` went from three entries to fifteen. Worth stating precisely, because
the tempting version of this story is wrong: the frontmatter was **not** "right
where the prose was wrong." It contained no fiction and no inert member, which
the prose did — but it was missing eleven real consumers. Both were incomplete;
only one was also false. I formed the cleaner claim first and checked it before
writing it down.

## A stub that vouched for its successor

`docs/reference/01-boot.md`'s absorption stub criticized the old document's
co-update list — for a *different* defect (the document was not on its own
list) — and closed with "The ABI's home is now [the registry], which has it
right."

It did not. The stub asserted a property of **another file** at the moment of
writing the redirect, and nothing ever re-checked it. It also said "the four
listed places were updated", which two of the four could not have been: one
does not exist and one has nothing in it to update.

The quotation itself was accurate — verified against `b5285434^` — so it stays
as past-tense provenance: fix present and future claims, leave the record of
what a document once said. What was fixed is the present-tense certification. **An absorption stub is the
worst-placed artifact in the vault to certify its destination**: it is written
in the same breath as the redirect, by the person most confident the
destination is good, and it is read by exactly the people who will not check.
