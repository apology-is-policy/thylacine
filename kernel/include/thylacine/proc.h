// Process descriptor — Plan 9 Proc struct, Thylacine adaptation.
//
// Per ARCHITECTURE.md §7.2 + §7.4 + §7.9. A Proc owns an address space,
// territory, fd table, handle table, credentials, threads, notes,
// parent/child links. The struct grows by appending; existing field
// offsets stay stable so incremental sub-chunks don't churn the SLUB
// cache size. New fields default to zero/NULL via KP_ZERO at allocation
// (P2-A audit R4 F40 close).
//
// History:
//   P2-A:  pid + threads list + thread_count.
//   P2-D:  + state (ALIVE / ZOMBIE) + parent + children + sibling
//          + exit_status + exit_msg + child_done Rendez.
//   P2-E:  + territory pointer (Territory).
//   P2-F:  + handle table head.
//   P2-G:  + address space (page table root, vma_tree) + credentials
//          + capability bitmask + notes queue.

#ifndef THYLACINE_PROC_H
#define THYLACINE_PROC_H

#include <thylacine/caps.h>     // caps_t (P4-Ic3 rfork_with_caps signature)
#include <thylacine/page.h>     // paddr_t (P3-Bcb pgtable_root)
#include <thylacine/rendez.h>
#include <thylacine/spinlock.h>
#include <thylacine/types.h>

struct Thread;

// PROC_MAGIC — sentinel set at proc_alloc / proc_init; checked at proc_free.
// Sits at offset 0 so SLUB's `*(void **)obj = freelist` write on
// kmem_cache_free naturally clobbers it; a subsequent proc_free reads the
// clobbered value, sees magic != PROC_MAGIC, and extincts with a clear
// double-free diagnostic (P2-A audit R4 F42 close).
#define PROC_MAGIC 0x50524F43C0DEFADEULL    // 'PROC' || 0xC0DE'FADE

// P2-D: Proc lifecycle state.
//
// INVALID  — zero-initialized; not usable. Detectable via state == 0
//            (parallels THREAD_STATE_INVALID).
// ALIVE    — has at least one running/runnable/sleeping thread, OR is
//            in the act of exiting but hasn't reached ZOMBIE yet.
// ZOMBIE   — all threads have terminated; exit_status + exit_msg are
//            set; waiting to be reaped by parent's wait().
//
// REAPED is not a state — by the time wait() returns the child PID,
// the Proc descriptor has been kmem_cache_freed and the magic is
// clobbered.
enum proc_state {
    PROC_STATE_INVALID = 0,
    PROC_STATE_ALIVE   = 1,
    PROC_STATE_ZOMBIE  = 2,
};

_Static_assert(PROC_STATE_INVALID == 0,
               "PROC_STATE_INVALID == 0 is the invariant that makes "
               "zero-initialized Proc structs detectably invalid");

struct Territory;
struct HandleTable;
struct Vma;

// A-1a: identity model (docs/IDENTITY-DESIGN.md §3.3 + §9.1; ARCH §28 I-22).
//
// A Proc carries a DURABLE identity (principal_id + groups) that is
// INHERITED across rfork/spawn — distinct from `caps` (subset-reduced)
// and `stripes` (fresh per Proc). corvus is the authority for the full
// id<->name<->groups<->keys mapping; the kernel caches the active set.
//
// Reserved ids/gids, NONE privileged (I-22 — no identity carries ambient
// authority; there is no superuser identity):
//   *_INVALID (0)      — never assigned; KP_ZERO default, fail-closed.
//   PRINCIPAL_SYSTEM   — the boot/kernel-proc identity (kproc, joey,
//                        pre-login). Holds caps via the boot chain, NOT
//                        via identity. Cannot be SET via the spawn path.
//   PRINCIPAL_NONE     — unauthenticated "nobody" (Plan 9 `none`); the
//                        lowest baseline; also reported for a dead peer.
//   Real users/roles: corvus-assigned in [1, 0xFFFFFFFD].
#define PRINCIPAL_INVALID  0u
#define PRINCIPAL_SYSTEM   0xFFFFFFFEu
#define PRINCIPAL_NONE     0xFFFFFFFFu
#define GID_INVALID        0u
#define GID_SYSTEM         0xFFFFFFFEu
#define GID_NONE           0xFFFFFFFFu

// Max supplementary groups cached on the Proc (1 primary + up to 15
// supplementary = 16 total, matching the practical 9P/POSIX group bound).
// Fixed-size to keep the Proc cache slot bounded; corvus holds the full
// membership and a consumer resolves beyond the cache by principal_id.
#define PROC_SUPP_GIDS_MAX 15

struct Proc {
    u64               magic;            // PROC_MAGIC
    int               pid;
    int               thread_count;
    enum proc_state   state;             // P2-D
    int               exit_status;       // P2-D: 0 = clean; non-zero = error
    struct Thread    *threads;          // doubly-linked list head (Thread.next_in_proc)

    // P2-D: parent/children linkage. parent is set at rfork time; never
    // changes (no setpgrp at v1.0). children is the head of a singly-
    // linked list of child Procs chained via Proc.sibling. On exit,
    // orphaned children re-parent to the kernel proc (PID 0) — Phase 2
    // close switches this to PID 1 (init) once init exists.
    struct Proc      *parent;
    struct Proc      *children;
    struct Proc      *sibling;

    // P2-D: exit reporting. exit_msg is a pointer to a static or
    // caller-owned string ("ok" for clean exit, anything else for
    // failure). msg lifetime is the caller's responsibility — at v1.0
    // we only pass static literals. Phase 5+ (exec'd userspace) needs
    // a per-Proc exit_msg buffer to copy from user.
    const char       *exit_msg;

    // P2-D: parent's wait Rendez. Parent's wait_pid() sleeps on this
    // until any child enters ZOMBIE (which is the wakeup edge). Single-
    // waiter convention (only the parent waits on its own children;
    // multiple parent threads waiting concurrently are a Phase 5+
    // concern handled by promoting to multi-waiter wait queue).
    struct Rendez     child_done;

    // P2-Eb: territory (Plan 9 Territory). At v1.0 each Proc has its own
    // private Territory (rfork(RFPROC) calls territory_clone). Phase 5+ adds
    // RFNAMEG: shared territory via refcount sharing.
    struct Territory      *territory;

    // P2-Fc: handle table. Per-Proc fixed-size at v1.0 (PROC_HANDLE_MAX
    // slots; see <thylacine/handle.h>). proc_alloc allocates a fresh
    // empty table; proc_free closes any open handles + releases the
    // table. RFFDG (rfork shared fd table — Plan 9 idiom; v1.0 P2-Fc
    // is one of the slots in a future shared-handle-table design) is
    // forward-looking for the Phase 5+ syscall surface.
    struct HandleTable *handles;

