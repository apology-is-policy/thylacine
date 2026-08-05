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
    // The per-Proc handle table is kmalloc-backed: sizeof(HandleTable) =
    // 8 + 24*PROC_HANDLE_MAX exceeds SLUB_MAX_OBJECT_SIZE (2048) at the
    // current PROC_HANDLE_MAX, so a dedicated slab cache cannot hold it --
    // kmalloc routes the oversize object through alloc_pages. Nothing to init.
}

struct HandleTable *handle_table_alloc(void) {
    // R5-F F50/F51: NO PANIC_ON_FAIL. The table allocates at proc_alloc,
    // reachable from rfork -- userspace OOM (a fork bomb) must not extinct
    // the kernel; caller-side rollback handles the NULL return per the
    // contract in handle.h. KP_ZERO makes every slot's magic = 0 (free).
    return kmalloc(sizeof(struct HandleTable), KP_ZERO);   // NULL on OOM
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
    // proc_free (all threads reaped), at orphan-table cleanup, AND (since
    // #926, generalized by #68) at BOTH at-exit close sites -- exits() and
    // thread_exit_self's last-out -- where proc_count_live_peers_locked == 0
    // was determined under g_proc_table_lock. thread_count may exceed 1
    // there (unreaped EXITING peers -- it decrements only at reap), but
    // exactly ONE live thread (the closer) exists, an EXITING peer's
    // residual execution (clear-child-tid handoff + sched()) never touches
    // the handle table, AND no production path ever touches a FOREIGN ALIVE
    // Proc's handle table -- every handle op derives its Proc from
    // current_thread()->proc (self). So the sole live thread freeing the
    // table cannot race a sibling. (The release MUST run lockless
    // regardless -- it may sleep.) FOOTGUN: a future cross-Proc handle
    // accessor -- e.g. a /proc/<pid>/fd surface inspecting a LIVE peer's
    // table -- breaks the self-only premise and would need the table lock
    // here AND coordination with the #926/#68 at-exit close. The
    // slot-zeroing is belt-and-suspenders before the kfree.
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
    kfree(t);
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

// #151: the close-on-exec bitmap accessors. Every one of them requires t->lock,
// which is why they take the table rather than the Proc -- the callers below all
// already hold it, and a variant that locked internally would invite a caller to
// read the bit outside the same critical section as the slot it describes.
static bool cloexec_get_locked(const struct HandleTable *t, hidx_t h) {
    return (t->cloexec[(u32)h / 64] >> ((u32)h % 64)) & 1u;
}

static void cloexec_set_locked(struct HandleTable *t, hidx_t h, bool on) {
    u64 bit = 1ull << ((u32)h % 64);
    if (on) t->cloexec[(u32)h / 64] |=  bit;
    else    t->cloexec[(u32)h / 64] &= ~bit;
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
//
// #151: `from` is the lowest index the scan may take (0 for every caller but
// F_DUPFD). The close-on-exec bit of the chosen slot is ESTABLISHED here, never
// inherited -- this is the single point every fd-creating path goes through, so
// writing it here is what makes a REUSED index unable to carry the previous
// occupant's flag. Writing it is also MANDATORY rather than defensive: it is how
// handle_dup_posix delivers F_DUPFD_CLOEXEC's requested value.
//
// handle_close clears the bit too, and MEASUREMENT (the #151 revert probes)
// showed the two OVERLAP COMPLETELY: removing either one alone leaves the whole
// suite green, and only removing BOTH trips `handles.cloexec_lifecycle`. So the
// no-inheritance property is doubly held, and no test can attribute it to one
// site. That is worth stating plainly rather than implying both are load-bearing
// -- the close-side clear is hygiene (it keeps "bit set => slot live" true, an
// invariant nothing currently reads) and is the one that could be dropped; this
// one cannot, because of the paragraph above.
static hidx_t handle_install_locked(struct HandleTable *t, enum kobj_kind kind,
                                    rights_t rights, void *obj, hidx_t from,
                                    bool cloexec) {
    if (from < 0) from = 0;
    for (int i = (int)from; i < PROC_HANDLE_MAX; i++) {
        if (t->slots[i].magic == 0) {
            t->slots[i].magic  = HANDLE_MAGIC;
            t->slots[i].kind   = kind;
            t->slots[i].rights = rights;
            t->slots[i].obj    = obj;
            cloexec_set_locked(t, (hidx_t)i, cloexec);
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
    hidx_t h = handle_install_locked(t, kind, rights, obj, 0, /*cloexec=*/false);
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
    // #151: the flag dies with the descriptor. Leaving it set would make the
    // NEXT fd to land on this index inherit a close-on-exec it never asked for
    // -- the reused-identity class this tree keeps meeting (a reused inode
    // serving a prior occupant's cached page, L1f F1). handle_install_locked
    // clears it as well; see its note on why both.
    cloexec_set_locked(t, h, false);
    __atomic_fetch_add(&g_handle_freed, 1, __ATOMIC_RELAXED);
    spin_unlock(&t->lock);

    handle_release_obj(kind, obj);
    return 0;
}

// VIVARIUM V-5 (docs/VIVARIUM.md section 5.5.1): swap a live slot's object in
// place. The header carries the rationale + the invariant argument; this body
// is close's capture-under-lock followed by an install into the SAME slot,
// with the outgoing release outside the lock exactly as close does it.
int handle_replace(struct Proc *p, hidx_t h, enum kobj_kind kind,
                   rights_t rights, void *obj) {
    struct HandleTable *t = proc_handles_or_extinct(p);

    if (h < 0 || h >= PROC_HANDLE_MAX)      return -1;
    if (!valid_alloc_args(kind, rights))    return -1;

    // The narrow gate (I-5 / I-6, header). Checked BEFORE the lock and before
    // anything is disturbed, so a refusal leaves the caller's reference intact
    // and the table untouched. The outgoing kind is checked under the lock,
    // where it can be read without racing a peer's close.
    if (kind != KOBJ_SPOOR)                 return -1;

    spin_lock(&t->lock);
    struct Handle *slot = &t->slots[h];
    if (slot->magic != HANDLE_MAGIC) {
        spin_unlock(&t->lock);
        return -1;
    }
    if (slot->kind != KOBJ_SPOOR) {
        // Refuse to replace anything but a Spoor. A hardware slot in
        // particular must not be reachable here: quietly dropping a
        // KObj_MMIO/IRQ/DMA/PCI handle out of a live fd is a lifetime event
        // I-5 gives no path for, and this primitive is not the place to
        // invent one.
        spin_unlock(&t->lock);
        return -1;
    }
    enum kobj_kind old_kind = slot->kind;
    void          *old_obj  = slot->obj;

    // Overwrite in place. The index does not change, which is the entire
    // point -- the guest is holding this number. Rights come from the caller
    // (derived from the incoming object's open mode); the outgoing slot's
    // rights are DISCARDED, never OR'd or inherited.
    //
    // #151: close-on-exec is deliberately NOT touched, and the reason is the
    // same fact that motivates the whole flag living in a per-slot bitmap: it
    // belongs to the DESCRIPTOR, and the descriptor is precisely what survives
    // here. A socket opened SOCK_CLOEXEC and then connect()ed must still be
    // close-on-exec afterwards; clearing it would silently revoke a guarantee
    // the guest obtained before the swap it never asked to observe.
    slot->kind   = kind;
    slot->rights = rights;
    slot->obj    = obj;
    spin_unlock(&t->lock);

    // May sleep (spoor_clunk's Dev close hook) -- outside the lock, and after
    // the new object is already installed, so the slot is never momentarily
    // empty for a peer thread to allocate into.
    handle_release_obj(old_kind, old_obj);
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

// May a SECOND handle be made to name this slot's object? Both callers create
// exactly that -- `handle_dup` a second slot in the SAME table, the LINEAGE
// L-3c fork copy a slot in ANOTHER Proc's table -- so both ask the identical
// question and must not answer it differently. That is why this is one
// predicate rather than two lists: a kind admitted here is admitted to both,
// and a kind added to `enum kobj_kind` is refused by both until someone
// deliberately classifies it.
//
// Caller holds the slot's table lock (the fields are read, not copied first).
static bool handle_slot_may_alias(const struct Handle *s) {
    // P4-Ib NoHwDup + P5-corvus-srv NoSrvDup: forbidden for every
    // NON-transferable kind. Maps to specs/handles.tla's HandleDup
    // precondition `h.kobj \in TxKObjs` -- the runtime guard is its exact
    // negation, `!kobj_kind_is_transferable`.
    //   - Hardware (MMIO / IRQ / DMA / Interrupt / PCI): drivers hold exactly
    //     one handle per hw resource -- extends I-5 to "non-duplicable at all",
    //     and across a fork that is I-5 proper (the handle is pinned to the
    //     Proc that created it; a child is a different Proc).
    //   - Srv: exactly one connection Spoor per Proc (CORVUS-DESIGN.md section 6.2).
    //   - Loom: a ring is pinned to the Proc whose handle table its REGISTERED
    //     handles index, so a copy would name the wrong table (Loom-2a).
    if (!kobj_kind_is_transferable(s->kind)) return false;

    // stalk-3b-beta NoSrvSpoorDup: a devsrv Spoor (dc='s' -- a /srv root, a
    // service-ref, or a CONNECTION endpoint) is pinned to its Proc; a second
    // handle naming the one connection would blur the SO_PEERCRED origin
    // (handles.tla SrvHandlesAtOrigin) and double-drive the SrvConn /
    // registry-ref lifecycle at close. dev9p roots (dc='9') stay dup-able.
    if (s->kind == KOBJ_SPOOR && s->obj && ((struct Spoor *)s->obj)->dc == 's')
        return false;

    return true;
}

// LINEAGE L-3c: copy `src`'s handle table into `dst`, PRESERVING slot indices.
// The header carries the contract; this body is `handle_dup`'s capture-acquire-
// install repeated per slot, with two differences that are the whole of it:
// the install goes to a FIXED index rather than the first free one (POSIX: the
// child's fd N is the parent's fd N, so a skipped slot must leave a hole, never
// renumber the ones after it), and the rights are carried VERBATIM rather than
// narrowed by a caller-supplied ceiling.
int handle_table_copy_into(struct Proc *dst, struct Proc *src) {
    struct HandleTable *dt = proc_handles_or_extinct(dst);
    struct HandleTable *st = proc_handles_or_extinct(src);
    // Not merely unexpected -- the two-lock hold below would deadlock on
    // itself, and a table cannot meaningfully be copied onto itself.
    if (dt == st) extinction("handle_table_copy_into: src and dst share a table");

    int copied = 0;

    // Lock order: SOURCE then DESTINATION. No cycle is possible because this is
    // the only two-table operation in the tree, and because `dst` is a Proc
    // rfork_internal has allocated but not yet linked into its parent's
    // children list -- nothing else can reach it to take its lock at all. The
    // destination lock is therefore uncontended by construction; it is taken
    // anyway so this stays correct if publication ever moves earlier.
    //
    // Holding the source lock across the WHOLE walk is what makes the child's
    // table a coherent snapshot: a peer thread of a multi-threaded parent
    // cannot close fd 3 and open a different object at 3 midway through
    // (the #844 shared-table hazard).
    spin_lock(&st->lock);
    spin_lock(&dt->lock);
    for (int i = 0; i < PROC_HANDLE_MAX; i++) {
        const struct Handle *s = &st->slots[i];
        if (s->magic != HANDLE_MAGIC) continue;
        if (!handle_slot_may_alias(s)) continue;
        if (dt->slots[i].magic != 0)
            extinction("handle_table_copy_into: destination slot not free");

        // The copy is a NEW reference; the child's own close (or its
        // handle_table_free at death) releases it. Non-blocking under the lock,
        // exactly as handle_dup does it.
        handle_acquire_obj(s->kind, s->obj);
        dt->slots[i].magic  = HANDLE_MAGIC;
        dt->slots[i].kind   = s->kind;
        // Rights VERBATIM. I-6 requires rights to be non-increasing across a
        // dup/transfer/endow, and equal satisfies that -- POSIX fork gives the
        // child the same access the parent had, and narrowing here would make
        // an inherited fd silently less capable than the one it was forked from.
        dt->slots[i].rights = s->rights;
        dt->slots[i].obj    = s->obj;
        // #151: fork PRESERVES close-on-exec -- POSIX, and the only reading that
        // makes the flag usable. A shell sets it on a bookkeeping fd once and
        // then forks per command; clearing it here would leak that fd into every
        // child, which is the exact thing it was set to prevent. (Linux keeps it
        // for the same reason: dup_fd copies the close_on_exec bitmap.) The
        // SKIPPED slots need nothing -- the destination table is freshly
        // KP_ZERO'd, so a hole's bit is already clear.
        cloexec_set_locked(dt, (hidx_t)i, cloexec_get_locked(st, (hidx_t)i));
        __atomic_fetch_add(&g_handle_allocated, 1, __ATOMIC_RELAXED);
        copied++;
    }
    spin_unlock(&dt->lock);
    spin_unlock(&st->lock);

    return copied;
}

// #151: the shared body of handle_dup (rights-reducing, first free slot, flag
// clear) and handle_dup_posix (rights verbatim, lowest slot >= min_idx, flag as
// asked). The three differences are parameters; everything else -- the
// non-transferable refusal, the devsrv-Spoor refusal, the one-lock-hold TOCTOU
// discipline, the rollback -- is identical and must stay that way, because both
// create a second handle naming one object and so face the same hazards.
static hidx_t handle_dup_common(struct Proc *p, hidx_t h, rights_t new_rights,
                                bool inherit_rights, hidx_t min_idx,
                                bool cloexec) {
    struct HandleTable *t = proc_handles_or_extinct(p);
    if (h < 0 || h >= PROC_HANDLE_MAX) return -1;
    if (min_idx < 0 || min_idx >= PROC_HANDLE_MAX) return -1;

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

    // #151: POSIX dup gives the new descriptor the SAME access as the old, so
    // the inherit form reads the rights here, under the one lock hold, rather
    // than making the caller do a handle_get first -- which would be the TOCTOU
    // this whole function is shaped to avoid. Equal rights satisfy I-6's
    // non-increasing requirement, exactly as handle_table_copy_into's verbatim
    // carry does.
    if (inherit_rights) new_rights = parent->rights;

    // Rights monotonic reduction: new_rights MUST be a subset of
    // parent->rights (specs/handles.tla RightsCeiling; rejects the
    // BuggyDupElevate counterexample). The inherit form passes it trivially,
    // and is deliberately NOT routed around the check -- a future change to
    // where `new_rights` comes from should still have to clear this bar.
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
    hidx_t child = handle_install_locked(t, kind, new_rights, obj, min_idx,
                                         cloexec);
    spin_unlock(&t->lock);

    if (child < 0) {
        // Roll back the acquire on table-full, OUTSIDE the lock (release may
        // sleep for a Spoor's last drop; harmless for the kinds dup allows).
        handle_release_obj(kind, obj);
    }
    return child;
}

hidx_t handle_dup(struct Proc *p, hidx_t h, rights_t new_rights) {
    return handle_dup_common(p, h, new_rights, /*inherit_rights=*/false,
                             /*min_idx=*/0, /*cloexec=*/false);
}

hidx_t handle_dup_posix(struct Proc *p, hidx_t h, hidx_t min_idx, bool cloexec) {
    return handle_dup_common(p, h, RIGHT_NONE, /*inherit_rights=*/true,
                             min_idx, cloexec);
}

// #157: dup onto a SPECIFIC index. The header carries the contract and why this
// is not close-then-dup; the body is handle_dup_common's capture-and-acquire
// followed by handle_replace's install-then-release-outside-the-lock, with the
// install going to a caller-named slot that may be free.
hidx_t handle_dup_to(struct Proc *p, hidx_t old, hidx_t new_h, bool cloexec) {
    struct HandleTable *t = proc_handles_or_extinct(p);
    if (old < 0   || old   >= PROC_HANDLE_MAX) return -1;
    if (new_h < 0 || new_h >= PROC_HANDLE_MAX) return -1;
    // Defence in depth: the caller returns EINVAL for this. Serving it here
    // would be nearly harmless (acquire, capture the same slot, install the
    // same values, release -- net zero) EXCEPT that it would set cloexec on a
    // descriptor POSIX says dup2 returns untouched. Refusing is one line.
    if (old == new_h) return -1;

    spin_lock(&t->lock);
    struct Handle *src = &t->slots[old];
    if (src->magic != HANDLE_MAGIC) {
        spin_unlock(&t->lock);
        return -1;
    }
    // The SAME predicate handle_dup and the fork copy use -- all three create a
    // second handle naming one object, so all three must answer identically.
    if (!handle_slot_may_alias(src)) {
        spin_unlock(&t->lock);
        return -1;
    }

    enum kobj_kind kind   = src->kind;
    rights_t       rights = src->rights;    // VERBATIM (POSIX; I-6 non-increasing)
    void          *obj    = src->obj;

    // The new descriptor is a NEW reference; its eventual close releases it.
    // Non-blocking under the lock, exactly as handle_dup does it.
    handle_acquire_obj(kind, obj);

    // Capture the outgoing occupant BEFORE overwriting. `had` is what decides
    // both the release below and the counter arithmetic: a dup3 onto a LIVE
    // slot destroys one descriptor and creates one, so the live count
    // (allocated - freed) must be unchanged; onto a FREE slot it creates one.
    struct Handle *dst      = &t->slots[new_h];
    bool           had      = (dst->magic == HANDLE_MAGIC);
    enum kobj_kind old_kind = had ? dst->kind : KOBJ_INVALID;
    void          *old_obj  = had ? dst->obj  : NULL;

    dst->magic  = HANDLE_MAGIC;
    dst->kind   = kind;
    dst->rights = rights;
    dst->obj    = obj;
    // #151: SET, not inherited. dup2's new descriptor is close-on-exec exactly
    // when the caller asked (dup3's O_CLOEXEC), regardless of what `old` or the
    // previous occupant of this index carried. Inheriting from `old` is the
    // classic dup2 mistake -- POSIX is explicit that the flag is cleared -- and
    // inheriting from the OCCUPANT would be the reused-identity failure
    // handle_install_locked's clear exists to prevent.
    cloexec_set_locked(t, new_h, cloexec);
    __atomic_fetch_add(&g_handle_allocated, 1, __ATOMIC_RELAXED);
    if (had) __atomic_fetch_add(&g_handle_freed, 1, __ATOMIC_RELAXED);
    spin_unlock(&t->lock);

    // May sleep (spoor_clunk's Dev close hook) -- outside the lock, and AFTER
    // the new object is installed, so the slot is never momentarily empty for a
    // peer thread to allocate into. handle_replace does it in this order for
    // the same reason.
    if (had) handle_release_obj(old_kind, old_obj);
    return new_h;
}

int handle_set_cloexec(struct Proc *p, hidx_t h, bool on) {
    struct HandleTable *t = proc_handles_or_extinct(p);
    if (h < 0 || h >= PROC_HANDLE_MAX) return -1;

    spin_lock(&t->lock);
    // The slot must be LIVE. Setting the flag on a free index would be a
    // promise about an fd that does not exist, and would then be inherited by
    // whatever opens next -- which is exactly the stale-flag failure
    // handle_install_locked's clear exists to prevent.
    if (t->slots[h].magic != HANDLE_MAGIC) {
        spin_unlock(&t->lock);
        return -1;
    }
    cloexec_set_locked(t, h, on);
    spin_unlock(&t->lock);
    return 0;
}

int handle_get_cloexec(struct Proc *p, hidx_t h) {
    struct HandleTable *t = proc_handles_or_extinct(p);
    if (h < 0 || h >= PROC_HANDLE_MAX) return -1;

    spin_lock(&t->lock);
    if (t->slots[h].magic != HANDLE_MAGIC) {
        spin_unlock(&t->lock);
        return -1;
    }
    int on = cloexec_get_locked(t, h) ? 1 : 0;
    spin_unlock(&t->lock);
    return on;
}

int handle_close_on_exec(struct Proc *p) {
    struct HandleTable *t = proc_handles_or_extinct(p);

    // Snapshot the bitmap AND clear it in one lock hold, then close outside.
    // Two properties come from doing it in that order rather than testing and
    // closing slot by slot:
    //
    //   - the closes may SLEEP (a Spoor's Dev close hook sends a 9P Tclunk), so
    //     they cannot happen under the lock at all;
    //   - having cleared the bits first, nothing can read them again and try to
    //     close the same slot twice, whatever a future caller does.
    //
    // The snapshot cannot go stale in the only caller that exists: execve
    // refuses unless this is the Proc's sole live thread (proc_exec_alone,
    // re-checked inside proc_exec_replace), so no peer is running to open or
    // close anything while this walks. The clear-first form does not DEPEND on
    // that, which is the point of writing it this way.
    u64 pending[HANDLE_CLOEXEC_WORDS];
    spin_lock(&t->lock);
    for (u32 w = 0; w < HANDLE_CLOEXEC_WORDS; w++) {
        pending[w]    = t->cloexec[w];
        t->cloexec[w] = 0;
    }
    spin_unlock(&t->lock);

    int closed = 0;
    for (u32 w = 0; w < HANDLE_CLOEXEC_WORDS; w++) {
        while (pending[w]) {
            u32 b = (u32)__builtin_ctzll(pending[w]);
            pending[w] &= pending[w] - 1;
            hidx_t h = (hidx_t)(w * 64 + b);
            if (h >= PROC_HANDLE_MAX) break;   // a partial trailing word
            // A -1 here means the slot was already free. That is not reachable
            // today (the bit is cleared whenever a slot is freed), so it is
            // ignored rather than counted: this returns how many fds the exec
            // actually closed, which is what a test can assert on.
            if (handle_close(p, h) == 0) closed++;
        }
    }
    return closed;
}

u64 handle_total_allocated(void) { return __atomic_load_n(&g_handle_allocated, __ATOMIC_RELAXED); }
u64 handle_total_freed(void)     { return __atomic_load_n(&g_handle_freed, __ATOMIC_RELAXED); }
