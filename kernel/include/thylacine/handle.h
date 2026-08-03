// Kernel handle table — typed unforgeable tokens to kernel objects (P2-Fc).
//
// Per ARCHITECTURE.md §18 + specs/handles.tla. A Handle is a per-Proc
// integer index naming a kernel object the process is allowed to access.
// Handles cannot be forged; they can only be received from the kernel
// (e.g., as the return of `burrow_create`) or transferred via 9P (Phase 4).
//
// At v1.0 P2-Fc:
//   - Nine kobj kinds (per §18.2): Process / Thread / BURROW / Spoor
//     (transferable) + MMIO / IRQ / DMA / Interrupt (hardware, non-
//     transferable) + Srv (a /srv service or connection object, non-
//     transferable; P5-corvus-srv).
//   - Six rights (per §18.2): READ / WRITE / MAP / TRANSFER / DMA / SIGNAL.
//   - Per-Proc HandleTable is a fixed-size array (PROC_HANDLE_MAX = 64).
//     Phase 5+ refactors to growable RB-tree when the syscall surface
//     lands.
//   - Underlying-kobj refcount integration: not yet. handle_close just
//     zeros the slot. P2-Fd integrates `burrow_unref` for KOBJ_BURROW; future
//     phases add refs for other kinds (struct Proc / Spoor / etc.).
//   - Cross-Proc transfer (handle_transfer_via_9p): not yet. Phase 4
//     wires the 9P out-of-band metadata path per §18.6.
//   - Type partitioning (transferable vs hw) is enforced at compile time
//     via _Static_assert on KOBJ_KIND_COUNT — adding a new kind requires
//     bumping the count + extending the masks + reviewing every switch
//     over kobj_kind in the kernel.
//
// State invariants pinned by specs/handles.tla (TLC-checked at P2-Fa):
//   I-2 (CapsCeiling)        — proc capabilities only reduce.
//   I-4 (OnlyTransferVia9P)  — no direct cross-Proc transfer syscall.
//   I-5 (HwHandlesAtOrigin)  — hw handles never transfer.
//   I-6 (RightsCeiling)      — handle rights only reduce on dup/transfer.
//   SrvHandlesAtOrigin       — KObj_Srv handles never transfer — the
//                              I-5-style structural rule for /srv
//                              connection Spoors (P5-corvus-srv).

#ifndef THYLACINE_HANDLE_H
#define THYLACINE_HANDLE_H

#include <thylacine/spinlock.h>     // #844: per-Proc handle-table lock
#include <thylacine/types.h>

struct Proc;

// HANDLE_MAGIC — sentinel set at handle_alloc; checked at handle_get /
// handle_close. Slot is FREE iff magic == 0 (KP_ZERO clears table to
// all-free at alloc; handle_close zeros the slot on release).
#define HANDLE_MAGIC 0x48414e444c45BAD2ULL    // 'HANDLE' | 0xBAD2

// Per ARCH §18.2. Eight kinds. Order matters — _Static_asserts pin the
// enum values to specific bits in the transferable + hw masks below.
enum kobj_kind {
    KOBJ_INVALID    = 0,    // zero-initialized; not usable
    KOBJ_PROCESS    = 1,    // a struct Proc *
    KOBJ_THREAD     = 2,    // a struct Thread *
    KOBJ_BURROW        = 3,    // a struct Burrow * (P2-Fd)
    KOBJ_SPOOR       = 4,    // an open 9P channel (Phase 4)
    KOBJ_MMIO       = 5,    // an MMIO range, non-transferable
    KOBJ_IRQ        = 6,    // an IRQ subscription, non-transferable
    KOBJ_DMA        = 7,    // a DMA buffer, non-transferable
    KOBJ_INTERRUPT  = 8,    // an eventfd-like interrupt, non-transferable
    KOBJ_SRV        = 9,    // a /srv service or connection object, non-transferable (P5-corvus-srv)
    KOBJ_LOOM       = 10,   // a Loom ring (KObj_Loom), non-transferable (Loom-2a)
    KOBJ_PCI        = 11,   // a claimed VirtIO-PCI function (KObj_PCI), hardware, non-transferable (pci-1b)
    KOBJ_KIND_COUNT = 12,
};

