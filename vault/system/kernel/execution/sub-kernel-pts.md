---
id: sub-kernel-pts
type: sub
title: "The pts registry — kernel-side terminal identity, and the seam that gives a server signal power without giving it a target"
parent: moc-kernel-execution
code: [kernel/pts.c, kernel/include/thylacine/pts.h]
audit: hard
guarded-by: [inv-i20, inv-i1, inv-i22, inv-i9]
validated-by: [spec-pty, spec-pty-stop, prose, gate-smp]
locks: [lock-pts]
hazards: []
abis: []
design: ["docs/PTY-DESIGN.md section 3"]
created: 2026-08-03
updated: 2026-08-03
---
## Purpose

A pseudoterminal is served entirely in userspace by [[sub-ptyfs]] — the rings,
the line discipline, the character transforms. But two things about a terminal
cannot live in a server: **who controls it**, and **who gets the signal**. Put
those in the server and a compromised or merely buggy terminal emulator can
signal arbitrary process groups.

So the kernel keeps the smallest possible thing that makes the seam sound: a
table mapping *(connection, qid)* pairs to a **pts identity**, and on each
identity the controlling session and its foreground process group. The server
holds an opaque id and can say *"a suspend character arrived on the pts I
serve."* It cannot say to whom.

That asymmetry is the file's entire reason to exist. Everything else here —
the generation counter, the pointer-identity correlation, the staged unrefs —
is machinery in service of it.

## Contract

| Function | Contract |
|---|---|
| `pts_mint(server, cn, master_qid)` | register a new pts, bind the master side, return a generation-stamped id `> 0` |
| `pts_bind_slave(server, cn, slave_qid, id)` | add a slave-side binding; idempotent for an identical re-bind |
| `pts_free(server, id)` | drop every binding, bump the generation, fan carrier loss |
| `pts_resolve_conn_qid(cn, qid, is_master_out)` | *(connection, qid)* → live id, or `-T_E_NOENT` |
| `pts_spoor_conn_qid(sp, cn_out, qid_out)` | an fd's Spoor → its *(connection, qid)* pair |
| `pts_resolve_spoor(sp, is_master_out)` | the two composed |
| `pts_tty_signal(server, id, class)` | the server's **sole** signal authority; returns the affected count |
| `pts_tty_acquire(p, cn, qid)` | the POSIX controlling-terminal acquisition |
| `pts_tty_set_fg` / `get_fg` | `tcsetpgrp` / `tcgetpgrp` |
| `pts_tty_cont(p, cn, qid, pgid)` | the shell's `fg`/`bg` resume |

The three mutating registry calls are gated on the **minting server's pid**;
the four terminal calls are gated on the caller's **session membership**. Two
different authorities for two different questions — which is the seam.

## Mechanism

### Naming a terminal without a handle

The design called this `KObj_Pts`, and it is not one. There is no handle
kind, no dup, no transfer, no leak surface — because there is nothing for
userspace to hold. A pts is named in exactly two ways:

- The **server** holds an integer the kernel gave it at mint.
- **Everyone else** names it implicitly, by holding a slave or master fd; the
  kernel resolves the fd to a pts and never tells the caller its id.

That is the grant-is-the-share shape from the network dataplane reused for
terminals: the capability *is* the fd you already hold, so the id never needs
to be unforgeable in userspace, and the narrowing was flagged at signoff
rather than drifted into.

### The correlation key is a pointer

Resolving an fd to a pts has to answer: is *this* fd one of the ends of *that*
pty? The answer is pointer identity on the service connection.

Both endpoints of one connection — the server's accepted connection and the
client mount's transport wrapper — are the same `struct SrvConn`. So an fd
resolves to a pts by walking: Spoor → 9P client → transport downcast →
connection pointer, then matching that pointer plus the Spoor's qid against
the bindings.

Two properties make the pointer safe to use this way:

**Each binding holds a reference on the connection.** So a registered pointer
cannot be freed and its address reused while bound — no ABA.

**The resolve never dereferences the candidate.** It only compares. A stale
or wild pointer therefore fails closed rather than faulting; the header states
this as a contract on the returned pointer, and the extraction helper hands
one out on exactly that understanding.

The downcast returns NULL for a loopback or Spoor-backed client, and those
sessions can carry no pts at all — so the fail-closed answer is also the
*correct* answer, not merely a safe one.

### The generation

An id is `(generation << 16) | index`. Free bumps the generation before the
index becomes reusable, so an id held across a free and re-mint fails every
later lookup instead of resolving to the new occupant. Generation zero means
*virgin slot* and is rejected, so a zero id can never validate.

This is the slot-generation discipline the network stack arrived at the hard
way, applied here before the equivalent bug could happen.

### The signal seam

`pts_tty_signal` is the whole design compressed into one function. It takes
`(id, class)` — five classes, no target. It validates the caller is the
minting server, snapshots the controlling session and foreground group,
releases the lock, and fans out on the snapshot.

Two classes are special:

