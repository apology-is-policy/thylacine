---
id: chg-2026-08-04-identity-transport-crypto-sweep
type: chg
title: "Identity and transport crypto — one deleted cap, two dead premises, and a backstop on the wrong loop"
date: 2026-08-04
arc: arc-vault
commits: []
touched:
  - sub-corvus
  - sub-corvus-crypto
  - sub-tls
  - inv-i23
  - moc-userspace
  - moc-userspace-runtime
established:
  - sub-corvus
  - sub-corvus-crypto
  - sub-tls
  - inv-i23
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-04
---
Batch 51: the key agent and the two cryptographic libraries — 6495 lines
across four files, three dossiers, and one invariant minted.

**THE AREA QUESTION, ANSWERED AGAINST THE CODE RATHER THAN THE SHAPE.**
The handoff asked whether `services` covers corvus and its crypto library
together, or whether they split the way libdriver did. Neither guess was
right. corvus is unambiguously a service — a real 9P server posting
`/srv/corvus`, with a Conn/fid table, a qid scheme, and the same
single-threaded dispatch loop [[sub-ptyfs]], [[sub-tapestryd]] and
[[sub-netd-server]] run. But the split is not grant-core/discovery, it is a
**plane** split: the daemon is a privilege boundary and the two libraries
are not, so the libraries go to `runtime` where the organizing fact
already says exactly that.

And the reading inverts on one fact from corvus's own imports: **the
shared 9P codec the other three servers link was lifted out of corvus's
private module.** There are four native 9P servers, not three, and the
fourth is the ancestor. That changes what "the template's fixes did not
all travel" means — a fix that reached the library reached the
descendants, and a fix that reached only a descendant never came back
here. [[moc-userspace]]'s cross-cutting note is corrected to say so.

