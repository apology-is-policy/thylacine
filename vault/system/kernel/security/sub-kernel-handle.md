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
updated: 2026-08-15
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

**Four duplication primitives, and they are not conveniences over each
other.** Two axes decide which one a caller needs — where the source comes
from, and whether the destination may already be occupied:

| | source | destination |
|---|---|---|
| `handle_dup_posix` | a slot | first free ≥ `min_idx`; the index is an **output** |
| `handle_replace` | a caller-held object | a fixed index that must be **live**; outgoing released |
| `handle_table_copy_into` | a slot in **another** table | the same fixed index, which must be free |
| `handle_dup_to` | a slot | a fixed index, free **or** live; outgoing released if live |

`min_idx` is why `handle_dup_posix` exists rather than being folded into
`handle_dup`: a shell's `savefd()` asks for the lowest free descriptor **at
or above 10**, precisely to move its bookkeeping out of the low range where
a user redirection could collide with it. Returning the first free slot
regardless would hand back fd 3 and break the guarantee the call was made
to obtain — silently, and only under a redirection.

The two are also kept separate rather than folded behind a sentinel because
zero rights already means `RIGHT_NONE` and would have to mean its opposite
here — `handle_dup` **reduces** rights (the capability surface),
`handle_dup_posix` carries them **verbatim** (POSIX: the new descriptor has
the same access, and I-6 is satisfied by non-increasing).