    // P3-Bcb: per-Proc address space.
    //
    // pgtable_root is the PA of this Proc's L0 translation table for
    // TTBR0 (user-half). Allocated at proc_alloc, freed at proc_free.
    // kproc gets pgtable_root = 0 because (a) it's allocated before
    // phys_init / buddy and (b) kernel-only Procs never need a user-
    // half mapping. P3-Bd loads pgtable_root into TTBR0_EL1 at context
    // switch; P3-Be handles the kproc kernel-only path.
    //
    // asid is the ASID assigned to this Proc by asid_alloc. Used at
    // P3-Bd as the high bits of TTBR0_EL1 so the TLB tags the Proc's
    // entries with its own ASID (avoiding cross-Proc TLB pollution).
    // kproc gets asid = 0 (ASID_RESERVED_KERNEL).
    paddr_t            pgtable_root;
    u16                asid;
    u16                _pad_asid[3];     // explicit alignment padding

    // P3-Da: per-Proc VMA list (sorted by vaddr_start ascending). Each
    // VMA describes a contiguous user-VA range with permissions +
    // backing BURROW. The list is the address-space description against
    // which page faults are dispatched (P3-Dc); the per-Proc pgtable
    // (TTBR0) is the runtime translation built on demand from the
    // VMA list as faults fire.
    struct Vma        *vmas;

    // P4-Ib: per-Proc capability bitmask. Drawn from CAP_* in
    // <thylacine/caps.h>. Capabilities monotonically reduce: rfork's
    // child inherits a SUBSET of the parent's caps (at v1.0: empty —
    // children start with no caps unless explicitly granted by a future
    // Phase 5+ capability-grant syscall). kproc starts with CAP_ALL.
    //
    // Pinned by specs/handles.tla:
    //   - HwHandleImpliesCap: holding a hw handle requires CAP_HW_CREATE.
    //   - CapsCeiling:        proc_caps[p] subset of InitialCapsOf(p).
    //   - ReduceCaps invariant: CAP_HW_CREATE can't be dropped while
    //     p still holds any hw handle (v1.0 doesn't expose a drop
    //     syscall; this is structural).
    u64                caps;

    // P5-corvus-syscalls / P5-hostowner-a / P5-corvus-srv: per-Proc
    // one-way flag bits. Zero-initialized at proc_alloc; once set, never
    // cleared. Bits 0-2 are set by the v1.0 hardening syscalls
    // (CORVUS-DESIGN.md §4.1.1); bits 3-4 are kernel-stamped (not
    // syscalls). Like every proc_flags bit, none is propagated by rfork
    // (rfork_internal deliberately does not copy proc_flags).
    //
    // PROC_FLAG_NODUMP   (bit 0) — set by SYS_SET_DUMPABLE(0). When set,
    //                              future core-dump paths must refuse
    //                              to dump this Proc. v1.0 has no core
    //                              dumps; the flag is forward-compat
    //                              scaffolding consumed by corvus +
    //                              per-user stratumd at startup.
    // PROC_FLAG_NOTRACE  (bit 1) — set by SYS_SET_TRACEABLE(0). When
    //                              set, future debug-Spoor attach paths
    //                              must refuse to attach to this Proc.
    //                              v1.0 has no debug Spoors; same
    //                              scaffolding pattern.
    // PROC_FLAG_MLOCKED  (bit 2) — set by SYS_MLOCKALL. When set,
    //                              future swap-out paths must skip
    //                              this Proc's pages. v1.0 has no
    //                              swap; same scaffolding pattern.
    // PROC_FLAG_CONSOLE_ATTACHED (bit 3) — kernel-stamped (NOT a
    //                              syscall) via proc_mark_console_-
    //                              attached() on a Proc spawned through
    //                              joey's console-login chain. The
    //                              local-console trust anchor for
    //                              hostowner elevation: corvus's
    //                              ADMIN_ELEVATE grants CAP_HOSTOWNER
    //                              only to a console-attached peer
    //                              (CORVUS-DESIGN §5.5; specs/corvus.tla
    //                              HostownerRequiresConsole). NOT
    //                              propagated by rfork (see
    //                              rfork_internal) — only an explicit
    //                              proc_mark_console_attached confers
    //                              it (corvus.tla MarkConsoleAttached).
    // PROC_FLAG_MAY_POST_SERVICE (bit 4) — kernel-stamped (NOT a
    //                              syscall) via proc_mark_may_post_-
    //                              service(). The post-gate for the
    //                              /srv service registry: SYS_POST_-
    //                              SERVICE refuses a Proc that does not
    //                              carry this bit (CORVUS-DESIGN §6.1;
    //                              specs/corvus.tla MarkMayPost /
    //                              PostService). joey stamps it on the
    //                              corvus Proc it spawns — so an
    //                              ordinary Proc cannot post or hijack
    //                              /srv/corvus, and a tombstoned name is
    //                              re-postable only by a marked Proc.
    //                              NOT propagated by rfork.
    //
    // Bits 0-2: per CORVUS-DESIGN.md C-2, corvus (and per-user stratumd)
    // call all three hardening syscalls at startup; the flags are
    // visible via future debug surfaces for audit verification.
    u32                proc_flags;

    // Reserved (4 bytes). Was srv_conn_count (u8) + _pad_srv[3]: the
    // per-Proc /srv client-connection cap, removed in stalk-3b-β
    // (3a-audit F4 — a session needs corvus AND its stratum-fs
    // concurrently, so one connection per Proc no longer holds). Kept as
    // explicit padding so proc_flags (u32) is followed by 4 bytes before
    // the 8-byte-aligned `stripes` — struct Proc stays 264 and every
    // subsequent offset assert holds.
    u32                _pad_srv;

    // P5-corvus-srv: the kernel's per-Proc identity tag — the
    // thylacine's stripe pattern; every animal's is unique. Drawn from
    // a monotonic kernel counter at proc_alloc (kproc + every rfork
    // child get a fresh value — an rfork child's `stripes` DIFFERS from
    // its parent's, so a child is structurally distinct, never an
    // inherited identity). Immutable for the Proc's life — no API
    // mutates it after proc_alloc. `stripes == 0` is a reserved fail-
    // closed sentinel: an unstamped or torn-read Proc reads 0 and
    // authorizes nothing. It is the kernel's unforgeable answer to
    // "is this the same Proc?" — read via proc_stripes by SYS_SRV_PEER
    // (P5-corvus-srv-impl-a3) to stamp a /srv connection's peer
    // identity (CORVUS-DESIGN.md §6.3; specs/corvus.tla ConnRecord.peer).
    u64                stripes;

