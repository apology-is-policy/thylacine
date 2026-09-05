---
id: sub-kernel-spoor
type: sub
title: "Spoor — the Plan 9 Chan: lifecycle, identity, and what a clone inherits"
parent: moc-kernel-namespace
code: ["kernel/spoor.c", "kernel/include/thylacine/spoor.h"]
audit: hard
guarded-by: [inv-i33]
validated-by: [gate-smp]
locks: []
hazards: []
abis: []
design: ["docs/ARCHITECTURE.md section 9", "docs/STALK-DESIGN.md"]
created: 2026-08-03
updated: 2026-08-03
---
## Purpose

A `struct Spoor` is Thylacine's Plan 9 `Chan`: a position inside one
Dev's namespace, plus the cursor and per-Dev state that position
carries. It is the object every namespace operation flows through —
`dev->attach` mints one at a mount, `dev->walk` advances it,
`dev->open` turns it into a read/write cursor, `spoor_clunk` retires it
— and the object every `KOBJ_SPOOR` handle points at, so it is also
what an EL0 file descriptor ultimately names.

This dossier covers the SUBSTRATE: allocation, the refcount, cloning,
retirement, and the identity fields. What is done WITH a Spoor lives in
[[sub-kernel-stalk]] (resolution), [[sub-kernel-territory]] (the mount
table that keys on its identity), and each Dev's own dossier. The
`Path` it carries is [[sub-kernel-path]] — a field of this struct with
its own note because its lifetime rules are separable.

## Contract

```c
void          spoor_init(void);                       // SLUB cache; extincts on re-call
struct Spoor *spoor_alloc(struct Dev *d);             // ref 1; NULL on d==NULL / OOM
void          spoor_ref(struct Spoor *c);             // +1; extincts on NULL/corrupt/pre<=0
void          spoor_unref(struct Spoor *c);           // -1, free at 0; NO dev->close
void          spoor_clunk(struct Spoor *c);           // -1, dev->close THEN free at 0
struct Spoor *spoor_clone(struct Spoor *c);           // fresh Spoor, copied state, ref 1
u32           spoor_next_devno(void);                 // monotonic from 1
int           spoor_stat_native(struct Spoor *c, struct t_stat *out);

struct Walkqid *walkqid_alloc(int max_qids);          // >= 1 slot even at 0
void            walkqid_free(struct Walkqid *w);

u64 spoor_total_allocated(void);  u64 spoor_total_freed(void);   // diagnostics
```

The two release entry points are NOT interchangeable, and picking the
wrong one is a live bug class rather than a style question:

- **`spoor_clunk` is the general "I am done with this" entry.** On the
  last drop it runs `dev->close` before freeing. Dev close hooks may
  assume one-shot semantics — nothing calls them twice on one Spoor.
- **`spoor_unref` is the pure refcount drop**, for a caller that knows
  the per-Dev state was never wired up or has already been torn down.
  Its production use is the failure-unwind in the walk syscalls, where
  the clone's `aux` is still a shallow copy of the source's and running
  the close hook would release the SOURCE's state (see Prosecution).

`spoor_ref` extincts on a NULL or zero-ref argument; `spoor_unref` and
`spoor_clunk` treat NULL as a no-op (the `burrow_unref` convention) but
extinct on a corrupted magic or a non-positive pre-value. After the drop
that reaches zero, the pointer is invalid.

## Mechanism

**Allocation.** `spoor_alloc_internal` is the single constructor behind
both `spoor_alloc` and `spoor_clone`: a `KP_ZERO` SLUB allocation from
the `spoor` cache, then `magic`, `dc` (cached from the Dev for
dispatch without a back-pointer chase), `dev`, a relaxed `ref = 1`, and
explicit zeroing of `flag`/`mode`/`offset`/`aux`. `devno` is set to 0 —
the static single-instance default; multi-instance Devs overwrite it at
attach. The relaxed init store is correct because the Spoor is not
reachable from another CPU until the caller publishes the pointer.

