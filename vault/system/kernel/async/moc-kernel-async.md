---
id: moc-kernel-async
type: moc
title: "Async — rings, shared pages, and the memory both sides can write"
parent: moc-kernel
created: 2026-08-02
updated: 2026-08-02
---
Two mechanisms that move a boundary crossing off the data path. [[sub-kernel-loom]]
takes the *request* out of the syscall — it becomes a slot in a shared ring.
[[sub-kernel-weft]] takes the *payload* out of the copy — it becomes a page both
Procs map. Both trade a trap per operation for a region of memory the kernel and
somebody else can write at the same time.

## The organizing fact

Once the request or the payload lives in memory the other side can write, every
kernel decision has to be made against a private copy. Both files do this the
same way, and it accounts for most of what looks redundant in them:

> **A shared word may bound the work. It may never locate the memory.**

So each shared control block has a kernel-private twin that is the real
authority, and the shared copy is a *mirror* the kernel publishes for the other
side to read:

| Shared word | Written by | How the kernel treats it |
|---|---|---|
| Loom `sq_tail` | user | advisory — "is there work"; bounds the drain |
| Loom `cq_head` | user | advisory — gates the wait and the fullness test |
| Loom `sq_head`, `cq_tail` | kernel | published mirror; authority is `l->sq_head`, `l->cq_tail` |
| Loom `sq_mask`, `cq_mask`, entry counts | kernel, once | mirror, **never read back** — masking uses the private counts |
| Weft `prod_tail` | guest | advisory — bounds the drain |
| Weft `cons_head` | kernel | published mirror; authority is the view's |
| Weft geometry words | kernel, once | mirror; authority is `weft_ring_view` |

The advisory words are safe *because of what they cannot do*. A hostile
`cq_head` makes a Proc overwrite its own unreaped completion, or wait for the
wrong thing — it never produces an index. Every actual index is
`private_counter & (private_mask)`.

There is exactly one user-written word that does reach an index — the Loom
submission ring's indirection slot, which names an SQE — and it is range-checked
against the private entry count before it indexes anything. It is the exception
that shows the rule is deliberate rather than incidental.

And the payloads themselves are copied out before they are believed: an SQE is
copied whole into kernel memory before any field of it is read, a Weft
descriptor is copied before it is validated. What the kernel acts on is never
the slot the other side can still be writing.

## The deferral shape, for the third time

This area's completion callback runs under the 9P engine's lock and may not
sleep or re-enter the engine. So the work it cannot do — re-issuing a multishot
op, dispatching a chain successor — is flagged and handed to a later context
that runs outside that lock.

That is the same structure as the two areas swept before it, with a different
reason each time:

- [[moc-kernel-console-gfx]] — an interrupt handler may not walk the poll list,
  so it flags and a manager thread walks it.
- [[moc-kernel-entry]] — a handler may not deliver a note mid-exception, so it
  is deferred to the return tail.
- here — a completion may not re-enter the engine, so it is deferred to the
  drive loop.

Worth naming because the bug class travels with it: the interesting failure is
never the deferral itself, it is a context that *should* run the deferred work
and does not. [[seam-el0-irq-tail-no-notes]] is that bug in the entry area;
[[seam-loom-rearm-needs-blocking-enter]] is the same shape here, caught by
looking for it.

## Children

- [[sub-kernel-loom]] — the submission/completion rings, the registered handle
  and buffer tables, the submit-time pin, and the poll thread.
- [[sub-kernel-weft]] — the cross-Proc shared page: the share registry, the
  descriptor ring, the readiness poke, and the orphan reaper.

## Cross-cutting

- Invariants: [[inv-i29]] (completion integrity), [[inv-i30]] (submit-time pin
  and ring TOCTOU), [[inv-i37]] (dataplane integrity).
- The engine both drive: [[sub-kernel-ninep-client]].
