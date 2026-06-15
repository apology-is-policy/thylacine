// Kernel handle table impl (P2-Fc).
//
// Per ARCHITECTURE.md §18 + specs/handles.tla. Implements the spec's
// HandleAlloc / HandleClose / HandleDup actions on a per-Proc fixed-size
// SLUB-allocated HandleTable.
//
// State invariants (from specs/handles.tla, TLC-checked):
//   RightsCeiling     — handle.rights subset of origin_rights[handle.kobj].
//                       Enforced by handle_dup's subset check before insert.
//   HwHandlesAtOrigin — hw-typed handles non-transferable. Enforced by
//                       handle_transfer_via_9p's omitted switch cases
//                       (Phase 4); structurally pinned by the
//                       _Static_assert on KOBJ_KIND_COUNT.
//   OnlyTransferVia9P — no direct cross-Proc transfer syscall exists.
//                       Enforced by absence (no impl).
//   CapsCeiling       — proc-level capability mask reduces only
//                       monotonically. Forward-looking; rfork mask is
//                       AND-only at Phase 5+ syscall surface.
//
// At v1.0 P2-Fc:
//   - Per-Proc table is fixed-size (PROC_HANDLE_MAX = 64 slots).
//   - No underlying-kobj refcount integration.
//   - No transfer-via-9P codepath; Phase 4 wires it.
//   - No syscall surface; in-kernel callers only.
//
// Phase 5+ refinement:
//   - Growable table via RB-tree keyed by hidx_t.
//   - Capability-handle elevation via factotum (per ARCH §15.4).
//
// #844 (handle-lifetime pass): the per-Proc handle-table lock for SMP safety
// is now LANDED -- struct HandleTable::lock serializes alloc / close / get /
// dup. handle_get takes a snapshot + bumps the obj refcount UNDER the lock and
// returns it by value; handle_put drops that borrowed ref OUTSIDE the lock.
// This closes the multi-thread-Proc TOCTOU where a sibling handle_close freed
// a slot's obj / zeroed the slot under a concurrent handle_get's live pointer
// (esp. across blocking dev I/O / accept / 9P handshake). Per-kind refcount
// integration (burrow / spoor / srvconn / mmio / irq / dma) is wired via
// handle_acquire_obj / handle_release_obj; all are SMP-safe (burrow gained a
// per-Burrow lock in #847, the rest are atomic ACQ_REL).

#include <thylacine/devsrv.h>
#include <thylacine/dma_handle.h>
#include <thylacine/extinction.h>
#include <thylacine/handle.h>
#include <thylacine/irqfwd.h>
#include <thylacine/loom.h>
#include <thylacine/mmio_handle.h>
#include <thylacine/pci_handle.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>
#include <thylacine/srvconn.h>
#include <thylacine/types.h>
#include <thylacine/burrow.h>

#include "../mm/slub.h"

static struct kmem_cache *g_handle_table_cache;
static u64                g_handle_allocated;
static u64                g_handle_freed;

// =============================================================================
// Type classifiers — implement the spec's TxKObjs / HwKObjs partition.
// =============================================================================

bool kobj_kind_is_transferable(enum kobj_kind k) {
    if (k <= KOBJ_INVALID || k >= KOBJ_KIND_COUNT) return false;
    return ((1u << (unsigned)k) & KOBJ_KIND_TRANSFERABLE_MASK) != 0;
}

bool kobj_kind_is_hw(enum kobj_kind k) {
    if (k <= KOBJ_INVALID || k >= KOBJ_KIND_COUNT) return false;
    return ((1u << (unsigned)k) & KOBJ_KIND_HW_MASK) != 0;
}

bool kobj_kind_is_srv(enum kobj_kind k) {
    if (k <= KOBJ_INVALID || k >= KOBJ_KIND_COUNT) return false;
    return ((1u << (unsigned)k) & KOBJ_KIND_SRV_MASK) != 0;
}

// =============================================================================
// HandleTable lifecycle.
// =============================================================================