// _Static_assert pins KIND_COUNT — adding a new kind requires bumping
// this constant + extending the transferable/hw/srv/loom masks below +
// reviewing every switch over kobj_kind in the kernel (per ARCH §18.3 typed
// transferability).
_Static_assert(KOBJ_KIND_COUNT == 12,
               "kobj_kind drift: when adding a new kind, update "
               "KOBJ_KIND_TRANSFERABLE_MASK / KOBJ_KIND_HW_MASK / "
               "KOBJ_KIND_SRV_MASK / KOBJ_KIND_LOOM_MASK + every switch over "
               "kobj_kind (handle_release_obj, handle_acquire_obj).");

// Per ARCH §28 I-4 + I-5 + §18.2: handles are partitioned into three
// disjoint sets — transferable (Process / Thread / BURROW / Spoor —
// pass-able via 9P), hardware (MMIO / IRQ / DMA / Interrupt — pinned to
// the origin Proc), and srv (Srv — a /srv connection Spoor, likewise
// pinned to its origin Proc). KOBJ_INVALID is in none.
//
// Implementing specs/handles.tla's TxKObjs / HwKObjs / SrvKObjs
// partition (its three pairwise-disjoint ASSUMEs). The _Static_asserts
// below are the runtime guarantee that no kind ever appears in two
// sets — a violation would silently let a non-transferable handle
// transfer.
#define KOBJ_KIND_TRANSFERABLE_MASK \
    ((1u << KOBJ_PROCESS) | (1u << KOBJ_THREAD) | \
     (1u << KOBJ_BURROW)     | (1u << KOBJ_SPOOR))

// pci-1b: KObj_PCI (a claimed VirtIO-PCI function) is hardware — a driver holds
// exactly one handle per PCI function, pinned to the claiming Proc. Joining the
// HW mask gives I-5 non-transferability + NoHwDup for free (no per-kind code in
// handle_dup / the 9P transfer path).
#define KOBJ_KIND_HW_MASK \
    ((1u << KOBJ_MMIO) | (1u << KOBJ_IRQ) | \
     (1u << KOBJ_DMA)  | (1u << KOBJ_INTERRUPT) | (1u << KOBJ_PCI))

// P5-corvus-srv: KObj_Srv is non-transferable but NOT hardware — a
// distinct third partition. A /srv connection Spoor is pinned to the
// Proc that opened it, so the kernel-stamped peer identity behind it
// (CORVUS-DESIGN.md §6.3) is unforgeable across a 9P walk.
#define KOBJ_KIND_SRV_MASK \
    (1u << KOBJ_SRV)

// Loom-2a: KObj_Loom is non-transferable + non-hardware — a fourth
// partition. A Loom ring is pinned to the Proc whose address space holds
// the ring Burrow + whose handle table the registered handles name, so it
// is meaningless to pass to another Proc (and is never dup-able).
#define KOBJ_KIND_LOOM_MASK \
    (1u << KOBJ_LOOM)

_Static_assert((KOBJ_KIND_TRANSFERABLE_MASK & KOBJ_KIND_HW_MASK) == 0,
               "transferable + hw kind masks must be disjoint");
_Static_assert((KOBJ_KIND_TRANSFERABLE_MASK & KOBJ_KIND_SRV_MASK) == 0,
               "transferable + srv kind masks must be disjoint");
_Static_assert((KOBJ_KIND_HW_MASK & KOBJ_KIND_SRV_MASK) == 0,
               "hw + srv kind masks must be disjoint");
_Static_assert((KOBJ_KIND_TRANSFERABLE_MASK & KOBJ_KIND_LOOM_MASK) == 0,
               "transferable + loom kind masks must be disjoint");
_Static_assert((KOBJ_KIND_HW_MASK & KOBJ_KIND_LOOM_MASK) == 0,
               "hw + loom kind masks must be disjoint");
_Static_assert((KOBJ_KIND_SRV_MASK & KOBJ_KIND_LOOM_MASK) == 0,
               "srv + loom kind masks must be disjoint");