    // P6-pouch-mem: per-Proc lock serializing VMA-list mutation. Held by
    // EVERY VMA-list mutator: SYS_BURROW_ATTACH / SYS_BURROW_DETACH (find-gap
    // + vma_insert / vma_remove) AND SYS_MMIO_MAP / SYS_DMA_MAP (the
    // burrow_map -> vma_insert in the HW-driver map paths; #713 vma_lock
    // audit F1). P6 #713: it is now ALSO held by the page-fault demand-page
    // reader (userland_demand_page) across its vma_lookup -> burrow-resolve
    // -> mmu_install_user_pte sequence. That is the multi-thread-Proc
    // SMP-safety fix: stratumd (the first heavily multi-threaded Proc, and
    // the CAP_HW_CREATE holder that calls SYS_MMIO_MAP / SYS_DMA_MAP) raced
    // the previously-unlocked reader against a sibling thread's VMA-list
    // mutation -> a freed/half-unlinked VMA -> a leaf PTE aliasing a recycled
    // (possibly kernel) page. exec_setup's burrow_map calls and vma_drain remain
    // unlocked deliberately: both run while the Proc is single-threaded by
    // construction (exec precedes the first thread_spawn; vma_drain runs at
    // proc_free with thread_count == 0), so no concurrent fault or mutator
    // can race them. KP_ZERO at proc_alloc / proc_init zero-inits it to the
    // valid unlocked state (SPIN_LOCK_INIT == (spin_lock_t){ 0 }).
    spin_lock_t        vma_lock;
    u32                _pad_vma_lock;     // explicit 8-byte-align padding

    // P6-pouch-signals-impl (sub-chunk 13a): per-Proc note state.
    //
    // `notes` is the per-Proc note queue — heap-allocated by proc_alloc via
    // notes_queue_alloc; freed by proc_free. The queue is ~544 bytes
    // inline; we pointer it to keep the Proc cache slot compact (same
    // pattern as handles, territory). NULL only briefly during proc_alloc/
    // proc_free; ALIVE Procs always carry a valid pointer.
    //
    // `handler_va` is the registered async-handler entry point (user-VA).
    // 0 = no handler (the fd-shaped path is the only consumer; the EL0-
    // return-tail check leaves all notes queued for devnotes_read). Set
    // by SYS_NOTIFY; cleared by SYS_NOTIFY(0).
    //
    // F13 audit close: at v1.0 `handler_va` is NOT inherited across
    // rfork — the child Proc starts with handler_va == 0 (the
    // proc_alloc KP_ZERO default). The Plan 9 inherit-on-fork semantic
    // is deferred to v1.x alongside RFNOTEG, when the rfork primitive
    // gains real fork-without-exec semantics. v1.0 SYS_SPAWN-shaped
    // creation always replaces the binary, so the parent's handler_va
    // wouldn't apply to the child's code anyway.
    //
    // F9 audit close: reads/writes use __atomic_load_n / __atomic_store_n
    // with acquire / release ordering so multi-thread Procs observe a
    // coherent value across CPUs.
    //
    // ARCH §7.6.2 invariants: the queue is the truth; the fd is a view;
    // the handler is the legacy consumer. The discipline holds at v1.0
    // (single-thread Procs at start; multi-thread per sub-chunk 9a).
    struct NoteQueue  *notes;
    u64                handler_va;

    // A-1a: durable identity (docs/IDENTITY-DESIGN.md §3.3 + §9.1).
    // INHERITED across rfork/spawn (rfork_internal copies parent->child);
    // kproc is stamped {PRINCIPAL_SYSTEM, GID_SYSTEM} at proc_init; the
    // spawn thunk optionally overrides via proc_apply_identity when the
    // parent holds CAP_SET_IDENTITY. KP_ZERO at proc_alloc leaves these at
    // INVALID (0) — the fail-closed transient before inherit/stamp runs.
    // NEVER mutated on a running Proc that another Proc observes (the
    // override lands in the child's own thunk before userland_enter), so a
    // plain read is a coherent snapshot. supp_gids[0..supp_gid_count) are
    // live; the tail is zeroed. Identity confers NO caps (I-22): caps flow
    // only through cap_mask; these fields gate the PERMISSION axis (A-2d),
    // never the CAPABILITY axis.
    u32                principal_id;
    u32                primary_gid;
    u32                supp_gids[PROC_SUPP_GIDS_MAX];
    u8                 supp_gid_count;
    u8                 _pad_identity[3];   // explicit align padding

    // SYS_EXIT_GROUP / cross-Proc kill cross-thread shootdown (ARCH §7.9.1,
    // invariant I-24). NULL = no group termination in progress; non-NULL =
    // proc_group_terminate has flagged every Thread of this Proc to die at
    // its next EL0-return die-check, and the LAST Thread out (thread_exit_self)
    // transitions the Proc to ZOMBIE with THIS msg (status derived as exits()
    // does: "ok" -> 0, else -> 1) instead of the default 0/"ok". Set ONCE via
    // an atomic CAS in proc_group_terminate (first msg wins; a racing second
    // group-terminate is a no-op); never cleared (the Proc is terminating).
    // Read __ATOMIC_ACQUIRE at every EL0-return die-check + thread_exit_self's
    // last-out path; the release/acquire pairing gives a cross-CPU peer a
    // coherent view independent of any lock. KP_ZERO inits it to NULL. NOT
    // propagated by rfork (a transient terminating state, not an inherited
    // property).
    const char        *group_exit_msg;

    // A-4a legate elevation (IDENTITY-DESIGN.md §9.8, invariant I-25). A
    // "legate" is the durable user (principal_id UNCHANGED) granted extra
    // caps for a bounded scope via the `cap` device clearance redeem
    // (devcap.c). These tag the scope; KP_ZERO -> not-a-legate.
    //   legate_scope_id    -- the scope this Proc belongs to (0 = none).
    //     Set on the legate ROOT at redeem (fresh, monotonic); INHERITED
    //     across rfork (a child of a scoped Proc joins the scope, carrying
    //     only the fork-grantable subset of the caps -- the elevation-only
    //     members are rfork-stripped, I-2). The subtree-membership tag the
    //     teardown walk matches on.
    //   legate_session_id  -- ephemeral attribution id (0 = not a legate).
    //     Audit reads "principal P via legate-session N". Set on the root
    //     at redeem; inherited across rfork. Durable principal_id is
    //     UNCHANGED by elevation (scripture §3.1).
    //   legate_valid_until -- ns deadline (0 = none); copied from the
    //     grant, inherited across rfork. On `now > legate_valid_until`
    //     (checked at the EL0-return tail) the whole scope is torn down.
    // PROC_FLAG_LEGATE_ROOT (proc_flags) marks the redeeming Proc; when it
    // DIES the scope is torn down (proc_group_terminate each member). The
    // teardown fires from proc_become_zombie_locked -- the shared ZOMBIE
    // chokepoint -- so it covers EVERY death path (a clean exit AND a kill /
    // group-terminate / SYS_EXIT_GROUP death), not only exits() (A-4a audit
    // F1). The flag is NOT propagated by rfork (proc_flags never are), so a
    // child is a scope MEMBER, never a second root. Evaporation = scope
    // teardown (reuses the #809/#811 cascade): no elevated Proc outlives
    // the scope. A-4a-2a lands these fields + the rfork-inherit + the
    // membership tag; the devcap redeem that SETS them (creates a legate)
    // and the teardown triggers (the zombie-chokepoint root death + the
    // EL0-tail valid_until expiry) land together in A-4a-2b -- so a legate
    // never exists without its teardown (I-25 holds at every commit).
    u32                legate_session_id;
    u32                legate_scope_id;
    u64                legate_valid_until;
};

