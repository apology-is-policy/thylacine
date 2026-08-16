---
id: chg-2026-08-16-notes-fp-and-phenotype
type: chg
title: "Notes re-swept: 'the entire user context' was one word too many, and it was mine"
date: 2026-08-16
arc: arc-vault
commits: []
touched: [sub-kernel-notes]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-08-16
---
265 lines from six vivarium/lineage commits, none of which appear in a
`--since` query against this dossier's own date. **They were authored before it
and arrived after it** — the vault branch did not merge them until two days
later. This is the `--first-parent` dating rule earning its keep in the
direction that is easy to miss: not "the doc is old", but "the doc was written
against a tree that did not yet contain a fix which already existed."

## The correction, and it is a single word

This note's dispatcher walkthrough said step 5 saves "the **entire** user
context into the thread."

Checked against the tree as this dossier saw it, the save was four lines: the
general registers, the user stack pointer, the link register, the status
register. **Not V0-V31, not FPSR, not FPCR.**

A note handler runs on the *same thread* as the code it interrupts with **no
context switch**, so the context switcher's eager FP save never fires here.
Nothing else did. The first handler instruction touching a vector register
silently corrupted the interrupted computation — any float arithmetic, any
autovectorised `memcpy`, any `printf("%f")`. Never an authority question (the
registers are the Proc's own); **silent data corruption**, live on the native
path since the signals sub-chunk, and made reachable by ordinary compiled C
once the phenotype landed.

## Why it hid, which is the reusable half

The four save lines *were* exhaustive — with respect to `struct
exception_context`. Every reader who asked "does delivery save the context?"
looked at the exception frame, enumerated its fields, and found the enumeration
complete. I did exactly that and wrote "entire".

The FP registers are not in that frame. They are preserved by a **different
mechanism**, for a different reason, and that mechanism is entirely correct in
its own domain — which is precisely why it is invisible. Nothing about reading
the exception frame suggests that some of the machine's user-visible state is
somebody else's job.

**A save is complete with respect to a STRUCTURE, never with respect to a
MACHINE.** State held by another correct mechanism is exactly the state an
enumeration of the obvious structure cannot see. Sibling to
[[chg-2026-08-16-burrow-attribution]]'s "the type says shape": both are cases
where the artifact in front of you answers a narrower question than the one
being asked, and answers it correctly.

Three details of the fix worth keeping. The 520-byte area is carried **inline**
on the thread (1232 → 1760 bytes) because delivery must be **alloc-free** — an
allocation failure mid-delivery would silently drop the handler invocation. The
existing switch-out FP slot **cannot** serve: preempt the handler and the
context switcher writes the *handler's* state there, destroying the snapshot.
And with two save sites and one shared restore, missing a site is **worse than
no fix** — the still-live restore writes a zeroed area into V0-V31. That was
observed (V0 reads 0, not the handler's pattern) rather than argued, and both
sites were revert-probed independently, each failing at its own assertion.

## The name IS the identity, and that bounds the POSIX mapping

The sharpest thing the phenotype work established is a property of the
*original* design: **a note carries no signal number.** Its name is its
identity.

Mapping SIGINT and SIGTERM both onto `interrupt` had been recorded as "a stated
imprecision, not an oversight". Building dispositions showed it is not
imprecise but **unrepresentable**: a guest ignoring SIGINT while leaving SIGTERM
at default has no correct answer — honour the ignore and SIGTERM goes silent
too, refuse it and a Proc that asked to ignore Ctrl-C dies on Ctrl-C. Both
wrong, and it is exactly the call a shell makes.

Generalised: where the name is the identity, **collapsing two identities onto
one name is a decision no downstream layer can un-make**, because the
distinguishing bit was never carried.

## Two mechanism notes that are about placement, not logic

**An ignored signal is dropped inside the post, not at delivery**, and the post
still succeeds (matching Linux). The first sketch dropped it at delivery. An
ignored note that reached the queue would occupy one of sixteen slots, would
**arm the terminate latch** — an ignoring Proc has no handler and is not
self-managing, so it passes every arm gate — and would leave blocked threads
unwinding until the return tail dropped it. Never posting touches none of that.
*A drop at the edge is not the same operation as a drop at the centre when
everything in between has side effects on arrival.*

**The phenotype branch sits above the native one deliberately.** "Does this Proc
have a live handler?" is answered by the native registration address, which is
0 for every phenotyped Proc — a Linux guest never calls the native register
syscall. So the "someone will catch this" exemption never applied to a
phenotyped Proc. Left unfixed that is a **hang, not a fidelity gap**: the latch
makes every unmasked thread's sleep return interrupted at once, and the
frame-push failure arms never drain it. *A fidelity gap in an exemption check
becomes a liveness bug when the thing exempted is a latch.*

## The dormant-constant caveat was scoped to one file; there are three

`NOTE_MASK_SUPPORTED` lives in three places with **three different values**:
`0x3f` in the kernel (bits 0-5, everything defined), `0x2f` in pouch (drops the
reserved `snare` bit, widened to add `tty` at PTY-1b), `0x1f` in libthyla-rs
(has `snare`, **missing `tty`** — unmoved since before PTY-1b, simply stale).

Each is defensible read alone, and **the three do not differ along one axis** —
kernel-vs-pouch is a policy disagreement about a reserved bit, kernel-vs-Rust
is staleness, pouch-vs-Rust is both at once. Lined up, `0x1f < 0x2f < 0x3f`
reads like a deliberate spectrum of narrowing scopes. **Two orthogonal
disagreements over one scalar cannot be told apart by comparing the scalar.**

The consumer picture inverts the usual worry: the kernel's copy is inert, the
Rust copy is inert (six mentions, all definition or doc comment), and **the
only copy anything reads is pouch's** — the one holding the third distinct
value. The live gap for native code is that **`NOTE_BIT_TTY` does not exist in
libthyla-rs at all**, in a crate exposing the other five, while `tty:` notes
have been deliverable, catchable and maskable since PTY-1b — and a native shell
is exactly the program that wants to defer them.

A dormant declaration is cheap. A dormant declaration **mirrored into places
that are not dormant** is not.

## Two counts of my own, both wrong, both small

The Contract said "Two paths, one queue" above what is now a three-row table,
and "Four families of note name" above a **five**-row table — the second wrong
when written, against a table in the same paragraph. Neither changes a
conclusion, which is why they survived: a miscount that no argument rests on is
invisible to every reader including its author.
