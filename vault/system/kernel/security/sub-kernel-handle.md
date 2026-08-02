---
id: sub-kernel-handle
type: sub
parent: moc-kernel-security
title: "The handle table — per-Proc references and their rights"
code:
  - kernel/handle.c
  - kernel/include/thylacine/handle.h
audit: hard
guarded-by: []
validated-by: [prose, gate-smp]
locks: []
abis: []
design: ["docs/ARCHITECTURE.md section 18", "specs/handles.tla"]
created: 2026-08-02
updated: 2026-08-02
---
## Purpose

Every kernel object a Proc can name — a Burrow, a Spoor, an MMIO window, a
Loom ring, a claimed PCI function — is reached through a **handle**: a slot
in the Proc's own fixed-size table, carrying the object pointer, its kind,
and a rights bitmask. The handle index is what userspace calls a file
descriptor.

The table is the answer to "what may this Proc name at all", and the rights
word is the answer to "and what may it do with this particular reference".

## Contract

`handle_alloc(p, kind, rights, obj)` installs a reference and returns its
index, or `-1`. The caller must **already** have accounted for one reference
on `obj` (the `burrow_create_anon`-consumed-reference convention);
`handle_close` releases it.

`handle_get(p, h, out)` returns a **by-value snapshot** with the object's
refcount already bumped; `handle_put` drops that borrowed reference. The
pairing is mandatory — see Concurrency.

`handle_dup(p, h, new_rights)` installs a second reference to the same
object with `new_rights ⊆ parent->rights`.

Four kinds of rejection are structural rather than incidental: a
non-transferable kind cannot be duplicated; rights can only narrow; empty
or out-of-range rights are refused; and an out-of-enum kind extincts (the
compile-time assert defends the build, the `default:` arm defends against
memory corruption at runtime).

## Mechanism

**The four-way kind partition is the whole of I-5, expressed as a compile-time
property rather than a runtime check.** Every `kobj_kind` except
`KOBJ_INVALID` belongs to exactly one of four masks:

| Partition | Kinds | Meaning |
|---|---|---|
| `TRANSFERABLE` | Process, Thread, Burrow, Spoor | may be duplicated; the future 9P transfer path |
| `HW` | MMIO, IRQ, DMA, Interrupt, PCI | pinned to the claiming Proc — a driver cannot leak its device |
| `SRV` | Srv | pinned to the opening Proc, so the kernel-stamped peer identity behind it is unforgeable |
| `LOOM` | Loom | pinned to the Proc whose address space holds the ring |

Seven `_Static_assert`s pin this: six pairwise-disjointness checks plus one
**completeness** check that the union covers every kind but `KOBJ_INVALID`.
That last one is the load-bearing one — it is what makes "add a new kind and
forget to classify it" a build failure instead of a handle that silently
falls through every partition test. `handle_dup`'s guard is then a single
expression, `!kobj_kind_is_transferable(parent->kind)`, which is the exact
negation of the spec's `h.kobj \in TxKObjs` precondition.

**One kind is split by a magic word rather than by its enum.** A `KOBJ_SRV`
object is discriminated at release time by the `u64` at offset 0:
`SRV_SERVICE_MAGIC` (a registry entry whose lifetime is the poster Proc's,
so closing the handle must *not* touch it) versus `SRV_CONN_MAGIC` (a
refcounted connection). Anything else extincts as corruption. Post-`stalk-3c`
the connection arm is unreachable — connection endpoints became `KOBJ_SPOOR`
— but it is deliberately retained as a UAF guard.

**The acquire side is not symmetric with the release side, and the asymmetry
is load-bearing.** `handle_acquire_obj`'s `KOBJ_SRV` arm is a deliberate
no-op, balanced against a release arm that *does* work — sound only because
a `KObj_Srv` handle is now always a service listener, whose release is also
a no-op. The code says so explicitly: if a `KObj_Srv` handle ever again named
a refcounted `SrvConn`, this no-op would underflow the get/put pairing into a
UAF. `KOBJ_LOOM` and `KOBJ_PCI` are the contrasting cases — both name
refcounted objects, so both *must* bump, and their comments say why a no-op
would free the object early.

**Two kinds carry a runtime convention check.** `handle_alloc` extincts if a
`KOBJ_BURROW` arrives with `handle_count <= 0`, or a `KOBJ_LOOM` with
`refcount <= 0` — turning the consumed-reference convention from
documentation into an enforced precondition, so a future caller that
installs an unaccounted object is caught at the install rather than at the
underflowing close.

## Data structures

`struct Handle` is 24 bytes — magic, kind, rights, obj — with `_Static_assert`s
on both the size and the `magic` offset at 0. A slot is free iff
`magic == 0`, which is why the `KP_ZERO` table allocation is meaningful and
not merely tidy.