#define PROC_FLAG_NODUMP            (1u << 0)
#define PROC_FLAG_NOTRACE           (1u << 1)
#define PROC_FLAG_MLOCKED           (1u << 2)
#define PROC_FLAG_CONSOLE_ATTACHED  (1u << 3)
#define PROC_FLAG_MAY_POST_SERVICE  (1u << 4)
// A-4a: marks the legate ROOT -- the Proc that redeemed a `cap`-device
// clearance grant. Set at redeem; one-way; NOT propagated by rfork (no
// proc_flag is). When the root DIES, proc_become_zombie_locked tears down
// its scope on every death path (clean exit AND kill / group-terminate;
// A-4a audit F1). I-25.
#define PROC_FLAG_LEGATE_ROOT       (1u << 5)
// LS-5 (P2 default disposition, ARCH 8.8.2): marks a Proc that opened its
// notes fd (devnotes, via SYS_NOTE_OPEN) -- it has declared it consumes its
// OWN notes (the shell's wait_pids_interruptible notes-fd poll). The
// discriminator the EL0-return-tail uncaught-`interrupt` default-terminate
// uses: a self-managing Proc is EXEMPT (it reads + acts on its notes), while a
// non-self-managing Proc with no async handler default-terminates on an
// uncaught, unmasked interrupt. One-way (a Proc that ever opened its notes fd
// stays self-managing for life); NOT propagated by rfork (a spawned child
// starts fresh -- a dumb coreutil never opens it, so Ctrl-C terminates it).
#define PROC_FLAG_SELF_MANAGING_NOTES (1u << 6)
// LS-5c (P3-terminate, ARCH 8.8.2): a terminate-disposition `interrupt` is
// pending -- notes_post enqueued an `interrupt` while the Proc had no async
// handler and was not self-managing, so (per the LS-5b default disposition)
// the Proc terminates at its next EL0-return tail. The latch is the
// LOCK-FREE wake-hint the #811 sleep/tsleep register-then-observe reads
// (the widened death predicate, thread_die_pending: group_exit_msg OR this
// bit while `interrupt` is unmasked for the thread) -- the sleep path can
// never take p->notes->lock (the devnotes F3-close ABBA), so the
// disposition decided at post time is cached here. The EL0-return tail
// remains the TRUTH (it re-runs the full LS-5b predicate against the live
// queue); a stale-positive latch costs one spurious *_INTR unwind, never a
// wrong termination. UNLIKE the one-way bits above this one is a LATCH:
// set and cleared ONLY under p->notes->lock (set: notes_post's interrupt
// arm; cleared: handler registration [notes_set_handler], the self-managing
// mark [notes_mark_self_managing], draining the last queued interrupt) so
// it tracks its disposition inputs race-free. NOT propagated by rfork (a
// pending interrupt is the parent's, not the child's). NEVER set on kproc
// (the arm guards it: in-kernel tests post to kproc's queue via the boot
// thread, and an armed kproc would *_INTR every kernel-thread sleep).
#define PROC_FLAG_INTR_TERMINATE_PENDING (1u << 7)

_Static_assert(sizeof(struct Proc) == 264,
               "struct Proc size pinned at 264 bytes (SYS_EXIT_GROUP baseline "
               "248 + the A-4a legate block: legate_session_id u32 + "
               "legate_scope_id u32 + legate_valid_until u64 = 16 -> 264). "
               "Adding a field grows the SLUB cache; update this assert "
               "deliberately so the change is intentional.");
_Static_assert(__builtin_offsetof(struct Proc, principal_id) == 168,
               "A-1a identity block appends after handler_va; existing "
               "offsets must stay stable (KP_ZERO inits the new tail).");
_Static_assert(__builtin_offsetof(struct Proc, group_exit_msg) == 240,
               "SYS_EXIT_GROUP group_exit_msg appends after the A-1a identity "
               "block; existing offsets stay stable (KP_ZERO inits it NULL).");
_Static_assert(__builtin_offsetof(struct Proc, legate_session_id) == 248,
               "A-4a legate block appends after group_exit_msg; existing "
               "offsets stay stable (KP_ZERO inits the new tail to "
               "not-a-legate).");
_Static_assert(__builtin_offsetof(struct Proc, magic) == 0,
               "magic must be at offset 0 so SLUB's freelist write on "
               "kmem_cache_free clobbers it (double-free defense — "
               "P2-A audit R4 F42)");

// Bring up the process subsystem. Allocates the kernel proc (PID 0) via
// SLUB; subsequent thread_init wires kthread to kproc. Must be called
// after slub_init.
void proc_init(void);

// Accessor for the kernel proc (PID 0). Returns NULL before proc_init.
struct Proc *kproc(void);

// SLUB-allocate a fresh Proc descriptor. Initializes pid (next monotonic
// pid), threads = NULL, thread_count = 0, state = ALIVE, child_done
// initialized. parent / children / sibling left NULL (caller wires
// linkage). Returns NULL on OOM.
struct Proc *proc_alloc(void);

// Release a Proc descriptor. Caller must ensure thread_count == 0
// (no live threads) and state == ZOMBIE (or post-reap path). Extincts
// on violation.
void proc_free(struct Proc *p);

// P2-D: rfork — Plan 9 process/thread creation primitive.
//
// `flags` selects which resources are shared vs cloned (per ARCH §7.4).
// At v1.0 P2-D, only RFPROC is supported — all other flags trigger an
// extinction. Subsequent sub-chunks add RFNAMEG (P2-E territory),
// RFFDG (P2-F fd table), RFMEM (P2-G address space), etc.
//
// `entry` is the kernel function the new process's initial thread will
// run. `arg` is passed to entry in x0 via the trampoline. Both must be
// non-NULL (entry must not be NULL).
//
// On success: returns the new child's PID (≥ 1; allocated by proc_alloc).
// The child is fully wired (ALIVE state, linked to parent's children
// list, initial Thread RUNNABLE in the local CPU's run tree). Caller
// returns immediately; child runs asynchronously when sched picks it.
//
// On failure (OOM): returns -1. No partial state — any allocations are
// rolled back.
//
// Conceptually models the parent's return from rfork; the child's
// "return from rfork" is replaced by entry(arg) executing in the kernel
// at v1.0 (no userspace yet). When P2-G lands the ELF loader and
// userspace, rfork's semantics extend to the syscall split (parent gets
// PID, child gets 0 — a register-set tweak in the syscall-return path).
int rfork(unsigned flags, void (*entry)(void *), void *arg);