**F1 — THE ADEQUACY ARGUMENT NAMES A KERNEL CAP THAT WAS DELETED (task
#148).** corvus keeps a single global session slot, and the comment
justifying it says one slot is adequate because "the kernel cap
SRV_CONN_PER_PROC_MAX=1 means at most one live connection per peer."
That cap does not exist — the namespace-resident service registry removed
it, the design document that did so says "**removed**" in bold, and a grep
of the kernel returns nothing. The live bound is a global 64; corvus
itself sizes for eight simultaneous connections.

The premise is contradicted twenty lines below its own statement. The
connection-ownership field, added later, exists precisely because several
connections coexist — its comment describes a second process presenting a
bearer token that must not wipe a live login session by disconnecting.

The design is still safe, and that is the part worth keeping: what holds
it together is **AUTH's refusal while a session is bound**, not the
deleted cap. So the real property is not "one session because at most one
connection" but "one session, therefore **one user at a time**" — a second
login is refused until the first closes. Nobody wrote that down. Eight
further sites across the source, the specifications and the reference docs
still name the deleted cap.

**F2 — THE SAME DELETION MADE A SECOND BRANCH LIVE, AND ITS COMMENT CALLS
IT BENIGN (task #149).** When the connection table is full corvus declines
to accept and comments: "Backlog full; defer accept ... the backlog will
drain as conns close." But the listener's readiness is level-triggered on
a non-empty backlog, so an infinite-timeout poll returns immediately every
iteration, no work is done, and the identity daemon spins a processor
until a slot frees.

**One deletion, two dead premises.** While the per-process cap existed,
"more than one connection" was impossible — so the single global session
was inarguable *and* "backlog full" was unreachable. Removing the cap made
both live and neither comment was revisited. This is the batch's shape:
not a wrong bound, a **correct bound whose reason expired**.

**F3 — THE HEADER DESCRIBES A DAEMON FOUR ARCS YOUNGER (task #150).**
corvus's file header is an append-only build log. It says storage is
in-memory only and that filesystem persistence "lands once that tree is
mounted" — persistence landed, and three of its loaders are *boot-fatal*.
It marks the accept-time peer snapshot dead code awaiting the
administrative verbs — those verbs landed, they **deliberately refuse to
read it** (a live capability query per call, because a cached one is stale
in both directions), and a different handler reads it anyway to report
file ownership. So the annotation says "wait for the verb" where the
design says "the verb must never touch this." And the daemon prints its
construction sub-chunk in the boot banner on **every boot** — the
staleness is not merely internal, it is announced.

**F4 — THE STALL BACKSTOP IS ON THE ONE LOOP THAT CANNOT STALL (task
#151).** [[sub-tls]]'s in-memory round-trip helper shuttles records between
two local connections and caps itself at sixty-four iterations, with a
comment naming the risk: no peer to wait on, so if nothing flows we are
stuck. Both its peers are under its own control. The handshake loop and
the plaintext read loop — the two that talk to a *remote* peer — have no
cap, no deadline and no timeout, and exit only on established, closed, or
peer end-of-file. A peer that dribbles enough to keep the socket readable
holds a connect open indefinitely, and `curl` and the HTTPS client call it
against arbitrary hosts.

**WHAT WAS SOUND, AND IS WORTH THE SAME SPACE.** [[sub-corvus-crypto]] is
the most disciplined code this sweep has read. Every failure path wipes,
each for a stated reason — including the one where an authenticated
decryption leaves *real plaintext* in the buffer at the moment the tag
check fails. The on-disk cost parameters are bounded before the
key-derivation function sees them, because a tampered header is otherwise
an input that can wedge or exhaust a single-threaded daemon: **a file
format defending against itself**. And the 2048-word recovery list is
proved strictly ascending by a compile-time function, because the lookup
is a binary search and a mis-sorted edit would not crash — it would
silently resolve a wrong word, and a wrong word is a wrong key. That is
the failure the crate could most plausibly have shipped, and the one it
made impossible.

corvus's own verified-sound set: the frame accumulator is capped and
resets rather than latching; the unwrap gate checks session-user against
dataset-owner; the boot sequence is fail-closed at every step and *proves*
its confinement rather than assuming it; the startup order (post before
chroot) is backwards-looking and correct, because the chroot displaces the
namespace the post needs.

**[[inv-i23]] IS MINTED HERE**, because corvus is its worked example and
the sweep had reached its first enforcer. Its unusual property is worth
the note: the invariant is **cooperative** — the service chroots itself,
the kernel does not compel it — so it holds for a service that takes up
its endowment and silently fails for one that forgets. corvus verifies its
own confinement at boot, which is meaningfully stronger than intending it.

**THE MERGE DUTY RAN BOTH HALVES, WHICH IS THE POINT.** Main's arriving
commit added a graphics API surface and a probe; nothing on the Present
plane claims anything about it, so no falsehood. But the **ledger moved** —
a new source file and a grown header, +1 file and +105 unswept lines — and
this batch read its baseline *after* the merge rather than carrying
forward the last batch's closing numbers. That is batch 50's correction
applied on its first opportunity: a sweep's line delta is a statement
about the whole tree, and it equals the batch's own work only when nothing
else changed.

LEDGER, read off the rendered view. Corpus 859 -> **864**. Coverage 277 ->
**281 owned of 422**, 65% -> **66%**; unswept lines 37397 -> **30902**.

**AND THE RULE EARNED ITSELF A FOURTH TIME — on pure arithmetic this
time, which is the more useful lesson.** Both numbers were written down
before rendering and both were wrong, but neither was a wrong *model* of
the census:

- **Files: predicted +2, actual +4.** The four are exactly the four
  source files the batch was scoped around; the manifests listed
  alongside them for provenance do not count, which is what batch 50
  learned. I applied that correctly and then miscounted my own file list,
  forgetting that the recovery wordlist is a second source file.
- **Lines: predicted 32902, actual 30902.** A subtraction slip. The
  delta is *exactly* the 6495 lines swept — clean for the first time in
  three batches, because the baseline was read **after** the merge rather
  than carried forward from the last batch's close. Batch 50's correction
  paid off on its first opportunity.

Four batches, four different reasons the prediction was wrong: a wrong
denominator, a wrong unit, an unaccounted merge, and now plain
arithmetic. The rule is not "understand the census well enough to
predict it" — it is **read it, never predict it**, and the reason is that
the ways to be wrong do not converge.