`struct HandleTable` is a lock plus `PROC_HANDLE_MAX` (64) inline slots. It
exceeds `SLUB_MAX_OBJECT_SIZE`, so it is kmalloc-backed through
`alloc_pages` rather than getting a dedicated slab cache — which is why
`handle_init` has nothing to do.

## Concurrency

One spinlock per table serializes alloc / close / get / dup. The discipline
that makes it correct across blocking work is:

> take the snapshot **and** bump the object's refcount under the lock;
> release the borrowed reference outside it.

That is `#844`. A sibling thread's `handle_close` either runs before the
`handle_get` (which then sees a zeroed slot and fails) or after it (by which
point `handle_get` holds its own reference), so a live pointer can never be
freed underneath a caller that is blocked in device I/O, an accept, or a 9P
handshake. Release must run outside the lock because it may sleep —
`spoor_clunk` runs the Dev close hook.

`handle_dup` is the subtle one: validate the parent, acquire the child's
reference, and install the child slot under **one** lock hold. The earlier
shape — `handle_get` then `handle_alloc` — took the lock twice and left a
window in which the parent could be closed between the two.

`handle_alloc` is the other `#844` lesson, found by audit rather than by
reasoning: it was left unlocked while get / close / dup were locked, even
though it is the *primary* fd-creating path. Two peer threads could pick the
same free slot, the second write clobbering the first — two fds naming one
slot, one object's table reference leaked, then a double release at close.

`handle_table_free` deliberately takes **no** lock, and the justification is
narrow enough that the code marks it a FOOTGUN: it runs only where exactly
one live thread remains, and no production path ever touches a *foreign*
ALIVE Proc's table, because every handle op derives its Proc from
`current_thread()->proc`. A cross-Proc accessor — a `/proc/<pid>/fd` surface
inspecting a live peer — breaks that premise. That is precisely why `#66c`
is deferred.

## Invariants enforced

- **I-5** (hardware handles non-transferable) — by partition membership, so
  no per-kind code exists in `handle_dup` or the transfer path.
- **I-6** (rights monotonic reduction) — `(new_rights & parent->rights) !=
  new_rights` rejects, the runtime form of `RightsCeiling`.
- **I-4** (transfer only via 9P) — enforced by *absence*: no direct
  cross-Proc transfer syscall exists.

None of the three has a registry note yet; the sweep this dossier belongs to
is what unblocks minting them.

## Error paths

Every rejection is a `-1` return, never an extinction — handle lookups sit
on the syscall entry path and must fail closed on a bad fd. The exceptions
are deliberate: a NULL/corrupted Proc or NULL table extincts
(`proc_handles_or_extinct`), as does an out-of-enum kind or a convention
violation, because all three mean memory corruption rather than a bad
argument.

`handle_get` zeroes the caller's snapshot *first*, so every failure path
leaves a struct that `handle_put` safely ignores. `handle_put` re-zeroes
after releasing, making a double put a no-op.

The one rollback: if `handle_dup`'s install fails on a full table, the
acquired reference is released **outside** the lock.

## Performance

Linear scan of 64 slots for a free index — O(64) worst case on every fd
creation, uncontended in practice. Growable tables via an index-keyed tree
are a stated Phase-5+ refinement. `g_handle_allocated` / `g_handle_freed`
are relaxed atomics for diagnostics only.

## Prosecution

- A new `kobj_kind` must be classified into exactly one partition; the
  completeness assert fails the build otherwise. Do not "fix" that assert by
  widening a mask.
- A new kind whose object is refcounted **must** bump in
  `handle_acquire_obj`. The `KOBJ_SRV` no-op is sound only because that
  handle is always a non-refcounted service listener.
- Any new fd-creating path must install through `handle_install_locked`
  under the table lock — the `#844` F1 lesson.
- Release may sleep; acquire may not. Anything called under the table lock
  must be non-blocking.
- The lockless `handle_table_free` rests on "no cross-Proc handle access
  exists". A cross-Proc accessor must take the table lock *and* coordinate
  with the `#926`/`#68` at-exit close.

## Seams

`#66c` (`/proc/<pid>/fd`) is deferred precisely on this surface — see the
FOOTGUN above. Growable tables, and refcount integration for
`KOBJ_PROCESS` / `KOBJ_THREAD` / `KOBJ_INTERRUPT` (currently no-ops on both
sides), are stated Phase-5+ items.

## Caveats

- `PROC_HANDLE_MAX` is 64. A Proc that needs more fds has no growth path at
  v1.0.
- The header's own preamble is stale in one respect: it describes
  `CapsCeiling` as "forward-looking; rfork mask is AND-only at Phase 5+".
  The unconditional `& ~CAP_ELEVATION_ONLY` strip landed at A-4-pre — see
  [[sub-kernel-caps]].
- `kobj_kind_is_srv` is defined but has no caller in the kernel tree.

## Provenance

[[chg-2026-08-02-authority-sweep]].