// P4-Ic3: kernel-internal rfork that grants the child a capability subset.
//
// Identical to `rfork` except the child's caps field is set to
// `(parent->caps & caps_mask)` — the AND with the parent's current caps
// is the impl-side enforcement of specs/handles.tla::RforkWithCaps's
// `granted \subseteq proc_caps[parent]` precondition (CapsCeiling). A
// caller cannot grant a capability the caller itself doesn't hold; if
// `caps_mask` requests bits beyond the parent's caps, the resulting
// child gets only the intersection. The parent's caps are read under
// `__ATOMIC_ACQUIRE` to coalesce a single observable snapshot.
//
// At v1.0 the only intended caller is kernel-internal context that runs
// as kproc (which holds CAP_ALL) and wants to spawn a driver Proc
// holding a specific subset (e.g., CAP_HW_CREATE for the virtio-blk
// driver). Phase 5+ can lift this to a syscall — the spec's
// `RforkWithCaps` action already models the userspace-callable form.
//
// Returns the child's PID on success; -1 on OOM (identical to `rfork`).
int rfork_with_caps(unsigned flags, void (*entry)(void *), void *arg,
                    caps_t caps_mask);

// rfork flags. Per ARCH §7.4. Only RFPROC implemented at P2-D.
#define RFPROC      0x0001    // create a new Proc (always required)
#define RFMEM       0x0002    // share address space (future P2-G)
#define RFNAMEG     0x0004    // share territory (future P2-E)
#define RFFDG       0x0008    // share fd table (future P2-F)
#define RFCRED      0x0010    // share credentials (future P2-G)
#define RFNOTEG     0x0020    // share note queue (future Phase 5)
#define RFNOWAIT    0x0040    // detach from parent's children list (future)
#define RFREND      0x0080    // share rendezvous space (future Phase 5)
#define RFENVG      0x0100    // share environment (future P2-G)

// P2-D: exits — terminate the calling process.
//
// `msg` is a status string ("ok" for clean exit, anything else for
// failure — translated to exit_status 0 vs 1). msg is captured into
// p->exit_msg by reference; caller must ensure msg lives until the
// parent calls wait() (typically a string literal).
//
// Steps:
//   1. Mark the Proc ZOMBIE, set exit_status + exit_msg.
//   2. Mark the calling Thread EXITING — sched() will leave it out of
//      the run tree.
//   3. Wake parent's child_done Rendez (if parent exists).
//   4. yield via sched(). exits() never returns; the thread is left in
//      EXITING state until wait() reaps it.
//
// At v1.0 P2-D, only a single thread per Proc is supported (mirrors
// what rfork(RFPROC) creates). Multi-threaded Procs at Phase 5+ require
// exits() to terminate ALL threads of the Proc atomically (a Phase 2
// close refinement).
//
// Re-parenting of orphaned children: if the exiting Proc has children,
// they re-parent to kproc() (PID 0; init at Phase 5+). Their parent
// pointer is updated; their child_done is irrelevant since kproc never
// calls wait_pid().
__attribute__((noreturn))
void exits(const char *msg);

// P6 hardening #3a (scripture e45a571): terminate the calling Thread's
// Proc on EL0 unhandled fault. Called from
// arch/arm64/exception.c::exception_sync_lower_el for every EL0 sync
// exception that previously called extinction_with_addr.
//
// `name` MUST be one of the NOTE_NAME_SNARE_* constants from
// <thylacine/notes.h>. `faulting_addr` is FAR_EL1 (data abort) or
// ELR_EL1 (other EL0 EC), printed in the diagnostic for forensics.
//
// NEVER returns. Emits a uart diagnostic line + calls exits(name).
// In multi-thread Procs (thread_count > 1) it extincts with a
// specific message (v1.0 lacks cross-thread shootdown). In all
// other corruption / impossible cases (no current thread, no
// proc, kproc-routed, ...) it extincts with a defense-in-depth
// message.
//
// See docs/ERRORS.md "Fault-note naming -- the snare:* family" for
// the design rationale + the per-cause `snare:*` table.
__attribute__((noreturn))
void proc_fault_terminate(const char *name, uintptr_t faulting_addr);

// P6-pouch-threads (sub-chunk 9): terminate the calling Thread.
//
// NEVER returns. Atomically:
//   1. If t->clear_child_tid != 0, uaccess_store_u32(0) at that user-VA
//      + torpor_wake(UINT32_MAX). Best-effort; an unmapped tidptr skips
//      the wake but does not extinct.
//   2. Mark self THREAD_EXITING under g_proc_table_lock.
//   3. If this was the last non-EXITING Thread in the Proc, ALSO mark
//      the Proc ZOMBIE with exit_status = 0 + wake parent's child_done
//      (mirrors exits("ok")).
//   4. yield via sched(); never returns.
//
// Used by SYS_THREAD_EXIT. Distinct from exits() in two ways:
//   - exits() requires the caller to be the last live Thread (extincts
//     if any peer is non-EXITING) and sets a caller-specified
//     exit_status. thread_exit_self() does not — it cleanly exits a
//     non-last Thread without touching Proc state, OR cleanly
//     transitions the Proc to ZOMBIE if this turns out to be the last.
//   - thread_exit_self() does the clear_child_tid handoff (for
//     pthread_join). exits() also does it for the calling Thread
//     (any one of the Proc's threads may carry a tidptr if the
//     program registered one).
__attribute__((noreturn))
void thread_exit_self(void);

// SYS_EXIT_GROUP / cross-Proc kill cross-thread shootdown (ARCH §7.9.1,
// invariant I-24). Flag `p` for group termination + wake/kick its Threads so
// each self-terminates at its next EL0-return die-check (el0_return_die_check).
// Does NOT wait for peers; the LAST Thread out reaps the Proc -> ZOMBIE with
// `msg` (NULL -> "killed"). Idempotent (CAS the flag; first msg wins).
//
// Mechanism: (1) CAS p->group_exit_msg = msg; (2) torpor_wake_all_for_proc(p)
// so already-sleeping futex peers return; (3) the #811 universal death-wake
// (ARCH 8.8.1): walk p->threads, per-peer wait_lock -> rendez_blocked_on ->
// wakeup, so EVERY rendez sleeper returns *_INTR and dies at its EL0-return
// die-check; (4) smp_resched_others() so a peer running in userspace on
// another CPU traps + hits its IRQ-from-EL0 die-check (the periodic timer is
// the floor if the IPI is missed).
//
// LOCK CONTRACT (#811): the caller MUST hold g_proc_table_lock -- the
// p->threads walk races a concurrent thread_free into a UAF otherwise. It
// additionally takes torpor_lock + per-peer wait_lock + (via wakeup)
// rendez/timerwait locks, all strictly BELOW g_proc_table_lock in the order.
void proc_group_terminate(struct Proc *p, const char *msg);

// EL0-return-tail die-check (ARCH §7.9.1, invariant I-24). Called at every
// return-to-EL0 (the sync-from-EL0 SVC + fault-handled tails in
// exception.c, and the IRQ-from-EL0 tail in vectors.S 0x480). If the calling
// Thread's Proc is group-terminating (group_exit_msg != NULL), the Thread
// self-terminates here via thread_exit_self() (NEVER returns in that case).
// Otherwise returns. #713: runs BEFORE the DAIF-masked ELR-set..eret window;
// the die path sched()s away and never reaches the eret.
void el0_return_die_check(void);