void handle_init(void) {
    if (g_handle_table_cache) extinction("handle_init called twice");

    // R5-F F50/F51 close: NO PANIC_ON_FAIL. Per-Proc handle tables
    // allocate at proc_alloc, which is reachable from rfork — userspace
    // OOM (a fork bomb) shouldn't extinct the kernel. Caller-side
    // rollback paths handle the NULL return per the documented
    // contract in handle.h.
    g_handle_table_cache = kmem_cache_create("handle_table",
                                             sizeof(struct HandleTable),
                                             8,
                                             0);
    if (!g_handle_table_cache) {
        extinction("kmem_cache_create(handle_table) returned NULL");
    }
}

struct HandleTable *handle_table_alloc(void) {
    if (!g_handle_table_cache)
        extinction("handle_table_alloc before handle_init");
    // KP_ZERO ensures every slot's magic = 0 (free) at allocation.
    struct HandleTable *t = kmem_cache_alloc(g_handle_table_cache, KP_ZERO);
    return t;   // NULL on OOM
}

// Per-kind release of the underlying kernel object reference. Called
// from handle_close (caller passes a Proc) and handle_table_free
// (orphan path; no Proc available — table outlives it).
//
// At v1.0 P2-Fd: KOBJ_BURROW has refcount integration via burrow_unref;
// other kinds are no-op. As subsystems land:
//   KOBJ_SPOOR     — Phase 4 (spoor_unref via 9P client).
//   KOBJ_MMIO     — landed P4-Ib (kobj_mmio_unref releases PA claim).
//   KOBJ_IRQ      — landed P4-Ib (kobj_irq_unref disables GIC INTID).
//   KOBJ_DMA      — landed P4-Ic5b1b (kobj_dma_unref releases page chunk).
//   KOBJ_PROCESS  — Phase 5+ (proc_unref when struct Proc gets refs).
//   KOBJ_THREAD   — Phase 5+.
//   KOBJ_INTERRUPT — Phase 5+.
//
// R5-F F54 close: explicit `default:` extincts on out-of-enum kind to
// catch memory corruption (the static_assert defends at compile time;
// the default arm defends at runtime).
static void handle_release_obj(enum kobj_kind kind, void *obj) {
    if (!obj) return;
    switch (kind) {
    case KOBJ_BURROW:
        burrow_unref((struct Burrow *)obj);
        break;
    case KOBJ_MMIO:
        // P4-Ib: KObj_MMIO release drops the refcount; the last unref
        // releases the PA-range claim back to g_mmio_claims so a
        // subsequent kobj_mmio_create can reuse the range.
        kobj_mmio_unref((struct KObj_MMIO *)obj);
        break;
    case KOBJ_IRQ:
        // P4-Ib: KObj_IRQ release drops the refcount; the last unref
        // disables the IRQ + unregisters the GIC handler. Existing
        // refcount machinery from P4-G (kernel/irqfwd.c).
        kobj_irq_unref((struct KObj_IRQ *)obj);
        break;
    case KOBJ_DMA:
        // P4-Ic5b1b: KObj_DMA release drops the refcount; the last
        // unref calls free_pages on the underlying buddy chunk. The
        // Burrow's separate reference (if any) is dropped independently
        // via burrow_free_internal's DMA case; either side dropping
        // last triggers the free.
        kobj_dma_unref((struct KObj_DMA *)obj);
        break;
    case KOBJ_SPOOR:
        // P5-fd-pipe: KOBJ_SPOOR release drops the Spoor's per-handle
        // reference. spoor_clunk runs the Dev's close hook (which sets
        // pipe EOF + wakes the other side per P5-pipe-blocking) and
        // then unrefs the Spoor; ref hits 0 → underlying Spoor freed.
        // Closing one end of a pipe through the handle table now
        // exercises the full lifecycle end-to-end.
        spoor_clunk((struct Spoor *)obj);
        break;
    case KOBJ_SRV: {
        // A KObj_Srv handle's obj is discriminated by the magic word at
        // offset 0. Post-stalk-3c a KObj_Srv handle is ONLY a service
        // listener:
        //   SRV_SERVICE_MAGIC — a service registry entry from create=post
        //     (devsrv_post_listener). Its lifetime is the poster Proc's
        //     (tombstoned by exits() -> srv_proc_exit_notify, never freed),
        //     NOT the handle's — closing the handle must not touch it. No-op.
        //   SRV_CONN_MAGIC — a SrvConn. Before stalk-3c a client connection
        //     was a KObj_Srv handle (the retired SYS_SRV_CONNECT); now the
        //     connection ENDPOINTS are KOBJ_SPOOR conn Spoors (released via
        //     spoor_clunk -> devsrv_close), so this arm no longer fires for a
        //     KObj_Srv handle. It is RETAINED as a UAF/corruption guard +
        //     belt-and-suspenders close (CORVUS-DESIGN.md §6.2): tear the
        //     connection down — EOF both rings so the peer wakes — then
        //     release the reference; teardown is idempotent.
        //
        //     kernel_attached exception (16c): when srvconn_is_kernel_attached
        //     is true (SYS_ATTACH_9P_SRV wrapped the conn in a kernel 9P
        //     client) teardown would break the FS attach — the c2s/s2c rings
        //     are still in use. Skip teardown; only unref. The connection
        //     tears down when the LAST KOBJ_SPOOR handle referencing the
        //     attach session closes (the adapter's transport.close at
        //     p9_attached_destroy runs srvconn_teardown + srvconn_unref).
        u64 m = *(const u64 *)obj;
        if (m == SRV_CONN_MAGIC) {
            struct SrvConn *cn = (struct SrvConn *)obj;
            if (!srvconn_is_kernel_attached(cn)) {
                srvconn_teardown(cn);
            }
            srvconn_unref(cn);
        } else if (m != SRV_SERVICE_MAGIC) {
            extinction("handle_release_obj(KOBJ_SRV): obj has neither "
                       "service nor connection magic (corruption / UAF)");
        }
        break;
    }
    case KOBJ_INVALID:
    case KOBJ_PROCESS:
    case KOBJ_THREAD:
    case KOBJ_INTERRUPT:
    case KOBJ_KIND_COUNT:
        // No refcount integration yet for these. KOBJ_PROCESS /
        // KOBJ_THREAD wait for struct Proc / Thread to gain refs
        // (Phase 5+); KOBJ_INTERRUPT lands with the Phase 5+
        // interrupt-eventfd surface.
        break;
    case KOBJ_LOOM:
        // Loom-2a: the last handle drop tears down the ring. loom_unref's last
        // ref clunks every registered Spoor + burrow_unref's the ring Burrow
        // (releasing the kernel's handle_count; a lingering user mapping's
        // mapping_count independently keeps the pages until its VMA tears down).
        loom_unref((struct Loom *)obj);
        break;
    case KOBJ_PCI:
        // pci-1b: the last handle drop releases the claimed PCI function --
        // kobj_pci_unref's last ref quiesces the device + drops each assigned
        // BAR's KObj_MMIO ref (a live user mapping's burrow ref independently
        // keeps that BAR's pages alive until its VMA tears down -- #847 dual
        // lifetime) + frees the (bus,dev,fn) exclusivity slot.
        kobj_pci_unref((struct KObj_PCI *)obj);
        break;
    default:
        extinction("handle_release_obj: out-of-enum kobj_kind (memory corruption?)");
    }
}

