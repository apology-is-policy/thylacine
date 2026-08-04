---
id: moc-userspace-boot-chain
type: moc
title: "The boot chain — the Procs that end up holding less than they started with"
parent: moc-userspace
created: 2026-08-04
updated: 2026-08-04
---
The Procs that run before there is a session: init, the hardware broker, the
identity coordinator, and the login gate. Named as an area by
[[moc-userspace]] from the beginning; populated as the sweep reaches each
member.

## The organizing fact

**Every member of this area is spawned holding an authority whose whole
purpose is to be given away, and each is finished when it holds less than it
started with.** Init is spawned console-attached and holding the
service-posting bit, and confers both onward before relinquishing the
console. The hardware broker is spawned with the create capability and an
*unnarrowed* hardware allowance, and spends the boot converting that into
per-driver narrowings it does not keep.

That shape has a consequence which is the reason to read these notes at all:
**an area whose job is delegation cannot be judged by what it protects, only
by what it computes.** A kernel dossier can point at a gate and say "nothing
gets past this". Here the honest question is the opposite one — *is the thing
being handed out the right size* — and the answer is arithmetic, not a check.

This is also where the system's capability rules are at their weakest, and
deliberately so. The kernel bounds a conferral by the conferrer's own
holding, which is a real rule that happens to be **vacuous against a
conferrer holding everything** — and every member of this area is such a
conferrer, because that is what being early means. So the invariants these
Procs participate in are held here, by construction rather than by
enforcement, and the only external check on the arithmetic is what the Proc
chooses to say about it.

Which makes a member's *reporting* load-bearing rather than cosmetic, and is
where the first swept member's findings both land: the broker's grants are
correct and its account of them is narrower than they are, in two independent
ways. On a plane whose computations nothing re-derives, the log is not a
diagnostic — it is the audit.

**This fact is written from one member.** [[sub-warden]] is the first swept
and a clean specimen: it holds a broad allowance, computes narrowings, and
confers exactly one further bit and only to the drivers that need it. The
other three are named by [[moc-userspace]] and have not been read; expect
this paragraph to be re-derived rather than merely extended when they land.

## Children

- [[sub-warden]] — the hardware broker: discovery to bind to grant to spawn,
  and the supervision ladder that decides whether a device's absence should
  fail the boot. Holds the userspace half of [[inv-i34]]'s fourth leg, the
  one the kernel cannot check.

## Cross-cutting

- The two libraries the broker stands on are [[sub-libdriver-discovery]]
  (what is out there) and [[sub-libdriver-grant]] (what a manifest may have);
  the broker is where they meet, and the only place they do.
- The area's authority currency so far is [[inv-i34]] for hardware. Identity
  and the console-trusted path ([[inv-i27]]) arrive with the remaining
  members.