// P6-pouch-threads (sub-chunk 9a) audit F1 close: cross-module access to
// `g_proc_table_lock` (kept static in proc.c). thread.c's
// thread_link_into_proc / thread_unlink_from_proc must serialize against
// proc_count_live_peers_locked's walk by holding the same lock; expose
// the lock as a pair of acquire/release helpers rather than the raw
// spin_lock_t to keep the lock's storage private to proc.c.
//
// Lock-order discipline (per CLAUDE.md §28 + the existing exits/wait_pid
// commentary): proc_table_lock is at the "lineage" level — acquired
// BEFORE r->lock-via-wakeup but AFTER any leaf locks the caller already
// holds. Sound iff no path that holds a per-thread Rendez or per-Proc
// vma_lock tries to acquire it. The current callers (thread_link /
// thread_unlink at thread_create_*) hold no other locks; they're safe.
//
// The helpers wrap spin_lock_irqsave / spin_unlock_irqrestore so the
// IRQ-state thread-through stays explicit at the call site (irq_state_t
// is the public spinlock primitive's typedef from <thylacine/spinlock.h>,
// already included above).
irq_state_t proc_table_lock_acquire(void);
void        proc_table_lock_release(irq_state_t s);

// P6-pouch-signals-impl (sub-chunk 13a) audit F4 close: cross-module
// access to the live-peer count for the kill-delivery defense-in-depth
// check + the SYS_POSTNOTE multi-thread gate. Counts peer Threads (NOT
// `self`) whose state is not THREAD_EXITING. PRECONDITION: caller holds
// g_proc_table_lock. `self` may be NULL to count all live Threads.
int proc_count_live_peers_locked(struct Proc *p, struct Thread *self);

// WAIT_WNOHANG — wait_pid_for flag: do not block; if no matching zombie
// is ready, return 0 immediately. Mirrors POSIX WNOHANG. Userspace passes
// this in the SYS_WAIT_PID flags arg (x1); the libt / libthyla-rs mirrors
// (WAIT_WNOHANG / T_WAIT_WNOHANG) MUST hold the same value.
#define WAIT_WNOHANG 1

// wait_pid_for — reap a ZOMBIE child, optionally filtered by pid and/or
// non-blocking. The selection-and-blocking generalization underlying
// SYS_WAIT_PID and the wait_pid wrapper.
//
//   want_pid == -1     : any child (the original wait_pid semantics).
//   want_pid  >  0     : only the child with that pid.
//   want_pid  == 0/<-1 : POSIX process-group selectors — no v1.0 meaning;
//                        match nothing (→ -1). Reserved for a future lift.
//   flags & WAIT_WNOHANG : do not block (see WAIT_WNOHANG).
//
// Returns:
//   > 0 : reaped child's pid; *status_out (if non-NULL) holds its status.
//   0   : WAIT_WNOHANG set and a matching child is alive but not yet a
//         zombie. 0 is never a valid child pid (g_next_pid starts at 1),
//         so it is an unambiguous "not ready" sentinel.
//   -1  : no matching child (none at all, or none with want_pid), OR the
//         caller's Proc is group-terminating (#811 SLEEP_INTR).
//
// CALLER NOTE (WAIT_WNOHANG progress): a WNOHANG poll loop MUST yield between
// calls — block on a notes fd (the kernel posts a `child_exit` note on every
// child exit), `torpor_wait`, or `sched` — never hot-busy-spin on the 0
// return. `while (wait_pid_for(pid, WAIT_WNOHANG, …) == 0) {}` with no yield
// can starve the very child it awaits on a constrained CPU set (EEVDF bounds
// it — it is not a deadlock — but it is still wrong). Job control composes
// WNOHANG with `poll()` on the shell's notes fd, not a hot loop.
//
// Picks the first matching zombie in the children list, unlinks it,
// copies exit_status, and runs the standard thread_free + proc_free
// teardown. The teardown + lock discipline are byte-for-byte identical to
// the legacy wait_pid; only the selection (pid filter) and the blocking
// decision (WNOHANG) differ.
int wait_pid_for(int want_pid, int flags, int *status_out);

// wait_pid — reap ANY zombie child, blocking. The pervasive case;
// equivalent to wait_pid_for(-1, 0, status_out). Plan 9 wait(2) shape.
// Most kernel callers (test harness rfork-then-reap) use this form.
int wait_pid(int *status_out);

// Diagnostic.
u64 proc_total_created(void);
u64 proc_total_destroyed(void);

// =============================================================================
// P4-C: Proc lookup for devproc.
// =============================================================================
//
// proc_find_by_pid: locate a Proc by its PID via DFS from kproc through
// the children/sibling tree. Returns the matching Proc * or NULL.
// Acquires + releases g_proc_table_lock internally.
//
// At v1.0 P4-C the returned pointer is stable only under the
// "no concurrent reap" assumption: the caller must not be racing a
// wait_pid that's about to free the target. This is safe in v1.0's
// kernel-internal callers (devproc reads under the kernel's
// non-preemptive context; the future syscall layer's /proc reads
// run in the calling thread's context, where the target Proc cannot
// be reaped without the calling thread participating).
//
// Phase 5+ adds proper Proc refcounting: proc_find_by_pid will return a
// proc_get'd reference, requiring proc_put to release.
struct Proc *proc_find_by_pid(int pid);

// proc_for_each: invoke callback(p, arg) for every Proc in the tree
// (DFS from kproc). The callback returns 0 to continue, non-zero to
// stop early. Returns the last callback return value (0 if iteration
// completed). g_proc_table_lock held throughout — callbacks must not
// re-enter proc_find_by_pid / rfork / exits / wait_pid / proc_for_each.
//
// Used by future devproc readdir (when 9P readdir lands at Phase 4+).
// Not called at v1.0 P4-C; declared here so the API is stable for
// the future caller.
int proc_for_each(int (*callback)(struct Proc *p, void *arg), void *arg);

// =============================================================================
// P5-hostowner-a: console attachment.
// =============================================================================
//
// PROC_FLAG_CONSOLE_ATTACHED marks a Proc spawned through joey's
// console-login chain — the local-console trust anchor for hostowner
// elevation (CORVUS-DESIGN.md §5.5). corvus's ADMIN_ELEVATE verb
// (P5-hostowner-b) grants CAP_HOSTOWNER only to a console-attached
// peer; specs/corvus.tla pins this as the HostownerRequiresConsole
// invariant.

// proc_mark_console_attached — stamp PROC_FLAG_CONSOLE_ATTACHED on `p`.
// Idempotent; maps to specs/corvus.tla's MarkConsoleAttached action.
// Callers: joey (the console-login chain root) marks itself at boot; the
// A-4c-2 SAK re-grant marks corvus (the trusted login authority). The bit
// is NEVER conferred by rfork — every console-attached Proc is the result
// of an explicit call here. The OR is atomic (__atomic_or_fetch): A-4c-2
// makes the bit multi-writer (the SAK runs on the console_mgr kthread, a
// DIFFERENT thread than the owner), so the v1.0 single-writer proc_flags
// convention no longer holds for this one bit. Extincts on a NULL/corrupt/
// non-ALIVE Proc.
void proc_mark_console_attached(struct Proc *p);