**Retirement.** `spoor_free_internal` re-checks the magic, asserts the
refcount actually reached zero, drops the `Path` ref, then **zeroes the
magic explicitly** before handing the slot back to SLUB. That last step
is not redundant with SLUB's own freelist write: it closes the window
between the free and the allocator's bookkeeping in which a stale
pointer would otherwise read plausible-looking fields.

**Cloning** copies `qid`, `mode`, `offset`, `aux`, `devno` and `flag`,
and shares the `Path` by increment. Each has a stated reason in the
source; the one that matters is the flag mask, discussed below.
`aux` is copied SHALLOW and `spoor_clone` does not interpret it — a Dev
whose `aux` owns refcounted state must take its own reference inside
`dev->walk` before populating the new Spoor. Between the clone and that
moment the two Spoors alias one `aux` with one reference, which is the
window the walk syscalls' failure paths exist to handle.

**Device numbering.** `spoor_next_devno` mints a monotonic per-instance
id so the mount table can key on the full Plan 9 `(type, dev, qid)`
identity. This is load-bearing for dev9p specifically: every 9P session
shares `dc='9'` and every session root has `qid.path == 0`, so without
`devno` two concurrent sessions' roots would be indistinguishable to
the mount table. The counter wraps at 2^32 attaches; the source argues
the wrap is benign because a collision needs two LIVE same-devno
sessions in one Territory's table, and the key is identity
disambiguation rather than a capability.

**Walkqid** is the Plan 9 walk result, preserved verbatim: the Spoor at
the deepest successful step plus the qid of every step that succeeded,
in a flexible-array tail. `walkqid_alloc(0)` still allocates one slot,
avoiding a zero-sized allocation for the zero-element clone-walk that
mount crossing depends on.

## Data structures

```c
struct Qid { u64 path; u32 vers; u8 type; u8 pad[3]; };   // 16 B, asserted

struct Spoor {
    u64          magic;    // SPOOR_MAGIC, offset 0 — asserted
    int          dc;       // cached dev->dc
    u32          devno;    // per-instance device number
    struct Dev  *dev;
    struct Qid   qid;
    spin_lock_t  lock;     // DEAD — see Caveats
    int          ref;      // atomic in practice; plain `int` in the type
    u32          flag;     // COPEN | CMSG | CSRVCLIENT | CWALKONLY | CDEBUGOWNER | CCONSWINSZONLY
    int          mode;     // omode after open
    s64          offset;   // byte cursor
    void        *aux;      // dev-private, opaque here
    struct Path *path;     // #66 namespace name; I-33 non-load-bearing
};
```

`magic` at offset 0 is pinned by a `_Static_assert` whose message names
the mechanism it defends: SLUB's freelist write at free lands at offset
0, so a use-after-free dereference hits a corrupted magic rather than
stale-but-plausible fields. There is **no assert on the struct's total
size** — the offset is pinned, the footprint is not.

`ref` is declared a plain `int` and manipulated exclusively through
`__atomic` / `t_atomic_*` accessors. The type does not carry the
contract; five separate comments in `spoor.c` do.

The six `flag` bits are not one kind of thing. `COPEN`/`CMSG` describe
the Spoor's I/O state; `CWALKONLY` marks an `O_PATH` navigation handle;
`CSRVCLIENT` records which END of a `/srv` byte connection this is;
`CDEBUGOWNER` records that this ctl Spoor holds a debug attach slot;
`CCONSWINSZONLY` restricts a renderer-minted `consctl` to one verb.
Three of the six are therefore not state but PROVENANCE — assertions
about how the Spoor came to exist — which is what makes the clone rule
load-bearing.

## Concurrency