// Per-kind acquire of the underlying kernel object reference. Called
// from handle_dup (the dup'd handle is a NEW reference; must be ref'd
// to balance the future close).
//
// R5-F F54 close: explicit `default:` extincts on out-of-enum kind.
static void handle_acquire_obj(enum kobj_kind kind, void *obj) {
    if (!obj) return;
    switch (kind) {
    case KOBJ_BURROW:
        burrow_ref((struct Burrow *)obj);
        break;
    case KOBJ_MMIO:
        // P4-Ib: in v1.0 dup is rejected for KOBJ_MMIO (NoHwDup), so
        // handle_acquire_obj will never be called with KOBJ_MMIO from
        // handle_dup. Including the case anyway as defense-in-depth +
        // forward-compatibility for the Phase 5+ transfer-via-9P path
        // (which is also forbidden for hw kinds per HwHandlesAtOrigin,
        // but the static_assert pins that).
        kobj_mmio_ref((struct KObj_MMIO *)obj);
        break;
    case KOBJ_IRQ:
        // P4-Ib: same rationale as KOBJ_MMIO above.
        kobj_irq_ref((struct KObj_IRQ *)obj);
        break;
    case KOBJ_DMA:
        // P4-Ic5b1b: same rationale as KOBJ_MMIO above (NoHwDup forbids
        // dup of hw kinds; this case is dead code at v1.0 but kept for
        // defense-in-depth + matching switch coverage so the static
        // assert on KIND_COUNT doesn't have to be revisited).
        kobj_dma_ref((struct KObj_DMA *)obj);
        break;
    case KOBJ_SPOOR:
        // P5-fd-pipe: KOBJ_SPOOR acquire bumps the Spoor refcount so
        // the dup'd handle has its own reference. Each holder
        // independently releases via spoor_clunk on close.
        spoor_ref((struct Spoor *)obj);
        break;
    case KOBJ_SRV:
        // RW-5 R1-F1: handle_GET reaches this (the #844 snapshot bumps the obj
        // refcount under the table lock); handle_dup does NOT (NoSrvDup rejects).
        // The no-op acquire is BALANCED with handle_release_obj's KOBJ_SRV arm
        // ONLY because post-stalk-3c a KObj_Srv handle is always a SERVICE
        // listener (SRV_SERVICE_MAGIC -> no-op release). If a KObj_Srv handle
        // ever again named a ref-counted SrvConn (SRV_CONN_MAGIC -> teardown +
        // srvconn_unref), this no-op would underflow the get/put pairing (a UAF)
        // -- it would then need a real srvconn_ref here (cf. the KOBJ_LOOM arm,
        // which DOES ref because a Loom obj is ref-counted).
        break;
    case KOBJ_INVALID:
    case KOBJ_PROCESS:
    case KOBJ_THREAD:
    case KOBJ_INTERRUPT:
    case KOBJ_KIND_COUNT:
        break;
    case KOBJ_LOOM:
        // Loom-2a: KObj_Loom is non-dup-able (not in TRANSFERABLE_MASK, so
        // handle_dup rejects it), but handle_GET also acquires -- it holds a
        // ref so the snapshot's obj stays alive across the borrow, paired with
        // handle_put's loom_unref. So this MUST bump (a no-op would underflow
        // the refcount on the get/put pairing and free the ring early).
        loom_ref((struct Loom *)obj);
        break;
    case KOBJ_PCI:
        // pci-1b: KObj_PCI is non-dup-able (in HW_MASK, so handle_dup rejects
        // it), but handle_GET acquires -- it holds a ref so the snapshot's obj
        // stays alive across the borrow (esp. the pci-1c MAP_BAR / INFO paths),
        // paired with handle_put's kobj_pci_unref. MUST bump (a no-op would
        // underflow the get/put refcount and free the claim early), exactly like
        // KOBJ_LOOM (both name ref-counted objects).
        kobj_pci_ref((struct KObj_PCI *)obj);
        break;
    default:
        extinction("handle_acquire_obj: out-of-enum kobj_kind (memory corruption?)");
    }
}