// proc_revoke_console_attached — clear PROC_FLAG_CONSOLE_ATTACHED on `p`
// (the unset side the A-4c-2 SAK needs; the SAK is the sole revoker). The
// AND is atomic (multi-writer bit; see proc_mark_console_attached). A
// no-op on a NULL/corrupt Proc (fail-closed). Owner-lifetime is the
// caller's responsibility — proc_console_sak calls this under
// g_proc_table_lock with the owner pinned.
void proc_revoke_console_attached(struct Proc *p);

// proc_is_console_attached — true iff `p` carries
// PROC_FLAG_CONSOLE_ATTACHED. Returns false for a NULL `p`. The read is an
// atomic load (the bit is multi-writer post-A-4c-2).
bool proc_is_console_attached(const struct Proc *p);

// A-4c-1: the kernel console owner (the trusted-path anchor for /dev/cons).
//
// proc_set_console_owner — record `p` as the current console owner (joey at
// boot; A-4c-2's SAK re-grants to corvus). Takes g_proc_table_lock. Pass NULL
// to clear (the A-4c-2 fail-safe revoke-only). proc_become_zombie_locked also
// clears it automatically when the owner dies, so the pointer never dangles.
void proc_set_console_owner(struct Proc *p);

// proc_console_post_interrupt — post the `interrupt` note (Ctrl-C) to the
// current console owner. Called from the console_mgr kthread (process context).
// No-op when there is no live owner. Takes g_proc_table_lock.
void proc_console_post_interrupt(void);

// proc_console_relinquish — `p` drops its OWN console-attach bit and, if it is
// the current g_console_owner, clears the owner pointer. A-5a / I-27: joey (the
// boot console anchor) calls this (via SYS_CONSOLE_RELINQUISH on itself) at the
// bringup->session boundary so that during a user session corvus is the SOLE
// console-attached Proc -- otherwise a post-SAK state is {joey,corvus} both
// attached. Takes g_proc_table_lock (the owner-pointer clear); the attach-bit
// clear is the atomic proc_revoke_console_attached. owner->NULL pre-SAK means
// Ctrl-C is dropped (no foreground consumer at v1.0); the SAK re-grants corvus
// regardless. The SYS_CONSOLE_RELINQUISH handler only ever passes the caller's
// own Proc (self-only -- it cannot revoke another Proc).
void proc_console_relinquish(struct Proc *p);

// proc_set_console_trusted — record `p` as the trusted login authority (corvus):
// the target the A-4c-2 SAK re-grants the console to. Takes g_proc_table_lock.
// Set when joey establishes corvus (SPAWN_PERM_CONSOLE_TRUSTED). Pass NULL to
// clear; proc_become_zombie_locked also clears it on the trusted Proc's death so
// the pointer never dangles (a then-fired SAK falls back to revoke-only).
void proc_set_console_trusted(struct Proc *p);

// proc_console_sak — the A-4c-2 SAK transition (I-27 trusted-path handoff). Run
// from the console_mgr kthread on a recognized serial BREAK. Under
// g_proc_table_lock: revoke the console-attach bit from the current owner + post
// it a notify note, then re-grant the bit to the trusted login authority and
// make it the owner. FAIL-SAFE: with no trusted Proc alive, revoke-only (owner
// cleared to NULL) -- no Proc can redeem CAP_HOSTOWNER / a clearance until a
// trusted login claims the console. Idempotent if the trusted Proc already owns
// the console (a BREAK flood is a no-op).
void proc_console_sak(void);

// =============================================================================
// P5-corvus-srv: per-Proc identity tag.
// =============================================================================
//
// `stripes` (see the struct field) is the kernel's unforgeable per-Proc
// identity, set fresh at proc_alloc. proc_stripes is the read API
// SYS_SRV_PEER (P5-corvus-srv-impl-a3) uses to stamp a /srv connection's
// peer identity; specs/corvus.tla pins it as ConnRecord.peer.

// proc_stripes — return `p`'s `stripes` tag. Fail-closed: a NULL or
// corrupted Proc reads as 0 — the reserved sentinel that authorizes
// nothing (never a stale or fabricated tag).
u64 proc_stripes(const struct Proc *p);

// proc_caps_by_stripes — the SYS_SRV_PEER live-caps lookup
// (P5-corvus-srv-impl-a3c). Scan the process table for an ALIVE Proc
// carrying `stripes` and snapshot its capability set into *caps_out.
// Returns true iff such a Proc exists.
//
// Returns false — fail-closed, *caps_out untouched — when `stripes` is
// the 0 sentinel, `caps_out` is NULL, or no ALIVE Proc matches (the peer
// has exited / is a zombie / was reaped). The table scan and the caps
// read run under g_proc_table_lock, so the result is a coherent snapshot
// — never a torn read, never a use-after-free of a Proc being reaped.
// The kernel half of specs/corvus.tla ConnOpPeerWasLive: a dead peer
// authorizes nothing.
bool proc_caps_by_stripes(u64 stripes, caps_t *caps_out);

// A-1a: the richer peer snapshot feeding SYS_SRV_PEER's srv_peer_info.
// One walk under g_proc_table_lock snapshots caps + principal_id +
// primary_gid for the ALIVE Proc carrying `stripes`. proc_caps_by_stripes
// is now a thin wrapper over this (caps-only). Same fail-closed contract:
// `stripes == 0` or no ALIVE match -> returns false, out-params untouched.
// Any out-param may be NULL (the caller takes only what it needs). Only
// VALUES escape — never the Proc pointer — so a peer reaped after the scan
// is not a UAF.
bool proc_peer_snapshot_by_stripes(u64 stripes, caps_t *caps_out,
                                   u32 *principal_out, u32 *primary_gid_out);

// =============================================================================
// A-1a: identity mutation (the single audited write site).
// =============================================================================
//
// proc_apply_identity — overwrite `p`'s durable identity with the given
// principal_id / primary_gid / supplementary gids. The ONLY function that
// mutates a Proc's identity after proc_init/rfork (which set it via
// stamp/inherit). Called from the spawn thunk BEFORE userland_enter, only
// after the parent verified CAP_SET_IDENTITY + value bounds. Copies the
// first `supp_gid_count` gids and zeroes the tail so no stale inherited
// gid survives past the count. Extincts on a kernel-internal contract
// violation (the caller should already have gated these): a NULL/corrupted
// Proc; supp_gid_count > PROC_SUPP_GIDS_MAX; or a principal_id/primary_gid
// that is the INVALID or SYSTEM sentinel (never legitimate to STAMP -- the
// boot chain sets SYSTEM directly in proc_init, and the spawn gate
// pre-validates real ids / NONE). Supplementary-gid VALUES are the caller's
// (the spawn gate's) responsibility -- this site bounds the count only.
// Confers NO caps (I-22).
void proc_apply_identity(struct Proc *p, u32 principal_id, u32 primary_gid,
                         const u32 *supp_gids, u8 supp_gid_count);