_Static_assert((KOBJ_KIND_TRANSFERABLE_MASK | KOBJ_KIND_HW_MASK |
                KOBJ_KIND_SRV_MASK | KOBJ_KIND_LOOM_MASK)
                   == (((1u << KOBJ_KIND_COUNT) - 1u) & ~(1u << KOBJ_INVALID)),
               "every kobj_kind except KOBJ_INVALID must be classified "
               "into exactly one of the four partitions");

// Per ARCH §18.2. Handle rights — bitmask of what the holder can do.
typedef u32 rights_t;
#define RIGHT_NONE      0u
#define RIGHT_READ      (1u << 0)
#define RIGHT_WRITE     (1u << 1)
#define RIGHT_MAP       (1u << 2)
#define RIGHT_TRANSFER  (1u << 3)
#define RIGHT_DMA       (1u << 4)
#define RIGHT_SIGNAL    (1u << 5)
#define RIGHT_ALL       0x3fu

struct Handle {
    u64               magic;       // HANDLE_MAGIC; 0 means free slot
    enum kobj_kind    kind;
    rights_t          rights;
    void             *obj;         // pointer to underlying kernel object
};

_Static_assert(sizeof(struct Handle) == 24,
               "struct Handle pinned at 24 bytes (8 magic + 4 kind + 4 "
               "rights + 8 obj). Adding a field grows the per-Proc table "
               "by PROC_HANDLE_MAX * delta bytes.");
_Static_assert(__builtin_offsetof(struct Handle, magic) == 0,
               "magic at offset 0 — KP_ZERO clearing the table at alloc "
               "makes every slot's magic == 0, naturally signaling free");

// Per-Proc handle table size. 64 was a v1.0 toy limit the code always
// anticipated growing (Phase 5+ -> growable). Real fd-hungry workloads --
// the on-device Go toolchain (cmd/go + compile/asm/link spawn fast and hold
// many open files), multi-fd servers -- need well more than 64, so it is
// raised to 256. 256 slots = 8 + 24*256 = 6152 bytes exceeds
// SLUB_MAX_OBJECT_SIZE (2048), so the table is kmalloc-backed (handle.c) --
// kmalloc routes the oversize object through alloc_pages (2 pages here) --
// rather than a dedicated slab cache, which cannot hold it. The growable
// table (a two-level fdtable keyed by hidx_t, the Linux model) is the v1.x
// design once a Proc needs >> 256; it touches the #844 shared-table lock
// discipline (peer threads snapshot slots by-value). Tracked as #355.
#define PROC_HANDLE_MAX 256

// #151: close-on-exec lives in a BITMAP PARALLEL TO THE SLOT ARRAY, not in
// struct Handle. Both halves of that are deliberate.
//
// PARALLEL, because POSIX close-on-exec is a property of the DESCRIPTOR, not of
// the open file description behind it. `dup(fd)` yields a second descriptor onto
// the same description with the flag CLEAR, and F_SETFD on either does not touch
// the other. A bit stored in struct Handle would be shared by exactly the things
// POSIX says must differ. Linux keeps `close_on_exec` as a bitmap beside `fd[]`
// in struct fdtable for this reason, and the shape is not a coincidence.
//
// NOT IN struct Handle, also because that struct has no slack: 8 + 4 + 4 + 8 is
// exactly the 24 its _Static_assert pins. A u32 there grows it to 32, taking the
// table from 6152 to 8200 bytes -- across the 2-page boundary into 3, a 50%
// per-Proc increase to carry one bit per slot. The bitmap costs 32 bytes total.
#define HANDLE_CLOEXEC_WORDS ((PROC_HANDLE_MAX + 63) / 64)

struct HandleTable {
    // #844: serializes all slot ops (alloc / close / get / dup) -- the Plan 9
    // Fgrp lock. Peer threads of a multi-threaded Proc share one HandleTable,
    // so a sibling's handle_close must not free a slot's obj or zero the slot
    // under a concurrent handle_get. Plain spin_lock (process-context only --
    // handle ops never run from IRQ; matches p->vma_lock). KP_ZERO at
    // handle_table_alloc inits it unlocked. The obj refcount (bumped under
    // this lock in handle_get/dup, dropped OUTSIDE it in handle_put/close)
    // carries the obj's lifetime past the lock release.
    //
    // The lock covers `cloexec` too: the flag is read and written by the same
    // peer threads that race over the slots, so it takes the same protection.
    spin_lock_t   lock;
    u32           _pad_lock;
    u64           cloexec[HANDLE_CLOEXEC_WORDS];
    struct Handle slots[PROC_HANDLE_MAX];
};