void handle_table_free(struct HandleTable *t) {
    if (!t) return;

    // Close any in-use handles before releasing the table. Calls the
    // per-kind release path so underlying refcounts (KOBJ_BURROW at P2-Fd)
    // are decremented correctly even on orphan-table cleanup.
    //
    // #844: NO table lock here -- this is the teardown path. It is reached at
    // proc_free (thread_count == 0), at orphan-table cleanup, AND (since #926)
    // at SINGLE-thread exit (exits(), thread_count == 1, Proc still ALIVE).
    // It is lockless-safe because at EVERY one of those call sites the Proc
    // has at most ONE live thread (thread_count <= 1) AND no production path
    // ever touches a FOREIGN ALIVE Proc's handle table -- every handle op
    // derives its Proc from current_thread()->proc (self). So the sole thread
    // freeing the table cannot race a sibling. (The release MUST run lockless
    // regardless -- it may sleep.) FOOTGUN: a future cross-Proc handle
    // accessor -- e.g. a /proc/<pid>/fd surface inspecting a LIVE peer's
    // table -- breaks premise (b) and would need the table lock here AND
    // coordination with the #926 at-exit close. The slot-zeroing is
    // belt-and-suspenders before the cache free.
    for (int i = 0; i < PROC_HANDLE_MAX; i++) {
        if (t->slots[i].magic == HANDLE_MAGIC) {
            handle_release_obj(t->slots[i].kind, t->slots[i].obj);
            t->slots[i].magic  = 0;
            t->slots[i].kind   = KOBJ_INVALID;
            t->slots[i].rights = RIGHT_NONE;
            t->slots[i].obj    = NULL;
            __atomic_fetch_add(&g_handle_freed, 1, __ATOMIC_RELAXED);
        }
    }
    kmem_cache_free(g_handle_table_cache, t);
}