// =============================================================================
// A-4a: the legate stamp (the single audited legate-creation write site).
// =============================================================================
//
// proc_become_legate — make `p` a legate ROOT (IDENTITY-DESIGN.md §9.8, I-25).
// The ONLY function that creates a legate; called from the `cap` device
// clearance redeem (devcap.c::cap_redeem_grant_for_writer) after it validated
// the pending clearance grant. Atomically ORs `caps_to_or` (already narrowed by
// the redeem's self_restriction to a subset of the grant) into p->caps, records
// the scope context (a FRESH kernel-allocated legate_scope_id -- NEVER caller-
// supplied, since it is the teardown-walk match key and a collision would tear
// down the wrong subtree -- plus the corvus-supplied session_id + the computed
// valid_until), and sets PROC_FLAG_LEGATE_ROOT. Durable principal_id is
// UNCHANGED (scripture §3.1: the legate is the same human, more authority).
// `caps_to_or` is OR'd with __ATOMIC_ACQ_REL (multi-thread Procs exist since
// P6; a sibling thread may read p->caps in a concurrent syscall cap-check).
// Extincts on a NULL / corrupted Proc (a kernel-internal contract violation).
// The matching EVAPORATION (scope teardown on root exit / valid_until expiry)
// lands in the same chunk so a legate never exists without its teardown (I-25).
void proc_become_legate(struct Proc *p, u64 caps_to_or, u32 session_id,
                        u64 valid_until);

// proc_legate_teardown_if_root — if `p` is a legate ROOT, group-terminate every
// OTHER Proc in its legate_scope_id (I-25). Called from proc_become_zombie_locked
// (the shared ZOMBIE chokepoint) so the sweep fires on EVERY root death path
// (clean exit AND kill / group-terminate; A-4a audit F1). A non-root Proc is a
// no-op. PRECONDITION: caller holds g_proc_table_lock (uses the LOCKED walk).
void proc_legate_teardown_if_root(struct Proc *p);

// =============================================================================
// P5-corvus-srv-impl-a2: the /srv service-registry post-gate.
// =============================================================================
//
// PROC_FLAG_MAY_POST_SERVICE gates SYS_POST_SERVICE: only a Proc carrying
// it may register a name in the /srv service registry. joey stamps it on
// the corvus Proc it spawns (CORVUS-DESIGN.md §6.1); specs/corvus.tla
// pins this as MarkMayPost gating PostService.

// proc_mark_may_post_service — stamp PROC_FLAG_MAY_POST_SERVICE on `p`.
// One-way: idempotent, never cleared, never conferred by rfork. v1.0
// caller: joey, on the /sbin/corvus Proc it spawns (wired at
// P5-corvus-srv-impl-b, when corvus posts /srv/corvus for real).
// Extincts on a NULL / corrupted / non-ALIVE Proc.
void proc_mark_may_post_service(struct Proc *p);

// proc_may_post_service — true iff `p` carries PROC_FLAG_MAY_POST_SERVICE.
// Returns false for a NULL / corrupted `p` (fail-closed).
bool proc_may_post_service(const struct Proc *p);

// =============================================================================
// LS-5: the self-managing-notes mark (P2 default disposition, ARCH 8.8.2).
// =============================================================================
//
// PROC_FLAG_SELF_MANAGING_NOTES marks a Proc that opened its notes fd
// (SYS_NOTE_OPEN). The uncaught-`interrupt` default-terminate
// (notes_deliver_at_el0_return) exempts such a Proc -- it consumes its own
// notes via the fd-read path -- and terminates only a NON-self-managing Proc
// that registered no async handler. The shell qualifies; a dumb coreutil does
// not. Strictly distinct from the console-OWNER / console-ATTACH marks (this
// is "I read my own notes", not "I receive Ctrl-C" or "I am the SAK anchor").

// proc_mark_self_managing_notes — stamp PROC_FLAG_SELF_MANAGING_NOTES on `p`.
// One-way: idempotent, never cleared, never propagated by rfork. v1.0 caller:
// sys_note_open_handler, on the Proc that opened its notes fd. Atomic OR (the
// proc_flags word is multi-writer post-A-4c-2). Extincts on a NULL / corrupted
// / non-ALIVE Proc (mirrors proc_mark_may_post_service -- a Proc running
// SYS_NOTE_OPEN is always ALIVE).
void proc_mark_self_managing_notes(struct Proc *p);

// proc_is_self_managing_notes — true iff `p` carries
// PROC_FLAG_SELF_MANAGING_NOTES. Fail-closed: a NULL / corrupted `p` reads as
// NOT self-managing -- the SAFE default (an unverifiable Proc does not dodge
// the interrupt default-terminate). Atomic load (multi-writer word).
bool proc_is_self_managing_notes(const struct Proc *p);

// =============================================================================
// LS-5c: the terminate-disposition interrupt wake (P3-terminate, ARCH 8.8.2).
// =============================================================================

// proc_intr_terminate_pending — true iff `p` carries
// PROC_FLAG_INTR_TERMINATE_PENDING (see the flag's comment for the latch
// contract). Fail-closed on a NULL / corrupted `p`. Acquire load: pairs with
// the release set in notes_post's arm so a lock-free reader (the #811 sleep
// predicate via thread_die_pending) observes a fully-published latch.
bool proc_intr_terminate_pending(const struct Proc *p);

// proc_interrupt_terminate_wake — wake every Thread of `p` blocked in a
// rendez sleep so it unwinds (*_INTR) to its EL0-return tail, where the
// LS-5b uncaught-interrupt default-terminate fires. Internally gated on
// proc_intr_terminate_pending (a no-op unless notes_post armed the latch),
// so interrupt-posting sites call it unconditionally after the post.
//
// CALLER MUST HOLD g_proc_table_lock (the walk reads p->threads, mutated
// only under that lock -- the #811 contract proc_group_terminate carries).
// The body is the #811 universal death-wake template: per-peer wait_lock ->
// rendez_blocked_on -> wakeup, lock order g_proc_table_lock -> wait_lock ->
// (wakeup: g_timerwait.lock -> r->lock). Deliberately NO
// torpor_wake_all_for_proc (a torpor waiter's stack rendez is reachable via
// rendez_blocked_on, and tsleep's widened register-then-observe closes the
// register-after-walk race) and NO smp_resched_others (the IRQ-from-EL0
// tail evaluates only group_exit_msg, so an IPI cannot accelerate a RUNNING
// thread's interrupt-death -- it reaches its next sync-from-EL0 tail
// regardless; the never-syscalling spinner gap is tracked as task #964).
void proc_interrupt_terminate_wake(struct Proc *p);

#endif // THYLACINE_PROC_H