- **Suspend** routes to the job-control stop fan rather than to a note post.
  See [[sub-kernel-jobctl]] — whether it stops or merely notifies is decided
  per member, at post time.
- **Hangup** additionally reaches the controlling process by pid when the
  session leader sits outside the foreground group, which is POSIX's second
  carrier-loss target.

With no controlling session, or no foreground group seated, every class
returns zero. **A terminal nobody controls routes nowhere** — which is the
correct default for a seam whose failure mode would be signalling the wrong
group.

### Acquisition, and the anti-steal guard

Acquisition is the POSIX dance with one added clause. The caller must be a
session leader; the binding must be a **slave** one (acquisition happens at
slave open — a master-side fd is refused); and the pts must not already be
another session's terminal.

That last clause is deliberate: a second open by the same session **inherits**
and returns success, a different session is refused, and explicit stealing is
unbuilt and fail-closed. The reverse direction is checked too — one session
holds at most one controlling terminal, enforced by scanning the table for an
existing claim by this session.

The scan is the only place the code reads across entries for a reason other
than lookup, and it is what makes "one session, one terminal" true rather than
merely intended.

### Teardown

Freeing a pts is carrier loss. The clear stages the connection references and
the *(session, foreground group)* pair; after the lock drops, the fan posts
hangup to the foreground group, hangup to the controlling process if it sits
outside that group, and then continue plus a job resume.

The hangup-before-continue order per member is POSIX's, and the reason is
worth keeping: the resume is what lets an *uncaught* hangup's termination
actually run, and lets a hangup-catching survivor actually handle it. A
stopped process that is hung up but never resumed is a corpse that never
finishes dying.

### The lazy reclaim

The registry has 64 slots and does not garbage-collect on a schedule. A mint
takes a free slot if one exists; only when the table is full does it look for
an entry whose bindings are *all* torn — a dead server's connections are torn
by its handle-close teardown — and reclaim exactly one.

A single live slave connection blocks reclaim even if everything else about
the entry is dead, which is conservative: the pts may still be serving through
that side.

The consequence is a real one and is documented rather than hidden: a dead
server's controlled session gets its carrier-loss hangup **at the sixty-fifth
mint**, not at its death. The interim is covered by composition — the orphan
rule handles newly-orphaned stopped groups, and the resume path keeps working
on a torn connection because the resolve is pure pointer identity.

## Data structures

Two, both file-static, both fixed-size, no allocation anywhere in the file.

`struct pts_binding` — a used flag, a master/slave discriminator, the
connection pointer with its held reference, and the qid on that connection.

`struct pts_entry` — liveness, the generation, the minting server's pid, four
binding rows, and the controlling pair. Four rows covers a master and a slave
on the shared mount with slack for per-user mounts, where a slave opened on a
second connection binds a second row.

The controlling pair lives **on the entry** and not in the server. That is the
whole security posture in one structural decision: no security state is
server-held, so no server bug can corrupt it.

## Concurrency

One spinlock, [[lock-pts]], covering everything. It is a strict leaf and the
strictness is the design — see the lock note for the staged-unref rule and the
snapshot-then-fan shape that keeps the process-table lock from nesting under
it.

Three things read process state, and each one solves the ordering differently,
which is worth seeing side by side:

| Site | Solution |
|---|---|
| the signal fan | snapshot under the lock, post after release |
| the foreground-group membership gate | check **before** taking the lock, accept a benign race |
| acquisition's liveness question | **not checked at all** — see Caveats |

The first two are correct and argued. The third is the gap.

The caller's own session id is read outside the lock in acquisition and in the
terminal calls. That is sound because a session id is self-stable: only
`setsid` mutates it, only on the caller, and a session leader cannot `setsid`
again.

## Invariants enforced

[[inv-i20]]'s third clause — *the foreground group, and only it*. The
enforcement is **structural rather than checked**: the signal call has no
process-group parameter. A server cannot escape its pts because it was never
given a way to name anything outside it.

[[inv-i1]] and [[inv-i22]] in the same stroke — the seam is those two realized
as an absent argument.

[[inv-i9]] on the stop leg, which is [[sub-kernel-jobctl]]'s to hold.

## Error paths

| Return | Cause |
|---|---|
| `-T_E_INVAL` | null argument, zero qid, malformed or stale id, bad signal class, master-side acquisition |
| `-T_E_EXIST` | a *(connection, qid)* already bound — to this pts on mint, to a *different* pts on slave bind |
| `-T_E_AGAIN` | registry full with nothing reclaimable |
| `-T_E_ACCES` | not the minting server; not in the controlling session; another session's terminal |
| `-T_E_NOMEM` | the entry's four binding rows are full |
| `-T_E_NOENT` | no binding for this *(connection, qid)*; a client not backed by a service connection |

Zero qid is rejected everywhere because it is the 9P attach-root qid — a
reserved value, not an arbitrary sentinel.

Every POSIX `EPERM` contour answers `-T_E_ACCES`, per the errno registry's
rule against the `-1` alias.

## Performance