int handle_table_count(const struct HandleTable *t) {
    if (!t) return 0;
    int n = 0;
    for (int i = 0; i < PROC_HANDLE_MAX; i++) {
        if (t->slots[i].magic == HANDLE_MAGIC) n++;
    }
    return n;
}

// =============================================================================
// Handle ops.
// =============================================================================

static struct HandleTable *proc_handles_or_extinct(struct Proc *p) {
    if (!p)                             extinction("handle op on NULL Proc");
    if (p->magic != PROC_MAGIC)         extinction("handle op on corrupted Proc");
    if (!p->handles)                    extinction("handle op on Proc with NULL handle table");
    return p->handles;
}

// Validate kind + rights for an alloc/dup. Returns true if all checks
// pass; false if the caller should reject with -1.
static bool valid_alloc_args(enum kobj_kind kind, rights_t rights) {
    if (kind <= KOBJ_INVALID || kind >= KOBJ_KIND_COUNT) return false;
    if (rights == RIGHT_NONE) return false;
    if (rights & ~RIGHT_ALL) return false;
    return true;
}

// #844: find a free slot and install (kind, rights, obj). Caller holds
// t->lock. Returns the slot index, or -1 if the table is full. Does NOT
// touch the underlying-kobj refcount -- the caller accounts for it (the
// burrow_create_anon-consumed-ref convention for alloc; the
// handle_acquire_obj bump for dup).
static hidx_t handle_install_locked(struct HandleTable *t, enum kobj_kind kind,
                                    rights_t rights, void *obj) {
    for (int i = 0; i < PROC_HANDLE_MAX; i++) {
        if (t->slots[i].magic == 0) {
            t->slots[i].magic  = HANDLE_MAGIC;
            t->slots[i].kind   = kind;
            t->slots[i].rights = rights;
            t->slots[i].obj    = obj;
            __atomic_fetch_add(&g_handle_allocated, 1, __ATOMIC_RELAXED);
            return (hidx_t)i;
        }
    }
    return -1;
}

