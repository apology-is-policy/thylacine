---
id: sub-kernel-caps
type: sub
parent: moc-kernel-security
title: "Capabilities — the fork-grantable ceiling, the cap device, and the legate"
code:
  - kernel/include/thylacine/caps.h
  - kernel/devcap.c
  - kernel/proc.c
audit: hard
guarded-by: []
validated-by: [prose, gate-smp]
locks: []
abis: []
design: ["docs/CORVUS-DESIGN.md section 5.5", "docs/IDENTITY-DESIGN.md section 9.8", "specs/corvus.tla", "specs/handles.tla"]
created: 2026-08-02
updated: 2026-09-05
---
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

Two disjoint classes, pinned by `_Static_assert`:

- **`CAP_ALL`** — the fork-grantable ceiling, what kproc holds at
  `proc_init` and what may flow parent → child. Six bits: `HW_CREATE`,
  `LOCK_PAGES`, `CSPRNG_READ`, `GRANT_HOSTOWNER`, `SET_IDENTITY`,
  `GRANT_CLEARANCE`.
- **`CAP_ELEVATION_ONLY`** — held by no Proc at creation and stripped from
  every child unconditionally: `HOSTOWNER`, `DAC_OVERRIDE`, `CHOWN`,
  `KILL`, `DEBUG`, `JIT`.

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
  `CAP_ELEVATION_ONLY`, never both and never neither; both asserts must be
  updated deliberately.
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
