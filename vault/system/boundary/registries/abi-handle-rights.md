---
id: abi-handle-rights
type: abi
kind: registry
stability: append-only
title: "The handle registry — kobj kinds, the four partitions, and the six rights"
pinned-by:
  - "_Static_assert KOBJ_KIND_COUNT == 12 (kernel/include/thylacine/handle.h)"
  - "_Static_assert: six pairwise-disjointness + one coverage assert over the four kind masks"
  - "_Static_assert sizeof(struct Handle) == 24, offsetof(magic) == 0"
  - "specs/handles.tla::TxKObjs / HwKObjs / SrvKObjs"
mirrors:
  - "usr/lib/libthyla-rs/src/lib.rs: T_RIGHT_* consts"
  - "usr/lib/libthyla-rs/src/handle.rs: the typed Rights surface"
  - "stratum (thylacine-pouch-arm) src/block/bdev_thylacine.c: T_RIGHT_* (partial: READ/WRITE/MAP/SIGNAL)"
created: 2026-08-02
updated: 2026-08-02
---
## The surface

A handle is a per-Proc integer naming a kernel object, carrying a *kind* and
a *rights* mask. It cannot be forged: it only ever comes from the kernel.

### The twelve kinds, in four disjoint partitions

`KOBJ_INVALID = 0` is zero-init and belongs to no partition — which is what
makes a freshly zeroed table entirely free.

**Transferable** (`KOBJ_KIND_TRANSFERABLE_MASK`) — may pass between Procs
via 9P, the only transfer path there is (I-4):

| n | kind | object |
|---|---|---|
| 1 | `KOBJ_PROCESS` | a `struct Proc *` |
| 2 | `KOBJ_THREAD` | a `struct Thread *` |
| 3 | `KOBJ_BURROW` | a `struct Burrow *` |
| 4 | `KOBJ_SPOOR` | an open 9P channel |

**Hardware** (`KOBJ_KIND_HW_MASK`) — pinned to the Proc that created them;
never transfer, never dup (I-5):

| n | kind | object |
|---|---|---|
| 5 | `KOBJ_MMIO` | an MMIO range |
| 6 | `KOBJ_IRQ` | an IRQ subscription |
| 7 | `KOBJ_DMA` | a DMA buffer |
| 8 | `KOBJ_INTERRUPT` | an eventfd-like interrupt |
| 11 | `KOBJ_PCI` | a claimed VirtIO-PCI function |

`KOBJ_PCI` joins this mask rather than getting its own, and that is
deliberate economy: membership buys non-transferability and no-dup with no
per-kind code in `handle_dup` or the 9P path.

**Srv** (`KOBJ_KIND_SRV_MASK`) — `KOBJ_SRV = 9`, a `/srv` service or
connection. Non-transferable but *not* hardware, so it needs its own
partition. Pinning it to the opening Proc is what makes the kernel-stamped
peer identity behind it unforgeable across a walk.

**Loom** (`KOBJ_KIND_LOOM_MASK`) — `KOBJ_LOOM = 10`. A ring is bound to the
address space holding its Burrow and the handle table its registered handles
name; passing it to another Proc would be meaningless, so it is neither
transferable nor dup-able.

### The six rights

| bit | name | permits |
|---|---|---|
| 0 | `RIGHT_READ` | `SYS_READ` and read-shaped ops |
| 1 | `RIGHT_WRITE` | `SYS_WRITE` and write-shaped ops |
| 2 | `RIGHT_MAP` | mapping the object into the address space |
| 3 | `RIGHT_TRANSFER` | passing the handle via 9P |
| 4 | `RIGHT_DMA` | DMA-capable use |
| 5 | `RIGHT_SIGNAL` | `SYS_IRQ_WAIT` on a `KObj_IRQ` |

`RIGHT_NONE` is 0 and `RIGHT_ALL` is `0x3f`. Rights **only ever reduce** on
dup or transfer (I-6), and an alloc with empty rights or with any bit
outside `RIGHT_ALL` is rejected.

`struct Handle` is pinned at 24 bytes with `magic` at offset 0 — so
`KP_ZERO` over the table leaves every slot's magic zero, which *is* the
free-slot signal. No separate free list.

## Change protocol