hidx_t handle_alloc(struct Proc *p, enum kobj_kind kind,
                    rights_t rights, void *obj) {
    struct HandleTable *t = proc_handles_or_extinct(p);

    if (!valid_alloc_args(kind, rights)) return -1;

    // R5-F F49 close: defensive runtime check for KOBJ_BURROW. The
    // contract is "the caller has already accounted for one ref"
    // (typically: burrow_create_anon's initial handle_count=1, OR a
    // pre-call burrow_ref from a future Phase 4 transfer-via-9p path).
    // Future-buggy callers that pass a Burrow without holding a ref
    // would create a slot with no backing count — the first
    // handle_close would underflow burrow_unref's count check.
    //
    // Catching here makes the convention enforced at runtime, not
    // just documented. The check is a no-op for non-KOBJ_BURROW kinds
    // and for NULL obj (test paths).
    if (kind == KOBJ_BURROW && obj) {
        struct Burrow *v = (struct Burrow *)obj;
        if (v->magic != VMO_MAGIC)
            extinction("handle_alloc(KOBJ_BURROW): obj has bad magic (not a Burrow or UAF)");
        if (v->handle_count <= 0)
            extinction("handle_alloc(KOBJ_BURROW): caller has not accounted for "
                       "the slot's reference (call burrow_ref first, or use the "
                       "burrow_create_anon-consumed-reference convention)");
    }

    // RW-5 R1-F2: the KOBJ_LOOM analog of the KOBJ_BURROW guard above. Like a
    // Burrow, a Loom carries the consumed-ref convention (loom_create returns
    // refcount = 1, the handle's ref). A caller installing a Loom with no
    // accounted ref would underflow the get/put + close refcount. Defense in
    // depth (the sole loom caller is correct), matching the burrow arm.
    if (kind == KOBJ_LOOM && obj) {
        struct Loom *l = (struct Loom *)obj;
        if (l->magic != LOOM_MAGIC)
            extinction("handle_alloc(KOBJ_LOOM): obj has bad magic (not a Loom or UAF)");
        if (__atomic_load_n(&l->refcount, __ATOMIC_ACQUIRE) <= 0)
            extinction("handle_alloc(KOBJ_LOOM): caller has not accounted for the "
                       "slot's reference (loom_create's initial refcount=1 convention)");
    }

    // #844 audit F1: scan + install UNDER the table lock, via the same
    // handle_install_locked that handle_dup uses. Without the lock, two peer
    // threads of one Proc (stratumd) doing concurrent fd-creating syscalls --
    // or one in handle_alloc + one in handle_dup -- could both pick the same
    // free slot and the second write clobbers the first: two fds naming the
    // slot, one obj's table ref leaked, then UAF/double-release at close. This
    // is the primary fd-creating path; it must serialize exactly like
    // get/close/dup. The g_handle_allocated bump inside handle_install_locked
    // is atomic (closes the F4 diagnostics race too).
    spin_lock(&t->lock);
    hidx_t h = handle_install_locked(t, kind, rights, obj);
    spin_unlock(&t->lock);
    return h;   // -1 if the table is full
}

int handle_close(struct Proc *p, hidx_t h) {
    struct HandleTable *t = proc_handles_or_extinct(p);

    if (h < 0 || h >= PROC_HANDLE_MAX)  return -1;

    // #844: capture + zero the slot UNDER the table lock; run the per-kind
    // release (which may sleep -- spoor_clunk's Dev close hook, SrvConn
    // teardown) OUTSIDE the lock. A concurrent handle_get either observes the
    // live slot (and takes its OWN ref before this release) or the zeroed slot
    // (and fails) -- never a torn read, never a freed obj under a live ref.
    spin_lock(&t->lock);
    struct Handle *slot = &t->slots[h];
    if (slot->magic != HANDLE_MAGIC) {
        spin_unlock(&t->lock);
        return -1;
    }
    enum kobj_kind kind = slot->kind;
    void          *obj  = slot->obj;
    slot->magic  = 0;
    slot->kind   = KOBJ_INVALID;
    slot->rights = RIGHT_NONE;
    slot->obj    = NULL;
    __atomic_fetch_add(&g_handle_freed, 1, __ATOMIC_RELAXED);
    spin_unlock(&t->lock);

    handle_release_obj(kind, obj);
    return 0;
}

int handle_get(struct Proc *p, hidx_t h, struct Handle *out) {
    // Zero the snapshot first so every -1 path leaves a clean, put-safe struct
    // (handle_put treats magic != HANDLE_MAGIC as a no-op).
    if (out) {
        out->magic  = 0;
        out->kind   = KOBJ_INVALID;
        out->rights = RIGHT_NONE;
        out->obj    = NULL;
    }
    // Graceful (non-extinct) failure -- matches the old NULL-returning
    // contract. Handle lookups on syscall entry must fail closed on a bad fd /
    // bad Proc, never extinct.
    if (!out) return -1;
    if (!p || p->magic != PROC_MAGIC || !p->handles) return -1;
    if (h < 0 || h >= PROC_HANDLE_MAX) return -1;

    struct HandleTable *t = p->handles;
    spin_lock(&t->lock);
    struct Handle *slot = &t->slots[h];
    if (slot->magic != HANDLE_MAGIC) {
        spin_unlock(&t->lock);
        return -1;
    }
    // Bump the obj refcount UNDER the lock (non-blocking) so the snapshot's obj
    // outlives the lock release + any concurrent handle_close; then copy the
    // snapshot by value. The caller drops this borrowed ref via handle_put.
    handle_acquire_obj(slot->kind, slot->obj);
    *out = *slot;
    spin_unlock(&t->lock);
    return 0;
}