The refcount is the whole concurrency story. `spoor_ref` uses
`t_atomic_fetch_add_acqrel_int`; `spoor_unref` and `spoor_clunk` use
`fetch_sub` and act on the PRE value, so under concurrent release from
two CPUs **exactly one sees `pre == 1`** and owns the free — and, in
`spoor_clunk`, owns the `dev->close` call. The close hook runs after
the decrement (so the count reads 0) but before the free, so it can
still safely read `aux`, `dev` and `qid`. A `pre <= 0` on any path is
treated as a use-after-free diagnostic and extincts.

`spoor_clone` reads the source's ref atomically for diagnosis only; it
does not mutate it, and the caller is responsible for holding the
source alive across the call.

`qid`, `dev`, `dc`, `devno` and `path` are written before the Spoor is
published and read freely afterward — no lock. The per-Spoor
`spin_lock_t lock` is initialized on every allocation and **acquired
nowhere in the tree**; see Caveats.

## Invariants enforced

No section-28 invariant is enforced *here*. The Spoor is the object the
namespace invariants are stated ABOUT — [[inv-i1]]'s isolation,
[[inv-i28]]'s containment and per-component X-search, and
[[inv-i3]]'s composition graph are all enforced by
[[sub-kernel-stalk]] and [[sub-kernel-territory]] over Spoors this
layer merely mints and retires. Recording that plainly matters: a
reader looking for where a Spoor's authority is checked must not stop
here and conclude the answer is "nowhere".

- **[[inv-i33]]** is the exception, and only half of it: this layer
  owns the `path` field and implements three of the four hooks that
  maintain it (`spoor_clone` shares by increment,
  `spoor_path_extend` copy-on-walks, `spoor_path_transplant`
  re-points at a mount cross, `spoor_free_internal` releases). The
  fail-soft property — an allocation failure leaves the name NULL and
  the WALK STILL SUCCEEDS — is implemented in
  [[sub-kernel-path]]'s constructors and honored here by never
  checking the result.

## Error paths

`spoor_alloc` returns NULL on a NULL Dev or SLUB exhaustion; every
caller must handle it, and the walk syscalls do (a clone failure
clunks the source and returns -1). `walkqid_alloc` returns NULL on a
negative count or OOM. `spoor_stat_native` returns -1 when the Spoor,
the output buffer, or the Dev's `stat_native` slot is absent — the
NULL-slot arm that makes an fstat on a slotless Dev fail, which has
been a live bug class twice ([[sub-kernel-dev]], Caveats).

Everything else on this surface is an extinction rather than an error
return: allocating before `spoor_init`, initializing twice, a corrupted
magic, a non-positive refcount, or a premature free. That is the
deliberate split — a lifetime violation is a kernel bug with no correct
recovery, while an allocation failure is a userspace-reachable
condition that must never extinct.

## Performance

`spoor_clone` is on the hot walk path — one per resolution hop,
including hops that fail and unwind — so it is deliberately a shallow
field copy plus an O(1) `Path` increment, with no string work. The
allocation is a SLUB fast-path. `spoor.alloc_10k_no_leak` is the
standing 10,000-cycle leak check inherited from the ROADMAP section 6.2
exit criterion.

The one avoidable cost is `spin_lock_init` on a field nothing acquires,
executed on every allocation and every clone.

## Prosecution

- **The refcount balances on every path.** `spoor_total_allocated` /
  `spoor_total_freed` exist so tests can assert the balance without
  dereferencing freed pointers; `(allocated - freed)` is the live count
  at any instant.
- **The clunk/unref choice must match the state of `aux`.** A clone
  whose `dev->walk` failed still aliases the source's `aux`; releasing
  it with `spoor_clunk` runs the Dev close hook against the SOURCE's
  state. `sys_walk_open_handler` gets this right on two of its three
  post-clone failure exits and wrong-but-currently-harmless on the
  third — see [[sub-kernel-dev]]'s Prosecution and the tracked item.
- **Flag inheritance is a policy, not a copy.** `spoor_clone` masks
  exactly one bit. Any new flag must be classified as state (inherit)
  or provenance (must not), and the classification must be written at
  the mask rather than at the flag's definition.
