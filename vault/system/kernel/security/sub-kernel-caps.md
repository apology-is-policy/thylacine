---
id: sub-kernel-caps
type: sub
parent: moc-kernel-security
title: "Capabilities — the fork-grantable ceiling, the cap device, and the legate"
code:
  - kernel/include/thylacine/caps.h
  - kernel/devcap.c
  - kernel/include/thylacine/devcap.h
  - kernel/proc.c
  - kernel/test/test_devcap.c
audit: hard
guarded-by: [inv-i2, inv-i25]
validated-by: [prose, spec-imperium, gate-smp]
locks: []
abis: []
design: ["docs/CORVUS-DESIGN.md section 5.5", "docs/IDENTITY-DESIGN.md section 9.8", "specs/corvus.tla", "specs/handles.tla"]
created: 2026-08-02
updated: 2026-09-24
---
## Graphical grant commit

A graphical grant is inserted with `seat_held` atomically under the grant
lock. Redemption refuses it without consuming it, even when the requester polls
`/use` before Corvus replies. Only the seat's successful RESTORED transition
releases the exact target stripes/session. Failure cancels the matching held
entry and zeroes the seat's record of it, so the RESTORED that later recovers a
failed seat finds nothing to release (`cons.graphical_seat_grant_and_failure`
drives both orders with a fresh requester). Both paths run in process-lock then grant-lock order; redemption takes
only the grant lock, so the barrier adds no inverse edge. Serial grants retain
their existing immediate redemption semantics. The grant bounds test covers early
redeem, mismatched releases, cancellation, one-shot release and successful redeem.
See [[sub-lictor]] for the physical restoration contract.

## Purpose

A capability is an unforgeable per-Proc bit gating a class of privileged
operation: creating hardware handles, overriding filesystem permissions,
killing across identities, emitting executable code. `Proc.caps` is the
whole of a Proc's *what may I do* authority — deliberately separate from
*who am I* ([[sub-kernel-perm]]).

The governing rule is that capabilities only ever **reduce** by inheritance.
The single sanctioned path by which a Proc gains one is the `cap` device,
and everything in this dossier exists to make that the only path.

## Contract

**`CAP_TCB_DIAL` + a real coverage assert (U, 2026-09-23).** Bit 14, the
fifteenth capability, gating open=connect on a TCB **byte** service in `/srv`
([[sub-kernel-devsrv]], STALK-DESIGN 5.2 / D8). **FORK-GRANTABLE** -- a member
of `CAP_ALL`, not of `CAP_ELEVATION_ONLY` -- because it flows down the vetted
boot chain exactly as `CAP_SET_IDENTITY` does: kproc -> joey -> `/sbin/login` ->
the per-user home proxy login spawns. That proxy runs as the USER, so the
authority to dial cannot be an identity check; a clearance would be the wrong
shape too, since the proxy is SPAWNED rather than elevated.

The same chunk replaced the `CAP_ALL` assert, which compared the macro against
its own definition token for token (`X == X`) and could not fail, with a real
COVERAGE assert `(CAP_ALL | CAP_ELEVATION_ONLY) == CAP_DEFINED`. Its two sides
are independent lists, so omitting a new bit from either one now fails the
build. Task #35; the full account, including the sabotage that proves the new
guard fires where the old one did not, is in [[abi-caps]].

**A capability is not a boundary on its own -- the holder must also be
unpuppetable ((U) F1, 2026-09-23).** The header comment beside `CAP_TCB_DIAL`
used to say the hole in this reasoning was open; it is now closed, and the fix
lives nowhere near this file. Because the home proxy runs AS the user, the user's
own shell is the same principal, and [[inv-i39]]'s debug gate admitted an OWNER
outright when this was written -- so the shell could debug-attach the proxy and
drive its transport without ever holding bit 14. **TWO independent answers close
it now**, and the audit of 2026-09-24 caught this paragraph naming only one:
`SPAWN_PERM_SEAL` on the proxy's spawn ([[sub-kernel-syscall-dispatch]],
[[sub-stratum-session]]), and the capability-cover rule on the owner axis
([[sub-kernel-devproc]], DEBUG-FS-DESIGN 3.1), which refuses the attach on
authority because the shell lacks exactly bit 14. The seal is kept as the second
answer, not made redundant: it is the one that still holds between peers of EQUAL
authority, and a caps-0 NATIVE fork of the proxy is covered by every
same-principal peer while still holding the parent's handles.

