---
id: chg-2026-08-16-seven-small-surfaces
type: chg
title: "A rule stated as a mechanism is violated correctly by its first exception"
date: 2026-08-16
arc: arc-vault
commits: ["*(pending)*"]
touched: [sub-kernel-devdev, sub-netd-server, sub-kernel-content, sub-kernel-dev, sub-libtapestry, sub-utopia-eval, sub-stratum-session]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-08-16
---
The seven remaining stale surfaces, none more than about forty lines of churn,
swept together. **Stale reaches zero.** Three of the seven carry something worth
more than their size.

## The rule that its first legitimate exception breaks

The environment's flat-block reader was written for the introspection device,
which resolves an arbitrary process under the table lock and is therefore
**cross-process**. The reader checks no identity itself, so the rule attached to
it read: *do not add a second caller without carrying the gate.*

A second caller arrived — the exec path projecting the environment onto a new
image's stack — and it carries **no gate**, correctly. It projects a process's
own environment onto its own new stack, reaching nothing that process could not
reach by reading the device itself. **The gate exists for the case where reader
and owner differ**, which is the introspection device's case and not exec's.

**A rule stated as a mechanism is violated correctly by its first legitimate
exception.** "Carry the gate" names a remedy; the property is "reader and owner
may differ". Stated as the remedy, the correct exception looks like a breach —
and the usual outcome is not that the rule gets fixed, it is that the rule
quietly stops being cited, because it was wrong once in front of someone.

The rewritten form is **stronger**, not looser: a new caller must be
same-process (no gate, and say why) or cross-process (carry the gate), and what
is forbidden is one that argues neither. The original permitted silence in the
same-process case by not imagining it.

## "Same shape again" was written in the comment that introduced the defect

The GPU seam's `/dev` stub was added exactly like its siblings — empty
pre-mount, with init expected to replace-mount the server's tree over it — and
the comment said so: *same shape again.*

An audit caught that the shape does not transfer. **A shared mount is one
server-side connection, and that tree's authority is per-connection**, so a
global mount would have let any process drive any other's rendering context. The
disposition inverted: the stub stays empty, and a client that wants the tree
mounts it itself, in its own namespace, getting its own connection.

Every previous stub's authority was per-**file**, where one shared connection is
fine. This one's is per-**connection**, where sharing is the entire hazard. **A
stub-shaped surface says nothing about the authority model behind it**, and the
mount pattern is a statement about that model — so the pattern is not
transferable by resemblance.

The phrase in the comment is the tell, and it is the kind that only reads as one
afterwards.

## The same optimisation, sound in one layer and unsound in the next

A socket's descriptor names an ordinary remote file, which the filesystem device
reports as **always ready** — right for a file, useless for a socket. Readiness
lives on a sibling, so the Linux-phenotype poll translator substitutes
descriptors: open the sibling, poll that, put the caller's number back.

The ported-libc boundary **caches** that readiness descriptor. The kernel
translator opens it **per call**. Same code shape, opposite verdicts, and the
discriminator is not visible at the call site: caching in the kernel translator
would place a descriptor the guest never asked for into **the guest's own number
space**, where the guest can close it — leaving a cached number that names
whatever was allocated next — and where it breaks the lowest-available guarantee.
In the ported libc the hazard is absent because there the readiness descriptor
**is** a guest descriptor its own library opened.

**Who owns the number space** is the fact that decides it, and nothing about the
two call sites shows which one you are in.

## Four smaller ones

**An absent operation should be inexpressible by a present one.** Two device-table
slots changed their failure contract from a flat sentinel to specific errnos, and
the interesting half is the NULL meaning: an absent slot now answers
*operation-not-supported*, **distinct from any verdict an implementation can
return**. Previously "this device has no such operation" and "the operation ran
and refused" were one value, so a caller could not tell a missing capability from
a denied one.

**Ordering as the mechanism, because the symptom has no signature.** The login
program now sets the echo mask *before* writing the passphrase prompt. A sender
reacting to the prompt puts bytes on the wire in any gap, where echo is still on
from the username read — so the byte is rendered on the trusted path and then
discarded by the flip, truncating the passphrase to **a plain authentication
failure**. Indistinguishable from typing the wrong password, so it could never
have been found from a report. The username prompt above it already had the
ordering right: a correct instance and an incorrect one adjacent in one function,
which is how the wrong one survived review.

**Authority by which descriptor you hold.** The graphics client's new control
verb writes on the surface's *own* control descriptor, so it rides the owning
connection by construction — no other connection can resolve that surface, so no
addressing argument is needed. The per-opener session is what makes that
load-bearing rather than tidy.

**A closed allowlist with per-entry reasons.** The shell's raw-mode set is a
fixed list; a new full-screen program gets the line discipline until someone
edits and tests. The per-entry justifications differ — the pseudoterminal host
wants a raw outer console *because the terminal it hosts is the one line
discipline*, the rest are renderers — so "it looks like a full-screen program" is
not the criterion, and compressing the reasons away would make it one.