_Static_assert(sizeof(struct HandleTable) ==
                   8 + 8 * HANDLE_CLOEXEC_WORDS + 24 * PROC_HANDLE_MAX,
               "HandleTable size pinned at 8 (lock + pad) + the cloexec bitmap "
               "+ PROC_HANDLE_MAX * sizeof(Handle)");
_Static_assert(HANDLE_CLOEXEC_WORDS * 64 >= PROC_HANDLE_MAX,
               "the cloexec bitmap must cover every slot -- a short bitmap "
               "would silently drop the flag on the high slots");

// Handle index — signed; -1 indicates invalid / not-found / table-full.
typedef int hidx_t;

// Bring up the handle subsystem. Allocates the SLUB cache for
// HandleTable. Must be called before proc_init (since proc_alloc
// allocates a HandleTable for each new Proc).
void handle_init(void);

// SLUB-allocate a fresh HandleTable for a new Proc. All slots free
// (magic == 0). Returns NULL on OOM.
struct HandleTable *handle_table_alloc(void);

// Release a HandleTable. Closes any open handles first (zeros their
// slots; at v1.0 P2-Fc no underlying kobj refcount is decremented —
// P2-Fd integrates burrow_unref for KOBJ_BURROW; future phases wire the
// other kinds).
void handle_table_free(struct HandleTable *t);

// Allocate a handle in p's table.
//
// kind: must be in [KOBJ_PROCESS .. KOBJ_KIND_COUNT-1]. KOBJ_INVALID
//   and out-of-range values are rejected.
// rights: non-empty bitmask drawn from RIGHT_ALL. Empty rights or
//   bits outside RIGHT_ALL are rejected.
// obj: pointer to the underlying kernel object. May be NULL at v1.0
//   for kinds whose underlying impl isn't yet integrated (test paths);
//   production callers always pass a valid obj.
//
// Returns the slot index on success, -1 on validation failure or
// table-full.
//
// Maps to specs/handles.tla::HandleAlloc(p, k, granted).
hidx_t handle_alloc(struct Proc *p, enum kobj_kind kind,
                    rights_t rights, void *obj);

// Release a handle. Returns 0 on success, -1 if the slot is empty
// (already-closed or never-allocated) or out-of-range. At v1.0 P2-Fc
// the underlying kobj is NOT reference-counted; close just zeros the
// slot.
//
// Maps to specs/handles.tla::HandleClose(p, h).
int handle_close(struct Proc *p, hidx_t h);

// Swap what a LIVE handle denotes, in place, keeping the same slot index.
// Returns 0 on success, -1 on any refusal (out-of-range h, empty slot, bad
// args, or a kind outside the permitted pair). On success the outgoing object
// is released exactly once; on any refusal nothing is touched and the caller
// still owns the reference it passed in.
//
// WHY THIS EXISTS, AND WHY IT IS THIS NARROW (VIVARIUM V-5, docs/VIVARIUM.md
// section 5.5.1). A Linux `connect()` must turn the guest's socket fd from the
// /net connection's `ctl` file into its `data` file, and the guest is holding a
// SPECIFIC fd number it will then read()/write(). handle_dup cannot serve --
// it allocates a NEW slot. close-then-alloc cannot serve either: the freed
// index is not reserved, so a peer thread's fd-creating syscall can take it.
//
// KOBJ_SPOOR -> KOBJ_SPOOR ONLY. This is deliberately narrower than "not a
// hardware kind", because the primitive has exactly one caller and a narrow
// gate is a cheap audit. Widening it means re-deriving these, none of which
// the current restriction has to argue:
//
//   * I-5 (hardware handles are non-transferable): with hw kinds excluded on
//     BOTH sides, no path here can make a KObj_MMIO/IRQ/DMA/PCI handle appear
//     at, or vanish from, an arbitrary fd. A widened version would have to.
//   * I-6 (rights reduce monotonically): rights come from the CALLER, which
//     derives them from the incoming object's open mode exactly as
//     sys_open_handler does. They are NEVER inherited from the outgoing slot
//     -- inheriting would let a READ-only ctl fd become a WRITE-capable data
//     fd for free, which is a monotonicity break dressed as a convenience.
//   * The #844 lock discipline: the slot swap happens under t->lock so a peer
//     thread sees either the old object or the new one, never a torn slot;
//     the outgoing release runs OUTSIDE the lock because it may sleep
//     (spoor_clunk's Dev close hook).
//
// There is NO EL0 caller and there must not be one: this is reached only from
// the phenotype's socket translators, which have already validated the guest's
// arguments. Exposing it as a syscall would be a new ABI with none of the
// above applied by whoever called it.
int handle_replace(struct Proc *p, hidx_t h, enum kobj_kind kind,
                   rights_t rights, void *obj);