A resolve is a linear scan of 64 entries by 4 rows — 256 pointer comparisons
worst case, under a spinlock, per terminal syscall. Nothing here is hot: these
run at open, at acquisition, at a foreground-group change, and per typed
signal character. No index exists and none is warranted.

## Prosecution

- The signal call must never grow a target parameter. The invariant is the
  absent argument.
- Every connection reference must stay balanced across mint, bind, clear, and
  the error arms — and every unref must stay staged for after the lock.
- The snapshot-then-fan shape in the signal and teardown paths is a
  correctness argument, not style.
- Acquisition's anti-steal guard and the one-terminal-per-session scan are a
  matched pair; removing either lets a session hold two terminals or steal
  one.
- The resolve must keep never dereferencing its candidate pointer.
- Generation zero must stay invalid, and free must keep bumping before reuse.

## Seams

**The v1.0 registry is 64 entries and no more.** Full plus nothing reclaimable
is a hard failure to mint.

**A hangup-surviving shell with stopped background jobs** loses the ability to
resume them once the pts entry is freed, because the resume path resolves
through the entry. Recorded in the code as needing a kill-authority continue.

**Reclaim is lazy** — a dead server's carrier-loss fan waits for table
pressure.

## Caveats

**A dead session keeps its terminal.** The controlling session is recorded as
a bare pid and is never cleared except at free. Nothing clears it when the
session leader dies. So after a leader's death the entry still reads as
controlled, and acquisition by a new session is refused with `-T_E_ACCES`
**for the life of the entry** — the anti-steal guard doing exactly the right
thing against a live session and exactly the wrong thing against a dead one,
because it cannot tell them apart.

No current consumer hits this: the one in-tree host opens a master, spawns a
single child, and exits. It becomes reachable the moment anything respawns on
a pts it already holds — the ordinary terminal-multiplexer shape. The fix is
available and cheap: check liveness *before* taking the lock and re-check
after, which is precisely what the foreground-group gate already does. Task
#67.

**POSIX hangup-on-leader-death is not implemented, and the audit's own close
narrowed the claim without marking it.** The design's motivation for having
sessions at all lists *"SIGHUP-on-leader-death"*; the audit finding that
closed the area restates it as *"session-leader death fans hangup and continue
to orphaned stopped process groups"* — which is the orphan rule, a strictly
narrower property, and the one that is built.

The gap in effect: a foreground job whose session leader dies gets nothing
from the kernel unless it is *also* stopped and newly orphaned. Carrier loss
arrives when the master closes instead of when the controlling process dies.
Task #68.

**`proc_setsid` claims a wiring that does not exist — for a case that cannot
occur.** Its comment says that once the registry exists, the call "also clears
any binding owned by the OLD session iff the caller was its leader (wired at
PTY-1d)." The registry exists, PTY-1d landed, and `setsid` touches no registry
state.

It does not need to. A session leader always has its group id pinned equal to
its pid — `setsid` sets both, and `setpgid` refuses a session-leader target —
and `setsid` refuses any caller whose group id equals its pid. **So the
caller can never be the old session's leader.** The condition is unreachable.

And the property the design actually relies on — that `setsid` drops the
controlling terminal — holds by a different mechanism entirely: every terminal
call compares against the caller's *live* session id, so changing it detaches
the caller immediately, with nothing to clear. The comment describes an
unbuilt mechanism where the real reason is a structural one. Task #69.

**Ids ignore their top sixteen bits.** The decode takes the index from bits
0–15 and the generation from bits 16–47; bits 48–63 are never examined, so an
id with garbage there validates identically to the real one. No authority
follows from it — the server-pid gate is unaffected, and the caller is already
entitled to the entry — but the header promises to reject a malformed id and
this class of malformed id is accepted.

## Provenance

[[arc-pty]], sub-chunks PTY-1c (the registry) through PTY-1f (the job-control
stop and the resume). The design is `docs/PTY-DESIGN.md` section 3; the audit
that shaped it is recorded in `memory/audit_pty_design_closed_list.md`, whose
findings F1, F2, F7, F8, F11 and F13 are each visible in the code as named
mechanisms.

## Tests

Ten tests, and between them they cover every gate in the file:
`pts.mint_bind_resolve_free`, the generation's stale-id rejection, the
cross-server authority matrix, the binding-row exhaustion, the torn-connection
reclaim, `pts.tty_acquire_matrix` (including the inherit-not-steal case and the
one-terminal-per-session refusal), `pts.tty_set_get_fg_matrix`,
`pts.tty_signal_routing` (including the hangup dual target),
`pts.tty_tstp_stop_cont_seam`, and `pts.teardown_hup_cont`.

**Not covered:** the dead-session acquisition refusal above, and the
leader-death hangup that does not exist. Both are absences, and a test suite
built from the implementation cannot see an absence.

## Referenced by

[[sub-ptyfs]] is the other half of the seam. [[sub-kernel-jobctl]] receives
the suspend and continue fans. [[sub-kernel-proc]] owns the session and group
fields this file reads. [[inv-i20]] · [[spec-pty]] · [[spec-pty-stop]].