The transferable lesson, and the reason it is recorded HERE rather than only at
the fix: **granting a capability to a process that shares a principal with an
attacker grants it to the attacker, unless something separately stops the
attacker from driving that process.** A capability answers "who may act"; it says
nothing about who may act THROUGH the actor. Any future fork-grantable bit handed
to a service that runs as a user inherits this whole problem, and the checklist
is two items, not one: give it the bit, and make it untraceable.

**And cover does not retire that second item -- it cannot see most of what is
worth stealing.** The cover rule is a subset test over the `caps` word alone,
while authority also lives in the `proc_flags` spawn perms
(`PROC_FLAG_MAY_POST_SERVICE`, `SESSION_HANGUP`), the [[inv-i34]] hardware
allowance, and the handle table. So a peer with EQUAL caps and FEWER perms
covers, and debugging hands it the perms. Read the checklist as: give it the
bit, and make it untraceable **whenever the bit is not a capability either** --
a `SPAWN_PERM_*` granted to a Proc that runs as a user needs `SPAWN_PERM_SEAL`
in the same `.perm()` call, because cover is blind to the thing just granted.
Live instance, self-found at the 2026-09-24 close and **since CLOSED**: the Halcyon
session compositor held `MAY_POST_SERVICE` with the shell's own cap mask and no seal,
so any tile program covered it exactly. It is sealed now.

**And the sweeping form of this rule did not survive its own audit.** The draft said a
`SPAWN_PERM_*` granted to a user-running Proc *must* carry `SEAL`, and claimed both of
login's spawn sites obeyed. There are **three**: the session shell (`main.rs:1341`,
`CONSOLE_OWNER | SESSION_HANGUP`) is deliberately unsealed, because neither bit is
onward-conferrable by `ut` and a same-principal peer can already end the session by
killing it, so sealing would make the user's own shell undebuggable and buy nothing.
The durable form is a QUESTION, not a rule -- *would puppeting the holder give that peer
authority it cannot otherwise obtain?* -- with all three answers recorded at
`CAP_TCB_DIAL` in `caps.h`. The instructive part is the failure mode: a rule generalised
from two examples, with its census taken from memory instead of from a grep, was false
on the day it landed.

Round 2 added a third item: **make the seal visible before the identity that would
admit an attacker.** It was first discharged by load order -- `proc_apply_identity`
publishing `principal_id` with RELEASE so an ACQUIRE reader of the new identity also saw
the stamp -- and the seal's own round-2 re-audit (2026-09-24) showed that held only for a
reader admitted BECAUSE it saw the new identity: not for a spawn that changes none, not
for a `CAP_HOSTOWNER` reader. It is now discharged by a lock: `proc_seal` stamps under
`g_proc_table_lock`, which every `/proc` reader holds -- see [[sub-kernel-proc]] and
[[sub-kernel-devproc]]. Granting the bit, sealing the holder and ordering the two are
still one obligation, not three; the third is now held by construction.


**Propagating Imperium and Haul (2026-09-17).** `CAP_GRANTABLE_IMPERIUM`
is DAC_OVERRIDE | CHOWN | KILL | POST_SERVICE. The last bit is 13 (12 remains
reserved for audio); it is absent from CAP_ALL, present in CAP_ELEVATION_ONLY
and CAP_GRANTABLE_CLEARANCE. `sys_cap_grant_imperium_core` accepts only the
propagating flag and this subset. `proc_become_legate` rejects nested
propagating redemption. During `rfork_internal`, only the parent's scoped
`legate_caps` can survive the elevation strip, intersected with actual parent
caps and the requested child mask. Ordinary clearance still does not flow.
The scope tag is published last with release ordering. Child insertion checks
parent teardown under the process-table lock, closing the fork/sweep gap.
The trusted authorization and user interface are described in [[sub-imperium]].


Two disjoint classes, pinned by `_Static_assert`:

- **`CAP_ALL`** — the fork-grantable ceiling, what kproc holds at
  `proc_init` and what may flow parent → child. Six bits: `HW_CREATE`,
  `LOCK_PAGES`, `CSPRNG_READ`, `GRANT_HOSTOWNER`, `SET_IDENTITY`,
  `GRANT_CLEARANCE`.
