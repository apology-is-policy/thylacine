---
id: moc-userspace-hardware
type: moc
title: "The hardware programs — EL0 talking to devices"
parent: moc-userspace
created: 2026-08-04
updated: 2026-08-04
---
The programs that reach a device from userspace: five standalone drivers, one
per virtio device class, and the two the broker hands a device to. Not the
libraries they use, which are in [[moc-userspace-runtime]], and not the servers
that sit on top, which are in the services area.

## The organizing fact

**Every member touches hardware directly and none of them is a service. Each is
spawned by something above it, does one bounded job, and reports through a
channel that party chose — never a terminal, and never a protocol.** There is no
member a person invokes and no member anything connects to.

That is why they read unlike the rest of userspace. There is no request shape to
learn, no fid table, no client. What a reader needs is the *spawn* contract: who
starts it, what it is handed, and what the starter does with what comes back.

### The split that decides how to read one

The area has two halves, and mistaking one for the other is the likely error,
because the code looks similar and the difference is entirely in the authority:

**The reference drivers** ([[sub-virtio-probes]]) predate the driver framework
and use the raw capability syscalls. They run under *broad* hardware authority —
nothing bounds which physical address they may claim, which is why each hardcodes
the bank base — and they are spawned and graded by the kernel's own test suite,
which reaps them and asserts on an exit status. They are the only end-to-end
exercise of the hardware-capability triple as a program uses it, and at least two
production drivers were ported from members of the set.

**The broker's leaves** ([[sub-menagerie-leaves]]) are the opposite on every axis.
They are granted a *narrowed* allowance, can touch nothing outside it, are spawned
by the warden rather than by a test, and report on a pipe or by staying alive.
They are two programs, 414 lines, and they are what the whole framework exists to
run.

Read one against the other and the contrast is the clearest demonstration
available of what [[inv-i34]] added: the same kind of work, done twice, once with
the authority assumed and once with it conferred.

### What this area keeps producing

Findings here are not about arithmetic. Every bound in both halves is either
correct or fails safe, and no defect found so far can corrupt anything outside
the program that holds it. What goes wrong instead is **the account** — an
obligation stated on the transport that does not need it, a counter described as
protection against a hang it cannot prevent, a page-sharing constraint whose only
written statement describes a function that was deleted, a guard defeated by a
cast one step earlier than the guard.

The pattern is sharp enough to be worth stating as an expectation: on this plane
the code is usually right and the story about the code is usually the thing that
is wrong. That follows from what the programs are. Four of the seven are proofs
whose passing is the only feedback anyone gets, and a proof that passes tells you
nothing about whether its comments are true.

## Children

- [[sub-virtio-probes]] — five drivers, one per device class, written against the
  raw capability syscalls and kept alive as kernel-suite gates. Where each device
  class was first made to work, and what a third one would be read from.
- [[sub-menagerie-leaves]] — the bus enumerator and the lifecycle driver: the two
  programs the warden spawns under a narrowed grant. The only programs in the tree
  that are handed hardware rather than taking it.

## Cross-cutting

- The libraries both halves stand on are in [[moc-userspace-runtime]] —
  [[sub-netdev]] for frame transport, and the two [[sub-libdriver-grant]] /
  [[sub-libdriver-discovery]] halves for everything about grants and identity.
- The party that spawns the second half is [[sub-warden]], on the boot-chain
  plane. Read it for what a grant *is*; read this area for what happens to one.
- The party that spawns the first half is the in-kernel test runner, which is not
  yet owned by any dossier — it is the subject of a standing item, and it is the
  reason the exit-status vocabulary here is the whole interface.
- The kernel objects being acquired are the hardware-capability handles, whose
  non-transferability is [[inv-i5]]. Nothing in this area enforces that; these are
  the programs that exercise the path where it holds.