Adding a kind: bump `KOBJ_KIND_COUNT`, add it to exactly one of the four
masks, and review every switch over `kobj_kind` (`handle_release_obj`,
`handle_acquire_obj`).

**This registry is the well-guarded one, and it is worth studying as the
model.** Seven assertions hold the partition in place: six pairwise
disjointness checks, plus a **coverage** assert that the union of the four
masks equals every bit below `KOBJ_KIND_COUNT` except `KOBJ_INVALID`. A new
kind that lands in two masks fails the build; a new kind that lands in *no*
mask also fails the build. Both halves are enforced.

Compare [[abi-caps]], which has the same two-set shape and only the
disjointness half — its coverage check is a tautology that cannot fire. The
difference is not stylistic: it is the difference between a new bit being
caught at compile time and a new bit silently doing nothing.

Adding a right: define the bit **and bump `RIGHT_ALL`**.

## The unpinned literal

`RIGHT_ALL` is the hardcoded `0x3fu`. Nothing ties it to the six `RIGHT_*`
definitions — no assert, no derivation. A seventh right defined as
`(1u << 6)` without bumping `RIGHT_ALL` to `0x7f` compiles clean, and then
fails *at runtime, everywhere*: `RIGHT_ALL` is the validation mask, so
`handle_alloc` and five separate `syscall.c` gates all reject any request
carrying the new bit as out-of-range.

That failure mode is worse than the capability one. A forgotten `CAP_ALL`
update makes a capability ungrantable — nothing happens. A forgotten
`RIGHT_ALL` update makes every attempt to *use* the new right fail
validation, from six sites, with no message pointing at the cause. Tracked
as task #36; the fix is to derive `RIGHT_ALL` from the bits rather than
restate it.

## Where the prose has drifted from the code

`handle.h`'s own header comment is stale in three places, all from kinds
appended without revisiting the sentence:

- "At v1.0 P2-Fc: **Nine** kobj kinds (per §18.2)" — there are twelve.
- The enum's own comment: "Per ARCH §18.2. **Eight kinds**." — twelve.
- "Type partitioning (transferable vs hw)" describes two partitions; there
  are four.

The count assert (`KOBJ_KIND_COUNT == 12`) is correct and enforced, so the
code is right and only the description is wrong — the same split as in
[[abi-caps]], where the values are pinned and the narrative around them is
not. Also stale in the same block: "Cross-Proc transfer
(`handle_transfer_via_9p`): not yet" and "Underlying-kobj refcount
integration: not yet", both of which have since landed.

## The mirror that drifted, and what fixed it

`bdev_thylacine.c` (Stratum, `thylacine-pouch-arm`) carries a **partial**
mirror — `READ`, `WRITE`, `MAP`, `SIGNAL`, but not `TRANSFER` or `DMA`,
because it uses only what it needs. That partiality is exactly how it once
broke: `T_RIGHT_SIGNAL` was defined as `(1u << 3)` — the next bit *visible
in the partial list* — which is `RIGHT_TRANSFER`. The driver asked for
transfer rights and got no signal rights.

It is correct now (`1u << 5`). The lesson the incident leaves is about the
shape, not the value: **a partial mirror invites the next bit to be
guessed from the local list rather than read from the registry.** Any
cross-project reproduction of these bits must copy the numbers, never
re-derive them.

`libthyla-rs` mirrors the full set in `lib.rs` as `T_RIGHT_*` consts, with a
typed surface in `handle.rs`.

## Prosecution

- A new kind in no mask fails the build — verified by the coverage assert.
  Do not weaken that assert to match `caps.h`; the direction of travel is
  the other way.
- A new right without a `RIGHT_ALL` bump is rejected at six validation
  sites and looks like six unrelated failures.
- Any code path letting a hardware, srv, or loom handle cross a Proc
  boundary breaks I-5 and its structural siblings. The masks, not per-kind
  checks, are what enforce it.
- A partial cross-project mirror must copy bit values verbatim. Deriving
  "the next free bit" from a partial list is how the Stratum collision
  happened.

## Referenced by

[[sub-kernel-handle]] · [[sub-kernel-hwcap]] · [[sub-kernel-srvconn]] ·
[[sub-kernel-loom]] · [[inv-i5]] · [[abi-caps]] · [[moc-boundary]].