- **`CAP_ELEVATION_ONLY`** — held by no Proc at creation and stripped from
  every child (except a PROPAGATING legate scope's own `legate_caps`):
  `HOSTOWNER`, `DAC_OVERRIDE`, `CHOWN`, `KILL`, `DEBUG`, `JIT`,
  `AUDIO_GRAPH` (bit 12) and `POST_SERVICE` (bit 13). The macro in `caps.h`
  is the authority for this list; a prose COUNT of it has been wrong four
  times (four, five, six, seven -- in `caps.h`, CLAUDE.md and ARCH section 28
  at once, 2026-09-21), so none is given here.

`(CAP_ALL & CAP_ELEVATION_ONLY) == 0` is asserted, so every bit is
fork-grantable **xor** elevation-only, never both. `CAP_ALL` is itself
asserted against its own expansion, which forces a deliberate decision when
a new bit is added rather than letting it default into kproc's mask.

`rfork` computes `(parent_caps & caps_mask) & ~CAP_ELEVATION_ONLY`. The Linux
`clone` routes through the `rfork_forked_with_caps` variant with `caps_mask =
CAP_ALL` — a clone carries no caps argument — so a Linux-phenotype child inherits
the parent's whole fork-grantable set, and the `& ~CAP_ELEVATION_ONLY` strip
still applies, so I-2's monotonic reduction holds on the phenotype path exactly
as on the native one.

## Mechanism

**The strip is unconditional, and that is the point.** `caps_mask` alone
cannot enforce non-leakage, because the mask is supplied by the caller and a
caller may pass one that includes an elevated bit. So the `& ~CAP_ELEVATION_ONLY`
is applied regardless. An elevated parent — one that legitimately redeemed
`CAP_HOSTOWNER` through the console-gated device — cannot leak it across a
fork even by asking to.

The parent's caps are read under an **acquire** load, because the child's
ceiling is bounded by what the parent observably holds *now* and
`proc_become_legate` is a cross-thread writer.

**The grant is two-phase, and the two phases are gated on different
parties.** The `cap` device (`dc='k'`) exposes two write-only files:

- `/grant` — the *authority to register*. Gated on `CAP_GRANT_HOSTOWNER` or
  `CAP_GRANT_CLEARANCE`, both of which are ordinary fork-grantable bits
  conferred on corvus alone.
- `/use` — the *redemption*. Gated on the target's own properties.

The split keeps "who may register a grant" (corvus) strictly distinct from
"who has been elevated" (a console session). Its defense-in-depth argument
is explicit: corvus verifies the system passphrase, which the kernel has no
notion of; the kernel verifies console attachment, which holds **even if
corvus is buggy or compromised**. A compromised corvus can register grants
for arbitrary stripes, but only a console-attached writer can redeem one, so
corvus elevating a network process is structurally impossible.

**`/grant` is length-discriminated, and the two kinds are carried on the
entry rather than inferred later.** A 16-byte write is a hostowner grant; a
32-byte write is an A-4 clearance grant. The `kind` rides on the table entry
so a *single* locked lookup at redeem reads it atomically — there is no
peek-then-redeem window in which a concurrent re-register could flip the
kind. Every register routes through one `cap_set_entry_locked` that writes
**all** fields, so a re-register over a slot of the other kind cannot leave
a stale discriminator behind.

**The two redeem paths differ in gate and in matching rule, deliberately.**

| | hostowner | clearance |
|---|---|---|
| console-attached required | **yes** | no — auth was corvus-side |
| requested vs granted mask | must be **equal** | must be a **subset** |
| effect | OR into `caps` | `proc_become_legate` |

Equality for hostowner keeps the protocol explicit: a `/use` asking for a
different cap is a bug or an attack, not a negotiation. Subset for clearance
is *self-restriction* — the Proc voluntarily narrows below its ceiling,
which is I-2's shape. In both cases a failed gate does **not** consume the
grant, so the legitimate holder can still redeem.

**The legate is a scope, not an identity change.** `proc_become_legate` ORs
the caps atomically, leaves `principal_id` untouched, allocates a monotonic
`scope_id`, and marks the Proc `LEGATE_ROOT`. The root flag is never
inherited (no `proc_flags` are), so an `rfork` child is a scope *member*,
carrying the tag but not the flag — and the members' authority is already
gone, because the caps in question are elevation-only and were stripped.

That is what makes the scope guarantee tractable: **I-25's privilege
property rests on the root alone.** A member never holds the elevated caps,
so a straggler the teardown sweep misses is an unelevated Proc with a stale
tag — untidy, not a violation. The teardown walk is a tidiness sweep; the
root dies on its own exit or self-terminates at `valid_until`.

**The arm-6 session hangup is this teardown's structural sibling, and is *not* a
cap.** `proc_session_hangup_if_leader` ([[sub-kernel-death]]) rides the same
ZOMBIE chokepoint and the same held-lock `proc_group_terminate` walk, but it
keys on the session `sid` (not the legate `scope_id`) and terminates *processes*
for session-lifecycle reclamation at logout — it confers no authority, so it is
not I-2/I-25 territory and [[inv-i26]] is untouched (login drives it without
CAP_KILL precisely because the *kernel* does the termination). The contrast is
instructive: the legate keys on a **dedicated monotonic** `scope_id`, so its
member-match is alias-free by construction; the hangup keys on `sid ==
leader-pid`, alias-free only because pids never recycle — a dependency the
mirror silently swapped, now recorded at `session_hangup_cb` (arm-6 audit F1).

**The clearance window opens at redeem, not at grant.** `valid_until` is
computed as `now + valid_for` *when the caps land*, so a slow redeem does not
shorten the window and no userspace/kernel clock-domain agreement is needed.
The addition saturates: a `valid_for` large enough to wrap clamps to `~0`
rather than wrapping to a small deadline — or, worse, to exactly `0`, which
is the sentinel meaning *no time bound*. The clamp removes that alias so a
bounded request can never degrade into an unbounded window.

## Data structures

`caps_t` is a `u64`. `struct cap_grant_entry` carries state, kind, cap_mask,
target_stripes, the redemption-window `expiry_ns`, the clearance
`valid_for_ns` *duration*, and a corvus audit `session_id`. The two time
fields are different things and the comments are careful about it: one
bounds how long the grant may sit unredeemed, the other how long the
resulting legate lives.

The table is a fixed `CAP_GRANT_MAX` array in BSS — zero-initialized, and
`CAP_GRANT_FREE == 0` makes that meaningful.

Each walked leaf Spoor carries a kmalloc'd aux holding **only** a magic
identifying which file it is; the magic alone discriminates `/grant` from
`/use` at write time.

## Concurrency

One irqsave spinlock over the grant table. Expiry is folded into the
free-slot scan rather than run as a separate sweep, so a stale grant is
reclaimed by the next registration that needs a slot.

Redeem does exactly one locked lookup, then releases the lock **before**
mutating `writer->caps`. That release is why the OR must be atomic: once the
slot is freed, corvus may re-register for the same stripes and a sibling
thread may redeem concurrently, so a plain read-modify-write would clobber
the sibling's OR and silently lose a capability. This was hardened as RW-5
F1.

## Invariants enforced

- **I-2** (fork-grantable caps monotonically reduce; elevation-only stripped
  at every fork) — the `rfork` expression is the whole enforcement.
- **I-25** (legate authority scope-bounded and fully revoked) — via the
  root-only property above.
- Feeds [[inv-i22]]: capabilities are the *only* growth path, so no identity
  carries ambient authority.

Not yet minted as registry notes; this sweep is what unblocks them.

## Error paths

Everything fails closed with `-1`. A grant whose mask escapes `CAP_GRANTABLE`
(or `CAP_GRANTABLE_CLEARANCE`) is refused; `target_stripes == 0` is the
fail-closed sentinel; a zero or over-`u32` `session_id` is refused; a full
table refuses rather than evicting a live grant. `/grant` and `/use` are
write-only — reads return `-1`, as does any non-frame write length.

`cap_proc_exit_notify` clears pending grants for a dying Proc's stripes, so
a grant cannot outlive its intended target and be redeemed by a stripe
collision later.

An unknown leaf magic extincts: it means corruption, not a bad argument.

## Performance

Linear scans of a small fixed table under a spinlock, on paths taken a
handful of times per boot. Not a hot surface.

## Prosecution

- A new capability bit must be added to `CAP_ALL` **or** to
  `CAP_ELEVATION_ONLY`, never both and never neither. Since (U) BOTH halves
  are build-enforced -- disjointness forbids a bit in both classes, coverage
  forbids one in neither. Prosecute that the coverage assert's two sides stay
  INDEPENDENT lists: rewriting either in terms of the other restores the
  tautology that was #35.
- Any new register path must write **every** entry field, or a re-register
  across kinds leaves a stale discriminator.
- The redeem must keep reading `kind` inside the same locked lookup that
  finds the entry — splitting them reopens a TOCTOU on the kind.
- The hostowner arm's console gate must stay *after* the lookup, so it can
  only ever see a hostowner grant.
- A failed gate must not consume the grant.
- Any future cap mutation must be atomic on `p->caps`; it has a cross-thread
  writer.

## Seams

[[seam-devcap-plain-caps-read]] — the two `/grant` register gates still read
`writer->caps` with a plain load, the last two stragglers of a sweep that
converted every other capability gate in the tree.

`caps.h` records a forward-looking obligation for the day a cap-drop syscall
lands: it must refuse with `-EBUSY` if dropping `CAP_HW_CREATE` would leave
the Proc holding hardware handles, or the implementation would admit states
`specs/handles.tla`'s `HwHandleImpliesCap` forbids. No such syscall exists,
so the invariant holds trivially today.

## Caveats

- **`caps.h` corrects its own scripture, and the correction is load-bearing.**
  Three documents (`JIT-ON-WX-DESIGN.md`, `LLVM-DESIGN.md` §8, ARCH §28
  I-42) describe `CAP_JIT` as "elevation-only, non-rfork-grantable, the
  `CAP_HW_CREATE` class". Read literally the trailing phrase is **wrong**:
  `CAP_HW_CREATE` is fork-grantable, which would contradict both
  "non-rfork-grantable" beside it and I-42's own "non-heritable" clause. The
  header says in as many words: do not "fix" this bit toward `CAP_ALL` on
  the strength of that phrase.
- **The comment drift this dossier once flagged is now fixed** (`830817c4`).
  Both enumerations had lagged their macros — the `CAP_ELEVATION_ONLY` comment
  said "All five" and then listed six; the `CAP_ALL` comment enumerated four
  elevation-only caps and omitted `DEBUG` and `JIT`. Both now read "All six" and
  enumerate all six correctly. The lesson survives the fix and is worth keeping:
  the **macros were correct throughout** and the asserts pinned them — only the
  prose had to catch up, because a `_Static_assert` can pin an expression but
  nothing pins a sentence.
- The reserved-bit block lists `CAP_SIGNAL_ANY` as a future bit, then notes
  it was realized as `CAP_KILL`. Next free bit is `1<<12`.

## Provenance

[[chg-2026-08-02-authority-sweep]].

[[chg-2026-08-15-stale-by-cotenancy]] re-verified this dossier without changing it.
`kernel/proc.c` moved ~910 lines in the interval, all of it the fork/exec arc;
a word-bounded diff over every capability token across the whole interval found
**one comment line and two call sites gaining a trailing parameter**, with
`CAP_NONE` unchanged at each. The staleness was borrowed from a co-tenant
surface, not earned. See the note on what that means for the churn ordering.

**2026-08-16: flagged again, same answer.** A further ~17 lines of `proc.c`, all
of it the exec-path disposition reset and its audit follow-up. No capability
token, mask or gate appears in any hunk. This is the second consecutive interval
in which this dossier's staleness was **borrowed from a co-tenant surface rather
than earned**, which is worth stating twice: a churn signal keyed to files
cannot distinguish the two, and a dossier repeatedly flagged for someone else's
work is one a reader learns to skip.

**2026-08-26: the first EARNED interval, and the reason the "learns to skip"
warning is double-edged.** `830817c4` (phenotype-fork-inherits-caps) finally
touched capability surface for real: `caps.h`'s `CAP_ELEVATION_ONLY` and
`CAP_ALL` comments were corrected to enumerate all six elevation-only caps (the
drift the Caveats once carried), and `proc.c` gained `rfork_forked_with_caps` —
the Linux `clone`'s rfork, `caps_mask = CAP_ALL`. After two borrowed flags a
reader had every reason to skip the third; this one was the dossier's own. Folded
at [[chg-2026-09-05-caps-fork-inherit]].