void handle_put(struct Handle *h) {
    if (!h) return;
    if (h->magic != HANDLE_MAGIC) return;   // zeroed / already-put / failed-get
    handle_release_obj(h->kind, h->obj);
    // Zero the snapshot so a double handle_put is a no-op.
    h->magic  = 0;
    h->kind   = KOBJ_INVALID;
    h->rights = RIGHT_NONE;
    h->obj    = NULL;
}

hidx_t handle_dup(struct Proc *p, hidx_t h, rights_t new_rights) {
    struct HandleTable *t = proc_handles_or_extinct(p);
    if (h < 0 || h >= PROC_HANDLE_MAX) return -1;

    // #844: validate the parent + acquire the child's ref + install the child
    // slot under ONE table-lock hold, so a concurrent handle_close of the
    // parent cannot slip between the lookup and the dup (the old code called
    // handle_get then handle_alloc -- two separate lock acquisitions with a
    // TOCTOU window). handle_acquire_obj is non-blocking (atomic / per-Burrow-
    // lock bump); only the alloc-failure rollback release runs after unlock.
    spin_lock(&t->lock);
    struct Handle *parent = &t->slots[h];
    if (parent->magic != HANDLE_MAGIC) {
        spin_unlock(&t->lock);
        return -1;
    }

    // P4-Ib NoHwDup + P5-corvus-srv NoSrvDup: dup is forbidden for every
    // NON-transferable kind. Maps to specs/handles.tla's HandleDup
    // precondition `h.kobj \in TxKObjs` — the runtime guard is its exact
    // negation, `!kobj_kind_is_transferable`.
    //   - Hardware (MMIO / IRQ / DMA / Interrupt): drivers hold exactly one
    //     handle per hw resource — extends I-5 to "non-duplicable at all".
    //   - Srv: exactly one connection Spoor per Proc (CORVUS-DESIGN.md §6.2).
    if (!kobj_kind_is_transferable(parent->kind)) {
        spin_unlock(&t->lock);
        return -1;
    }

    // stalk-3b-β NoSrvSpoorDup: a devsrv Spoor (dc='s' -- a /srv root, a
    // service-ref, or a CONNECTION endpoint) is pinned to its Proc; a second
    // handle naming the one connection would blur the SO_PEERCRED origin
    // (handles.tla SrvHandlesAtOrigin) and double-drive the SrvConn /
    // registry-ref lifecycle at close. dev9p roots (dc='9') stay dup-able.
    if (parent->kind == KOBJ_SPOOR && parent->obj &&
        ((struct Spoor *)parent->obj)->dc == 's') {
        spin_unlock(&t->lock);
        return -1;
    }

    // Rights monotonic reduction: new_rights MUST be a subset of
    // parent->rights (specs/handles.tla RightsCeiling; rejects the
    // BuggyDupElevate counterexample).
    if ((new_rights & parent->rights) != new_rights) {
        spin_unlock(&t->lock);
        return -1;
    }
    if (!valid_alloc_args(parent->kind, new_rights)) {
        spin_unlock(&t->lock);
        return -1;
    }

    // Capture BEFORE install (the install skips the occupied parent slot, so
    // `parent` stays valid -- but capturing is the clean, defensive form).
    enum kobj_kind kind = parent->kind;
    void          *obj  = parent->obj;

    // The dup'd handle is a NEW reference; its eventual handle_close releases
    // it. Acquire under the lock (non-blocking).
    handle_acquire_obj(kind, obj);
    hidx_t child = handle_install_locked(t, kind, new_rights, obj);
    spin_unlock(&t->lock);

    if (child < 0) {
        // Roll back the acquire on table-full, OUTSIDE the lock (release may
        // sleep for a Spoor's last drop; harmless for the kinds dup allows).
        handle_release_obj(kind, obj);
    }
    return child;
}

u64 handle_total_allocated(void) { return __atomic_load_n(&g_handle_allocated, __ATOMIC_RELAXED); }
u64 handle_total_freed(void)     { return __atomic_load_n(&g_handle_freed, __ATOMIC_RELAXED); }