- **The magic must stay at offset 0** — the assert says so and names
  why. Reordering the struct silently disarms the use-after-free
  defense.
- **`spoor_ref` on a zero-ref Spoor increments before it extincts.**
  Harmless because extinction halts, but the ordering is deliberate and
  a future "log and continue" variant would be unsound.

Covered by `spoor.alloc_unref_round_trip`, `ref_lifecycle`,
`clone_lifecycle`, `clone_copies_state`, `clunk_dispatches_close`,
`alloc_10k_no_leak`, `stat_native_stamps_devno`.

## Seams

- **The struct has no size assert.** Every other ABI-adjacent struct in
  the tree that grew unexpectedly was caught (or missed) by exactly
  this kind of pin — see the `t_stat` mirror finding. `struct Spoor` is
  kernel-internal so a growth breaks no ABI, but it does silently
  enlarge a hot SLUB cache.
- **`CMSG` has no production consumer.** It is defined, documented as
  message-style read semantics, copied by the clone, and read by
  nothing outside a test. Either a Dev was meant to honor it or it
  should go.

## Caveats

- **`spoor_clone` inherits `COPEN`, so `COPEN` does not mean "this
  Spoor was opened".** It means "this Spoor, or an ancestor it was
  cloned from, was opened". The mask excludes exactly one bit
  (`CWALKONLY`), and the exclusion's comment argues from that flag's
  own semantics — a per-flag reason, not a policy — so the other five
  inherit by default with no reason written anywhere. Of the five, one
  consumer is actively broken by it: `devdev_close` disarms the console
  drain on `qid.path == DEV_KIND_CONSDRAIN && (flag & COPEN)` and its
  comment calls that check load-bearing, so a failed walk from the
  renderer's own drain fd silently disarms a live tap (task #74). A
  second consumer lands on the safe side of the identical inheritance —
  dev9p's dir-fid park gates on `COPEN == 0`, so a spurious set only
  declines to park a parkable fid. The remaining three are saved by
  mechanisms that have nothing to do with cloning: `CDEBUGOWNER` by the
  release walk matching `debug_owner` on POINTER IDENTITY (whose
  comment justifies the choice by pid reuse and post-reap staleness,
  not by clones), `CCONSWINSZONLY` because it is restrictive so
  inheriting it fails safe, and `CSRVCLIENT` because `devsrv_walk`
  refuses a non-registry source outright. **Five flags inherit; the
  safety of four is accidental relative to the rule that produces it.**
- **The clone test asserts the property that breaks it.**
  `spoor.clone_copies_state` sets `flag = COPEN | CMSG` and asserts
  `nc->flag == c->flag` — "flag copied", a whole-word claim that has
  been false since `CWALKONLY` was masked, and which passes only
  because the chosen bits avoid the one exception. The test documents
  that inheritance HAPPENS; nothing tests that it is SAFE.
- **`struct Spoor.lock` is dead.** The `spin_lock_init` inside
  `spoor_alloc_internal` is the only reference in the tree; nothing
  acquires it. The header reserves it so "the SMP-safe refcount upgrade
  (Phase 5+) doesn't need a struct change" — but that upgrade shipped
  as atomics and never used the lock (task #77).
- **The header describes its own refcount as future work.** Its
  preamble calls the Spoor "Single-CPU at v1.0", and its Phase 4+
  extension list still carries "Spoor refcount becomes atomic when the
  syscall surface goes SMP". It is atomic now, and `spoor.c` says so
  five times. A reader who trusts the header reaches the opposite
  conclusion about the current concurrency story (same task).
- **A `Walkqid` is caller-owned.** `dev->walk` allocates it; the caller
  frees it. The Spoor inside it may or may not be the `nc` the caller
  passed, and the syscall handlers reject the "or may not" case rather
  than support it.

## Provenance

(generated -- incoming `touched` backlinks, newest first; never hand-written)