// Look up a handle into a caller-owned snapshot, with a reference HELD on
// the underlying kobj. Returns 0 on success (*out filled: kind / rights /
// obj, magic == HANDLE_MAGIC, and the obj's refcount bumped so it stays alive
// until the matching handle_put), -1 on failure (out-of-range h, empty slot,
// NULL/corrupted p, or NULL out -- *out zeroed, no ref held).
//
// #844: the lookup + the snapshot copy + the ref bump run under the per-Proc
// handle-table lock, so a sibling thread's concurrent handle_close cannot
// invalidate the read or free the obj under the caller. The OLD contract
// ("returns a live pointer into the table, valid until the slot is closed")
// was a TOCTOU in a multi-threaded Proc -- the slot pointer dangled and the
// obj could be freed across any use (esp. blocking dev I/O / accept / 9P
// handshake). Callers use out->obj (ref-held, safe across blocking ops) and
// MUST handle_put(out) on EVERY exit path once a get succeeds.
int handle_get(struct Proc *p, hidx_t h, struct Handle *out);

// Release the reference a successful handle_get acquired on h->obj. Runs the
// per-kind release OUTSIDE any handle-table lock (it may sleep -- spoor_clunk's
// Dev close hook, SrvConn teardown). NULL-safe + idempotent: a no-op on a
// NULL/zeroed snapshot (a failed handle_get), and it zeroes *h after release so
// a double handle_put is a no-op. NOT slot deletion -- that is handle_close
// (which drops the TABLE's ref); handle_put drops the CALLER's borrowed ref.
void handle_put(struct Handle *h);

// Duplicate a handle within p's table with possibly reduced rights.
//
// new_rights MUST be a subset of the parent's rights — elevation is
// rejected (-1 returned). This is the impl-side enforcement of the
// spec's RightsCeiling invariant: BuggyDupElevate produces a counter-
// example by adding bits not in parent's rights, which this check
// rejects at runtime.
//
// Returns the new slot index on success, -1 on:
//   - h out-of-range / empty slot
//   - new_rights NOT a subset of parent's rights (rights elevation)
//   - new_rights == 0 or has bits outside RIGHT_ALL
//   - table full
//
// Maps to specs/handles.tla::HandleDup(p, h, new_rights).
hidx_t handle_dup(struct Proc *p, hidx_t h, rights_t new_rights);

// LINEAGE L-3c: copy `src`'s handle table into `dst` -- the fd inheritance a
// forked child gets and a spawned child does not. Called from rfork_internal
// on the FORK shape only; `dst` must be a freshly-allocated, still-unpublished
// Proc whose table is empty.
//
// SLOT INDICES ARE PRESERVED: the parent's fd N becomes the child's fd N. That
// is POSIX, and it is why this cannot be a loop over handle_dup -- dup installs
// into the first free slot, so a single skipped handle would renumber every fd
// after it and the child's inherited stdout would land somewhere else entirely.
//
// A slot is copied only if `handle_slot_may_alias` admits it -- the SAME
// predicate handle_dup uses, because both create a second handle naming one
// object and so face the identical hazard. The consequence worth stating: a
// hardware handle (I-5), a /srv connection Spoor, and a Loom ring are NOT
// copied, and the child gets a HOLE at that index rather than the fork being
// refused. That is deliberate. I-5 is a property of the handle -- "pinned to
// the Proc that created it" -- not a property of forking, so the fork proceeds
// and the child simply does not hold what it cannot hold. Refusing the whole
// fork would instead punish a parent for holding a handle it never intended to
// pass, and would make a driver unable to create a process at all. A child that
// needs hardware authority gets it the way every other Proc does: the warden's
// confer path (the I-34 allowance), never fd inheritance.
//
// The hole is observable (the child sees EBADF at that index, and its next
// open lands there where Linux's would not), which is the honest report of an
// authority the child was never eligible to hold.
//
// Rights are carried verbatim (I-6 is satisfied by non-increasing, and POSIX
// fork gives the child the parent's access). Each copied slot takes its own
// reference on the underlying object; the child's handle_table_free releases
// them, so a failed rfork's proc_free rollback is already correct.
//
// Cannot fail: no allocation per slot, and the destination is known-empty and
// the same size. Returns the number of slots copied (a diagnostic, and what
// the regression tests assert on).
int handle_table_copy_into(struct Proc *dst, struct Proc *src);