`handle_close_on_exec(p)` closes every flagged handle. It runs **after
exec's commit point**, for two reasons pulling the same way: the closes may
**sleep** (a Spoor's Dev close hook sends a 9P `Tclunk`), so it cannot run
under any lock; and a failed exec must leave the process unchanged, so it
must not run before the last thing that can fail. Linux places its
equivalent after its own point of no return for the second reason.

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

`struct HandleTable` is a lock, a **close-on-exec bitmap**, and
`PROC_HANDLE_MAX` (**1024**) inline slots — about 24.7 KiB. It far exceeds
`SLUB_MAX_OBJECT_SIZE`, so it is kmalloc-backed through `alloc_pages`
rather than getting a dedicated slab cache — which is why `handle_init` has
nothing to do. A `_Static_assert` pins the total against
`8 + 8*HANDLE_CLOEXEC_WORDS + 24*PROC_HANDLE_MAX`.

**1024 is a measured number, not a round one.** A GL client holds one
handle per live BO mapping plus its files, so a Quake-class texture set
needs 300–500 live handles, and the compositor holds a DMA handle per built
BO on the same constant. The growable two-level table keyed by `hidx_t` —
the Linux model — remains the v1.x design for a Proc that needs far more,
and it is deferred because it touches the shared-table lock discipline
below (peer threads snapshot slots by value).

Read the fps figure attached to that lift carefully, because the header
states it precisely and it is easy to quote wrongly: the lift is credited
**only in combination** with the session-fid and compositor-fid lifts. The
A/B ladder's second rung is the reason — raising this constant *alone* was
**byte-identical**, an exoneration. Four nested tables bound the same
workload in series, and lifting any one of them just moved the binder.

**The close-on-exec bitmap sits beside the slot array rather than inside
`struct Handle`, and both halves of that are load-bearing.**

*Parallel*, because close-on-exec is a property of the **descriptor**, not
of the open file description behind it: `dup(fd)` yields a second
descriptor onto the same description with the flag **clear**, and setting
it on either must not touch the other. A bit inside `struct Handle` would
be shared by exactly the things POSIX says must differ. Linux keeps it as a
bitmap beside `fd[]` for this reason and the shape is not a coincidence.

*Not inside*, also because that struct has **no slack** — 8 + 4 + 4 + 8 is
exactly the 24 its own assert pins. A `u32` there grows it to 32, taking
the table from 24712 to 32904 bytes: **across the order-3 `alloc_pages`
boundary into order-4, doubling the physical allocation to carry one bit
per slot.** The bitmap costs 128 bytes total. The size assert on `struct
Handle` is what forced the better design rather than merely recording it.

A second assert requires the bitmap to cover every slot, because a short
one "would silently drop the flag on the high slots" — the failure that
would otherwise appear only above whatever index the words happened to
reach.

`hidx_t` is the handle index type: signed, with -1 for
invalid / not-found / table-full.

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

**`handle_dup_to` exists for the same reason, and its argument is the
sharpest statement of the rule in the file.** The obvious implementation —
close the destination, then dup with `min_idx` set to it — is wrong because
**the freed index is not reserved**: a peer thread's fd-creating syscall
can take it between the two calls, and the dup then lands somewhere else
entirely, silently, and only under concurrency. Doing it in one hold is
also what makes the outgoing release happen **after** the new object is
installed, so the slot is never momentarily empty for anyone to allocate
into.

Its source passes the same alias gate `handle_dup` uses, because it creates
a second handle naming one object and faces the identical hazard. Its
**destination is deliberately ungated by kind**, and the asymmetry is
argued rather than assumed: the destination is being *closed*, and
`handle_close` places no kind restriction on closing, so refusing here
would invent a rule that dup2-onto-fd-N is less permitted than
close(N)-then-dup. `handle_replace` refuses a non-Spoor outgoing for a
reason the header says does **not** transfer — its swap is internal to
`connect()` and invisible to the guest, whereas a dup3 caller has
explicitly named the fd it wants gone.

**The close-on-exec flag is set after the alloc rather than passed through
it, and the window is argued closed rather than assumed small.** A flag
parameter on `handle_alloc` would mean touching ~100 call sites for one
bit. The gap between alloc and set is provably empty: only exec consumes
the flag, execve refuses unless it is the Proc's only live thread, and the
caller **is** a live thread mid-syscall — so no exec can run between them.

**Fork copies the table, and what it declines to copy is the interesting
half.** `handle_table_copy_into` carries rights verbatim (POSIX fork gives
the child the parent's access; I-6 holds by non-increasing) and each copied
slot takes its own reference, so the child's `handle_table_free` releases
them and a failed fork's rollback is already correct. It cannot fail — no
per-slot allocation, and the destination is known-empty and the same size.

Hardware handles are **not** inherited: a Proc that needs hardware
authority gets it the way every other Proc does, through the warden's
confer path, never through fd inheritance. The resulting hole is
*observable* — the child sees `EBADF` at that index, and its next open
lands there where Linux's would not — which the header calls the honest
report of an authority the child was never eligible to hold.

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

Linear scan for a free index — **O(1024)** worst case on every fd creation,
under the table lock. That figure moved 16× with `PROC_HANDLE_MAX` and the
scan did not change shape, so what was a cheap bounded walk is now the
strongest argument for the growable two-level table: the cost is paid on
every fd creation by every Proc, while the 1024 slots exist for the handful
of GL clients that need them.

Not a defect today — the scan stops at the first free slot, so a Proc
holding few handles pays little regardless of the ceiling, and the worst
case needs a nearly-full table. But it is the one property of this surface
that the ceiling lift made materially worse rather than merely larger, and
it is worth stating because the header's own rationale for 1024 is a
throughput argument. `g_handle_allocated` / `g_handle_freed` are relaxed
atomics for diagnostics only.

## Prosecution

- A new `kobj_kind` must be classified into exactly one partition; the
  completeness assert fails the build otherwise. Do not "fix" that assert by
  widening a mask.
- A new kind whose object is refcounted **must** bump in
  `handle_acquire_obj`. The `KOBJ_SRV` no-op is sound only because that
  handle is always a non-refcounted service listener.
- Any new fd-creating path must install through `handle_install_locked`
  under the table lock — the `#844` F1 lesson, now with four callers. It is
  the single install chokepoint and takes the starting index and the
  close-on-exec bit as parameters precisely so the four duplication
  primitives cannot each re-implement the scan.
- A new duplication primitive must be justified on the two axes (source,
  destination-may-be-occupied) or it is a convenience over an existing one.
  Composing two of them is the specific thing to refuse: the index freed by
  the first is **not reserved**, so a peer thread can take it before the
  second runs.
- Anything that lifts `PROC_HANDLE_MAX` must re-run every proof that named
  it. The lift to 1024 voided one (a kthread bounded "by the 64-slot
  table"), and left `poll.h` and `syscall.h` still quoting 64 (#184, #166).
- The close-on-exec bitmap must stay sized off `PROC_HANDLE_MAX`; its
  coverage assert is what stops a short bitmap silently dropping the flag
  on high slots.
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

- ~~`PROC_HANDLE_MAX` is 64.~~ **It is 1024, and this dossier was wrong when
  it was written, not merely stale.** The constant went 64 → 256 on
  2026-06-24 and 256 → 1024 on 2026-08-13; this dossier was written on
  2026-08-02, six weeks after the first lift, and said 64 in two places.

  Worth keeping rather than quietly fixing, because the failure has a
  source and the source is still live: **`poll.h` and `syscall.h` both
  still say `PROC_HANDLE_MAX = 64` today** (tasks #184, #166). A sweep that
  reads the surrounding prose instead of the `#define` inherits whatever
  the prose last believed — which is the [[chg-2026-08-15-build-targets]]
  lesson (*prefer the shortest list*) arriving from the other direction. The
  rule that would have caught it is the one the tree keeps teaching: a
  stale-constant sweep is where an error is most easily laundered, so read
  what each mention **claims**, and take the value from the definition.

  A Proc needing far more than 1024 fds still has no growth path at v1.0;
  the two-level table is the v1.x design.
- The header's own preamble is stale in one respect: it describes
  `CapsCeiling` as "forward-looking; rfork mask is AND-only at Phase 5+".
  The unconditional `& ~CAP_ELEVATION_ONLY` strip landed at A-4-pre — see
  [[sub-kernel-caps]].
- `kobj_kind_is_srv` is defined but has no caller in the kernel tree.

## Provenance

[[chg-2026-08-02-authority-sweep]].