// #151: dup with the parent's rights carried VERBATIM, installing at the lowest
// free index >= min_idx, and with the new descriptor's close-on-exec flag set to
// `cloexec`. This is POSIX dup / F_DUPFD / F_DUPFD_CLOEXEC; handle_dup above is
// the rights-REDUCING form the capability surface uses, and the two are kept
// separate rather than folded behind a sentinel because "0 rights" already means
// RIGHT_NONE and would have to mean its opposite here.
//
// `min_idx` is why this exists at all and is not a convenience. A shell's
// savefd() does F_DUPFD_CLOEXEC(fd, 10) precisely to move its bookkeeping fd out
// of the low range where a user redirection could collide with it. Returning the
// first free slot regardless would hand back fd 3 and break the guarantee the
// call was made to obtain -- silently, and only under a redirection.
//
// Rights are read under the same lock hold as the install, so a peer thread's
// close cannot land between reading the parent's rights and copying them.
//
// Returns the new index, or -1 on the same refusals handle_dup makes (empty
// slot, non-transferable kind, devsrv Spoor, table full) plus an out-of-range
// min_idx.
hidx_t handle_dup_posix(struct Proc *p, hidx_t h, hidx_t min_idx, bool cloexec);

// #151: read / write a slot's close-on-exec flag. Returns 0 or 1 (get) and 0
// (set) on success; -1 if h is out of range or names a free slot.
//
// A caller that wants an fd born with the flag set does alloc-then-set rather
// than passing it through handle_alloc, which would mean touching ~100 call
// sites for one bit. The window between the two is provably empty: only exec
// consumes the flag, execve refuses unless it is the Proc's only live thread
// (proc_exec_alone), and the caller IS a live thread mid-syscall -- so no exec
// can run between the alloc and the set.
int handle_set_cloexec(struct Proc *p, hidx_t h, bool on);
int handle_get_cloexec(struct Proc *p, hidx_t h);

// #151: close every handle whose close-on-exec flag is set. Returns the number
// closed. Called from execve AFTER the commit point, for two reasons that pull
// in the same direction: the closes may SLEEP (a Spoor's Dev close hook sends a
// 9P Tclunk), so this cannot run under any lock; and a failed exec must leave
// the process unchanged, so it must not run before the last thing that can fail.
// Linux places do_close_on_exec() after its own point of no return for the
// second reason.
int handle_close_on_exec(struct Proc *p);

// Type classifiers. Map to specs/handles.tla's TxKObjs / HwKObjs /
// SrvKObjs partitions. kobj_kind_is_transferable returns true for
// Process / Thread / BURROW / Spoor; kobj_kind_is_hw for MMIO / IRQ /
// DMA / Interrupt; kobj_kind_is_srv for Srv. Exactly one is true for
// every kind except KOBJ_INVALID (and any out-of-range value), for
// which all three return false.
bool kobj_kind_is_transferable(enum kobj_kind k);
bool kobj_kind_is_hw(enum kobj_kind k);
bool kobj_kind_is_srv(enum kobj_kind k);

// Per-Proc count of in-use handle slots (linear scan; for tests +
// diagnostics, not perf-critical).
int handle_table_count(const struct HandleTable *t);

// Diagnostics — cumulative cache statistics.
u64 handle_total_allocated(void);
u64 handle_total_freed(void);

#endif // THYLACINE_HANDLE_H
