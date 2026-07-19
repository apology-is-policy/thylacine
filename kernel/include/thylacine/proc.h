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
//          + exit_status + exit_msg + the child-reap wait (multi-waiter
//          child_waiters since #344; single-waiter child_done Rendez before).
//   P2-E:  + territory pointer (Territory).
//   P2-F:  + handle table head.
//   P2-G:  + address space (page table root, vma_tree) + credentials
//          + capability bitmask + notes queue.

#ifndef THYLACINE_PROC_H
#define THYLACINE_PROC_H

#include <thylacine/caps.h>     // caps_t (P4-Ic3 rfork_with_caps signature)
#include <thylacine/page.h>     // paddr_t (P3-Bcb pgtable_root)
#include <thylacine/rendez.h>
#include <thylacine/poll.h>      // poll_waiter_list -- the multi-waiter child-reap wait (#344)
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
struct Allowance;   // I-34 hardware allowance (<thylacine/allowance.h>)
struct Env;         // G15 per-Proc environment group (<thylacine/env.h>)
struct Spoor;       // 8a-1b debug_owner slot token (<thylacine/spoor.h>)
struct debug_hw;    // 8a-2b per-Proc HW-breakpoint table (arch/arm64/hwdebug.h)

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

// #65 (invariant I-32): the per-Proc resource floor. Fixed maxima that bound a
// non-TCB Proc's resource use so a fork/thread/memory bomb is bounded, not
// box-extincting. Tunable; generous for any v1.0 user workload (shell +
// coreutils + editor) yet a bomb hits them fast. The TCB (PRINCIPAL_SYSTEM) is
// exempt (proc_resource_exempt). Full rationale: IDENTITY-DESIGN.md §3.8.
//
//   PROC_PAGE_MAX   -- live anon pages via SYS_BURROW_ATTACH (256 MiB). The
//                      memory-bomb bound; checked under vma_lock so it is exact.
//   PROC_THREAD_MAX -- live threads. Tighter than the others because a thread
//                      pins THREAD_KSTACK_TOTAL bytes of UNSWAPPABLE kernel
//                      kstack (256 threads -> 8 MiB kstacks).
//   PROC_CHILD_MAX  -- live DIRECT children (the direct-fork rate).
#define PROC_PAGE_MAX   65536u   // 256 MiB at 4-KiB pages
// CF-3 A audit F1: per-Proc cap on TRANSIENT byte-I/O bounce heap (the
// SYS_RW_MAX kmalloc tier in the read/write/pread/pwrite handlers).
// 512 KiB = four concurrent 128-KiB bulk ops -- ample for the measured
// build workloads (84% of ops run at in-flight depth 1) while bounding
// the order-5 heap a Proc's BLOCKED ops can pin (a held-open pipe / idle
// socket / hung server holds its bounce for the block's duration) at
// 64x below the unbudgeted threads x SYS_RW_MAX. Over-budget ops degrade
// to the 4-KiB stack tier -- shorter, never failed.
#define PROC_BOUNCE_MAX (512u * 1024u)
#define PROC_THREAD_MAX 256
#define PROC_CHILD_MAX  256
// PROC_VMA_MAX -- live VMAs (the I-32 FOURTH axis; overcommit, ARCH section 6.5).
// The Linux vm.max_map_count analog. The eager attach path is already transitively
// bounded (each VMA charges >=1 page, so PROC_PAGE_MAX caps eager VMAs), but a FREE
// lazy reservation (SYS_BURROW_ATTACH_LAZY -- uncharged at attach) reopens a
// VMA-slab DoS this axis closes. Charged at vma_insert / uncharged at vma_remove,
// both under p->vma_lock; TCB-exempt. 65536 * sizeof(struct Vma) (64 B) = 4 MiB of
// Vma slab is the per-Proc ceiling.
#define PROC_VMA_MAX    65536u
// PROC_SHARED_MAP_MAX_PAGES -- cross-Proc shared-in mappings (G-2; the I-32
// FIFTH axis; TAPESTRY.md §18.12 R2-F3). burrow_share_into maps ANOTHER Proc's
// memory (a netd flow ring, a tapestryd weave) into this one: those pages are
// charged to the SHARER's own budget (netd's SYS_BURROW_ATTACH page_count /
// tapestryd's DMA pool), NOT the client's page_count -- so without this axis a
// client's shared-in footprint is unbounded AND (the R2-F3 crash leg) a client
// mapping pins a crashed compositor's DMA pages charged to nothing live. 128 MiB
// = room for two live weave generations of a large surface (≤ 2 x 64 MiB across
// a reweave) + a full complement of flow rings; a DoS floor, not an accountant.
// Charged in burrow_share_into / uncharged at the SHARED_IN-flagged VMA's
// teardown (burrow_unmap / vma_drain), all under p->vma_lock; TCB-exempt.
#define PROC_SHARED_MAP_MAX_PAGES  32768u   // 128 MiB at 4-KiB pages

struct Proc {
    u64               magic;            // PROC_MAGIC
    int               pid;
    int               thread_count;
    enum proc_state   state;             // P2-D
    int               exit_status;       // P2-D: 0 = clean; non-zero = error
    struct Thread    *threads;          // doubly-linked list head (Thread.next_in_proc)

    // P2-D: parent/children linkage. parent is set at rfork time and is
    // rewritten only by proc_reparent_children when the parent exits with
    // live children (no setpgrp at v1.0). children is the head of a singly-
    // linked list of child Procs chained via Proc.sibling. On exit, orphaned
    // children re-parent to init (g_init_proc; 2B-F3, ARCH 7.9 step 6),
    // falling back to kproc (PID 0) while init is not up.
    struct Proc      *parent;
    struct Proc      *children;
    struct Proc      *sibling;

    // P2-D: exit reporting. exit_msg is a pointer to a static or
    // caller-owned string ("ok" for clean exit, anything else for
    // failure). msg lifetime is the caller's responsibility — at v1.0
    // we only pass static literals. Phase 5+ (exec'd userspace) needs
    // a per-Proc exit_msg buffer to copy from user.
    const char       *exit_msg;

    // P2-D / #344: parent's child-reap wait list. Every Thread that calls
    // wait_pid_for registers its OWN stack-local poll_waiter here and parks
    // on its OWN private Rendez; a child entering ZOMBIE (the wakeup edge)
    // calls poll_waiter_list_wake under g_proc_table_lock, which wakes EVERY
    // registered waiter. This is the #344 multi-waiter lift the old single-
    // waiter `struct Rendez child_done` could not do: a 2nd concurrent waiter
    // tripped sleep's single-waiter assert (-> extinction), so the RW-2
    // 2B-F1/F2 `wait_active` guard had to REFUSE the 2nd caller (-1) rather
    // than serve it -- which broke multi-threaded Go's parallel `go build`.
    // `struct poll_waiter_list` is byte-identical in layout to the old
    // `struct Rendez` (a spinlock + a pointer), so the swap is offset-stable
    // (every following field keeps its pinned offset). The list is the
    // wake-RELAY only; the AUTHORITATIVE "is a matching zombie reapable"
    // decision is wait_pid_for's re-scan of `children` under g_proc_table_-
    // lock (register-then-observe, the poll.c discipline). poll_waiter_list_-
    // wake is called UNDER g_proc_table_lock (the established exits() order
    // proc_table_lock -> list -> rendez), so the parent stays alive through
    // the wake and no death-wake is lost (I-9).
    struct poll_waiter_list  child_waiters;

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
    // context_id is the rolling-ASID context (generation | hardware ASID) --
    // RW-1 B-F1; ARCH section 6.2.1. Replaces the per-Proc-permanent u16 asid.
    // 0 == "never assigned" (always misses). Resolved + re-stamped at context
    // switch by asid_resolve (the pre-hook) into the high bits of TTBR0_EL1;
    // never allocated at create nor freed at reap (rollover recycles the
    // space). kproc keeps context_id = 0 (pgtable_root == 0 -> kernel TTBR0,
    // bypassing the allocator). Accessed via __atomic_* (the fast path is
    // lockless). See arch/arm64/asid.{c,h}.
    paddr_t            pgtable_root;
    u64                context_id;

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

    // #344: formerly `wait_active`, the RW-2 2B-F1/F2 per-Proc wait_pid_for
    // serialization that refused a genuinely-concurrent 2nd waiter with -1.
    // The #344 multi-waiter lift -- a per-Thread stack `poll_waiter` on
    // `child_waiters` + the g_proc_table_lock-protected re-scan (replacing the
    // single-waiter `child_done` Rendez + the lockless `wait_pid_cond` walk
    // that the guard existed to protect) -- RETIRES the guard: all may wait.
    // The field is kept (now reserved) so every following field holds its
    // pinned offset; the size + offset _Static_asserts below depend on it.
    // KP_ZERO inits it to 0; nothing reads or writes it.
    u32                _reserved0;

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

    // #65 (invariant I-32): per-Proc resource-floor counters. A DoS bound,
    // NOT a privilege axis -- they cap a non-TCB Proc's resource use so a
    // fork/thread/memory bomb hits a clean limit instead of stressing the
    // allocator toward the box-killing cliff. `thread_count` (above) is the
    // third counter.
    //
    // page_count -- live anonymous pages committed via SYS_BURROW_ATTACH (the
    //   user-unbounded memory-bomb vector; anon is eager at v1.0 so attach is
    //   the single charge point). Charged += npages on a successful attach,
    //   -= npages on a successful detach, both UNDER `vma_lock` (the lock that
    //   already serializes the attach/detach path) -- so the page cap is
    //   EXACT (no TOCTOU overshoot). NOT charged: pgtable sub-tables
    //   (transitively bounded by mapped VA <= page_count), kstacks (bounded by
    //   the thread cap), the exec image (one-shot, bounded by the binary). At
    //   exit the Proc + this counter vanish together (no surviving aggregate at
    //   v1.0; vma_drain is the SEAM hook where a future aggregate uncharges).
    //   External stat readers use __atomic_load_n (a coherent snapshot).
    // child_count -- live DIRECT children == the length of the `children` list.
    //   ++ at proc_link_child, -- at proc_unlink_child, and re-based at
    //   proc_reparent_children (adopter += N) -- all under g_proc_table_lock.
    //   The child cap is checked EARLY in rfork_internal (before the heavy
    //   proc_alloc/territory_clone/thread_create), so it carries a bounded
    //   TOCTOU overshoot (<= ncpus-1 concurrent spawners) -- acceptable for a
    //   floor (a bound, not an exact accountant).
    // NEITHER is propagated by rfork (a child starts with its own zeroed
    // counts -- KP_ZERO). PRINCIPAL_SYSTEM Procs (the TCB) maintain the
    // counters for observability but are EXEMPT from the caps
    // (proc_resource_exempt).
    u32                page_count;
    u32                child_count;

    // I-34 (ARCH section 28; docs/MENAGERIE.md section 4; specs/allowance.tla):
    // the per-Proc hardware allowance -- the narrowing of CAP_HW_CREATE the
    // warden confers when it spawns a per-device driver. NULL = BROAD (the
    // warden + the existing trusted servers, bounded only by the I-5 kernel
    // reservation -- the as-built v1.0 behavior). Non-NULL = NARROWED: the
    // driver may create KObj_MMIO/IRQ/DMA only within the conferred set, and
    // only while not revoked. Heap-allocated by proc_confer_allowance; deep-
    // copied across rfork by allowance_clone_into (a narrowed parent's child
    // is equally narrowed -- the hardware-axis analog of caps' monotonic
    // reduction, I-2); freed by proc_free (allowance_free). KP_ZERO inits it
    // NULL (broad/none). See <thylacine/allowance.h>.
    struct Allowance  *allowance;

    // I-32 FOURTH axis (overcommit, ARCH section 6.5): live VMA count -- the DoS
    // bound a free (uncharged-at-attach) SYS_BURROW_ATTACH_LAZY reservation needs
    // (eager self-limits at ~PROC_PAGE_MAX VMAs since each charges >=1 page). ++ at
    // vma_insert (gated on PROC_VMA_MAX unless exempt), -- at vma_remove, both under
    // p->vma_lock (the VMA-list mutation domain) -> the cap is EXACT. Counted
    // uniformly for every VMA (attach / exec image / guard / DMA / Weft share). NOT
    // propagated by rfork (KP_ZERO -> 0). PRINCIPAL_SYSTEM is exempt from the CAP
    // (the count is still maintained for observability). All accesses use __atomic_*;
    // a FUTURE /proc reader (none is wired at this commit -- audit F2) MUST use
    // __atomic_load_n for a coherent cross-Proc snapshot, as page_count's reader does.
    // Like page_count, it is a resource axis, not a privilege axis (orthogonal to I-22).
    u32                vma_count;

    // G15 (ARCH section 9.7): the per-Proc environment group -- the Plan 9 Egrp
    // surfaced as the per-Proc /env directory (devenv). Lazily allocated (NULL ==
    // empty), deep-COPIED across rfork by env_clone_into (the Plan 9 default-copy;
    // the reserved RFENVG share-flag stays deferred), freed by proc_free
    // (env_free). KP_ZERO inits it NULL. Shared across a Proc's threads (one
    // p->env, env->lock serializes); never shared cross-Proc at v1.0. See
    // <thylacine/env.h>.
    struct Env        *env;

    // CF-3 A audit F1 (an I-32-shaped resource axis): live TRANSIENT bounce
    // bytes -- the byte-I/O syscall staging's heap tier currently allocated
    // by this Proc's in-flight reads/writes. Charged before the kmalloc,
    // uncharged at the free on every path. A blocked dev->read/write (a
    // held-open pipe, an idle socket, a hung server) holds its charge for
    // the block's duration, so the cap bounds the order-5 heap a Proc can
    // pin: an over-budget op DEGRADES to the stack tier (a short op, never
    // a failure). PRINCIPAL_SYSTEM is exempt per proc_resource_exempt (the
    // I-32 pattern; the counter is not maintained for exempt Procs -- the
    // charge and uncharge gates are symmetric). NOT propagated by rfork
    // (KP_ZERO -> 0). __atomic_* CAS charge / fetch_sub uncharge; no lock.
    u64                bounce_bytes;

    // 8a-1b (I-39; docs/DEBUG-FS-DESIGN.md section 7.2): the one-debugger attach
    // slot. NULL = not being debugged; non-NULL = the /proc/<pid>/ctl Spoor that
    // holds the debug attach (an IDENTITY TOKEN only -- NEVER dereferenced, only
    // compared for pointer-equality, so no spoor_ref is taken and the debugger's
    // fd Spoor and this target have independent lifetimes: no cross-ref, no UAF).
    // Guarded by g_proc_table_lock: claimed (attach), released (detach / the
    // ctl-fd close hook, incl. debugger death via #68/#926 close-at-exit), and
    // -- from 8a-1b-beta -- read by proc_group_terminate's death cascade, all
    // under proc_for_each. A second attach on a non-NULL slot is Einuse (the Plan
    // 9 one-debugger-per-target shape). KP_ZERO at proc_alloc inits it NULL, so a
    // reused Proc struct never carries a stale token. NOT propagated by rfork.
    struct Spoor      *debug_owner;

    // 8a-1b-beta (I-39; docs/DEBUG-FS-DESIGN.md section 4.2/4.4): the per-Proc
    // debugger stop flag (the model's `sflag`). 0 = run, 1 = a debugger has
    // requested every thread park at its next EL0-return checkpoint. SET (RELEASE)
    // by proc_debug_stop_deliver, CLEARED (RELEASE) by proc_debug_resume -- the
    // clear is ordered BEFORE the resume's per-peer wake so a thread registering
    // on the tail after the wake re-observes the cleared flag (the I-9 register-
    // then-observe close, the mirror of proc_group_terminate's set-before-walk).
    // READ (ACQUIRE) at the EL0-return tail (el0_return_stop_check) + in the park
    // wake-cond. A plain u32 driven by __atomic_* (the group_exit_msg / proc_flags
    // idiom). KP_ZERO -> 0 (a reused struct never carries a stale stop); NOT
    // propagated by rfork (a spawned child is not being debugged).
    u32                debug_stop_req;

    // PTY-1f (I-20 stop leg; PTY-DESIGN.md section 4; specs/pty_stop.tla
    // stopOwners): the SECOND, INDEPENDENT stop owner -- the job-control
    // stop (an uncaught tty:susp on a foreground-group member). A thread
    // parks at its EL0-return checkpoint (or the sleep/tsleep stop detour)
    // iff `debug_stop_req | job_stop_req` (proc_stop_requested), and each
    // resume clears ONLY its own owner: proc_job_resume_one_locked clears
    // THIS flag (the tty:cont fan / SYS_TTY_CONT / the F8 teardown + orphan
    // rule), proc_debug_resume clears only debug_stop_req -- a tty:cont can
    // never run a debugger-stopped thread (StopCompatI39; the
    // BUGGY_DOUBLE_STOP counterexample). Death overrides both (the park
    // loop's group_exit_msg check precedes; GroupDie clears stopOwners).
    // SET (RELEASE) by proc_job_stop_pgrp's uncaught-susp arm under
    // g_proc_table_lock, CLEARED (RELEASE) by the job-resume paths under the
    // same lock; READ (ACQUIRE) at the park predicate sites, exactly the
    // debug_stop_req discipline. Occupies the pad slot after debug_stop_req
    // (same cache line as the debug flag -- the tail's fast path reads both;
    // no struct growth). KP_ZERO -> 0; NOT propagated by rfork.
    u32                job_stop_req;

    // 8a-2b (I-39; DEBUG-FS-DESIGN section 5): the per-Proc hardware-breakpoint
    // table (struct debug_hw, arch/arm64/hwdebug.h). NULL = no breakpoints (the
    // overwhelmingly common case); non-NULL = lazily kmalloc'd on the first
    // `hwbreak` ctl verb and freed at proc_free (never at detach -- so the
    // context-switch reader hwdebug_switch_in never derefs freed memory; detach
    // clears the table to bp_count=0 instead). The POINTER is set once (RELEASE)
    // under g_proc_table_lock when the target is fully-stopped, read (ACQUIRE) in
    // the ctx-switch install hook + the EC-0x30 handler. The table CONTENTS
    // (bp_count/bp_va) are mutated only stopped (add/remove) or benign-running
    // (detach's count=0), guarded by the atomic bp_count. KP_ZERO inits it NULL;
    // NOT propagated by rfork (a spawned child is not being debugged).
    struct debug_hw   *debug_hw;

    // 8c-2 #95 (I-39; DEBUG-FS-DESIGN section 5c): the debugger's FOCUS thread --
    // the M whose frame /proc/<pid>/{regs,fpregs,kregs,kstack} + the step verb
    // report. NULL = report the head thread (the manual-stop / attach default). A
    // multi-M Go breakpoint fires on whichever M runs the migrated goroutine, NOT
    // the head (TID==PID), so without this the debug-fs reports the head M's PC
    // (not the bp) and the debugger cannot attribute the stop -> an auto-resume
    // loop. SET (RELEASE) to current_thread() by proc_debug_fault_stop -- the ONE
    // path every bp/step/wp EC fire routes through, so the firing M becomes the
    // focus uniformly. CLEARED (RELEASE) to NULL by proc_debug_resume (the focus is
    // valid only for the current stop; a manual ctl `stop` follows a resume, so it
    // reads NULL -> head, preserving the attach/8a-2b single-thread behavior).
    // READ (ACQUIRE) under g_proc_table_lock: devproc_focus_thread validates the
    // pointer is still a thread of `target` (else head). NO UAF -- and the
    // load-bearing reason is the GPTL PIN, not a stop gate (#95-audit F1): reap
    // (wait_pid_for) does proc_unlink_child under g_proc_table_lock BEFORE the
    // lock-free thread_free loop, and proc_for_each walks the kproc-rooted children
    // tree, so a target a reader can still reach has not been unlinked -> none of
    // its threads is freed; the in-list validation then rejects any stale/foreign
    // pointer. The fully-stopped gate is an ADDITIONAL property of the mem / regs /
    // kregs / step readers but NOT of the 8b settled kstack, so it is not the
    // safety net -- the pin + validation are (do not delete the validation loop as
    // "redundant"). KP_ZERO inits it NULL; NOT propagated by rfork.
    struct Thread     *debug_focus_thread;

    // PTY-1a (PTY-DESIGN.md section 4; POSIX sessions + process groups).
    // sid = the session id (the session leader's pid); pgid = the process-
    // group id (the group leader's pid). UNLIKE the debug/resource tail
    // fields these ARE rfork-INHERITED (a child joins its parent's session
    // + group -- POSIX fork semantics; rfork_internal copies them in the
    // identity-inherit block). A parentless Proc defaults to its own
    // session/group (sid = pgid = pid, stamped beside the pid at both
    // assignment sites). Mutated ONLY by proc_setsid / proc_setpgid under
    // g_proc_table_lock; read by the tty seam (fg-pgid routing, PTY-1d),
    // notes_post_pgrp (PTY-1b), and the SYS_WAIT_PID pgrp selectors
    // (PTY-1e). Values are pids (< 2^31), stored u32 per the design.
    u32                sid;
    u32                pgid;

    // PTY-1e (PTY-DESIGN.md section 4; the wait extension). The per-child
    // report latches -- the Linux CLD_STOPPED/CLD_CONTINUED shape. A
    // WAIT_UNTRACED / WAIT_CONTINUED wait_pid_for REPORTS a latched child
    // (returns its pid + a packed status WITHOUT reaping -- report-is-not-
    // reap, round-2 R2-F6) and consumes the latch exactly once, under
    // g_proc_table_lock. SET by the job-stop park / tty:cont resume
    // (PTY-1f -- the setters also wake the parent's child_waiters, the
    // fresh I-9 edge; until 1f nothing sets them, so the report arms are
    // complete-but-quiescent). KP_ZERO-fresh false; never rfork-inherited
    // (a new child has no pending report). A latch set while no wait
    // requests its flag stays LATCHED (a plain wait neither sees nor
    // consumes it).
    bool               stop_report_pending;
    bool               cont_report_pending;

    // G-2 (the I-32 FIFTH axis; TAPESTRY.md §18.12 R2-F3): pages of OTHER
    // Procs' memory currently shared INTO this Proc via burrow_share_into
    // (SHARED_IN-flagged VMAs). Charged/uncharged under p->vma_lock (exact);
    // atomic store for lockless /proc readers, like page_count. KP_ZERO-fresh
    // 0; never rfork-inherited (a child has no shared-in mappings -- its
    // address space starts empty). Occupies the former 348..352 tail pad, so
    // struct Proc stays 352 bytes.
    u32                shared_map_pages;
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
// PTY-1b: the tty-class twin of the LS-5c latch above -- a terminate-
// disposition tty:quit / tty:hup is pending (posted with no handler + not
// self-managing). A SEPARATE bit, not a widening of the interrupt latch,
// because the lock-free #811 die-pending predicate pairs each latch with
// ITS OWN family mask bit (interrupt <-> NOTE_BIT_INTERRUPT, tty <->
// NOTE_BIT_TTY) -- one shared latch would make a thread that masked only
// interrupts spuriously *_INTR-unwind for a pending tty note it HAS
// masked, breaking the "a latch-woken thread never unwinds into a tail
// that refuses to act" property. Same latch discipline as the interrupt
// bit: set/cleared ONLY under p->notes->lock (armed by notes_post's
// terminate-class arm; cleared by handler registration, the self-managing
// mark, or draining the last queued terminate-class tty note); NOT
// propagated by rfork; never set on kproc.
#define PROC_FLAG_TTY_TERMINATE_PENDING (1u << 8)
// G-4 (TAPESTRY.md section 18.12 R2-F6): marks the bound console RENDERER --
// the Proc granted the /dev/consdrain + /dev/consfeed pair (Aurora). The THIRD
// console role, orthogonal to BOTH console-ATTACH (the elevation gate; the
// renderer has NO elevation authority) and console-OWNER (the Ctrl-C target;
// the drain/feed conveys no interrupt-target authority) -- I-27's three-role
// split. Kernel-stamped via SPAWN_PERM_CONSOLE_RENDERER only (never a
// syscall self-mark); one-way; NOT propagated by rfork. Single-holder: the
// stamp succeeds only while g_console_renderer is NULL (proc_set_console_-
// renderer's claim under g_proc_table_lock); cleared on the holder's death.
#define PROC_FLAG_CONSOLE_RENDERER  (1u << 9)

_Static_assert(sizeof(struct Proc) == 352,
               "struct Proc size pinned at 352 bytes (the 328 baseline + the 8c-2 "
               "#95 debug_focus_thread pointer @328 + the PTY-1a sid/pgid pair "
               "@336/@340 + the PTY-1e report latches @344/@345 + the G-2 "
               "shared_map_pages @348 in the former tail pad -- NO size growth). "
               "Adding a field grows the SLUB cache; update this assert "
               "deliberately so the change is intentional.");
_Static_assert(__builtin_offsetof(struct Proc, shared_map_pages) == 348,
               "G-2 shared_map_pages (the I-32 fifth axis: cross-Proc shared-in "
               "pages) occupies the former 348..352 tail pad after the PTY-1e "
               "report latches; KP_ZERO-fresh 0, never rfork-inherited.");
_Static_assert(__builtin_offsetof(struct Proc, debug_focus_thread) == 328,
               "8c-2 #95 debug_focus_thread (the debug-fs focus M) appends after "
               "debug_hw (offset 328, the next 8-aligned slot past the pointer @320); "
               "existing offsets stay stable (KP_ZERO inits it NULL = report head).");
_Static_assert(__builtin_offsetof(struct Proc, sid) == 336,
               "PTY-1a sid (the POSIX session id) follows debug_focus_thread (offset "
               "336, the next 4-aligned slot); rfork-INHERITED, unlike the KP_ZERO "
               "debug/report fields around it.");
_Static_assert(__builtin_offsetof(struct Proc, pgid) == 340,
               "PTY-1a pgid (the POSIX process-group id) follows sid (offset 340).");
_Static_assert(__builtin_offsetof(struct Proc, stop_report_pending) == 344,
               "PTY-1e stop_report_pending appends after pgid (offset 344; "
               "KP_ZERO-fresh false, never rfork-inherited).");
_Static_assert(__builtin_offsetof(struct Proc, cont_report_pending) == 345,
               "PTY-1e cont_report_pending follows (offset 345).");
_Static_assert(__builtin_offsetof(struct Proc, debug_hw) == 320,
               "8a-2b debug_hw (the per-Proc HW-breakpoint table pointer) appends "
               "after debug_stop_req (offset 320, the next 8-aligned slot past the "
               "u32 @312 + its pad -- the pad PTY-1f's job_stop_req now occupies); "
               "existing offsets stay stable (KP_ZERO inits it NULL = no "
               "breakpoints).");
_Static_assert(__builtin_offsetof(struct Proc, job_stop_req) == 316,
               "PTY-1f job_stop_req (the second stop owner, I-20) occupies the pad "
               "slot after debug_stop_req @312 -- same cache line as the debug flag "
               "(the EL0-return tail's fast path reads both), NO struct growth, "
               "every existing offset stable.");
_Static_assert(__builtin_offsetof(struct Proc, debug_stop_req) == 312,
               "8a-1b-beta debug_stop_req (the I-39 per-Proc debugger stop flag) "
               "appends after debug_owner (offset 312, the next slot past the Spoor* "
               "@304); existing offsets stay stable (KP_ZERO inits it 0 = run).");
_Static_assert(__builtin_offsetof(struct Proc, bounce_bytes) == 296,
               "CF-3 A bounce_bytes appends after the G15 env pointer (offset 296); "
               "existing offsets stay stable (KP_ZERO inits it 0).");
_Static_assert(__builtin_offsetof(struct Proc, debug_owner) == 304,
               "8a-1b debug_owner (the I-39 one-debugger attach slot) appends after "
               "bounce_bytes (offset 304, the next 8-aligned slot past the u64 @296); "
               "existing offsets stay stable (KP_ZERO inits it NULL = not debugged).");
_Static_assert(__builtin_offsetof(struct Proc, vma_count) == 280,
               "I-32 fourth-axis vma_count appends after the I-34 allowance pointer "
               "(offset 280); existing offsets stay stable (KP_ZERO inits it 0).");
_Static_assert(__builtin_offsetof(struct Proc, env) == 288,
               "G15 per-Proc env pointer appends after vma_count (offset 288, the "
               "next 8-aligned slot past the u32 @280 + its pad); existing offsets "
               "stay stable (KP_ZERO inits it NULL = empty).");
_Static_assert(__builtin_offsetof(struct Proc, page_count) == 264,
               "#65 resource-floor counters append after the A-4a legate "
               "block; existing offsets stay stable (KP_ZERO inits them 0).");
_Static_assert(__builtin_offsetof(struct Proc, child_count) == 268,
               "child_count follows page_count in the #65 resource-floor "
               "block (offset 268).");
_Static_assert(__builtin_offsetof(struct Proc, allowance) == 272,
               "I-34 hardware-allowance pointer appends after the #65 "
               "resource-floor block (offset 272, 8-byte aligned); existing "
               "offsets stay stable (KP_ZERO inits it NULL = broad).");
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

// 2B-F3: publish `p` as init — the orphan-adopter (ARCH section 7.9
// step 6). Called once per boot from joey_thunk in the child's own
// context, before exec. Extincts on double-publish or a corrupted Proc.
void proc_publish_init(struct Proc *p);

// Accessor for init (joey, the first user Proc). NULL while init is not
// up — early boot before joey, the in-kernel test phase, or after init
// died; proc_reparent_children falls back to kproc then. Lock-free
// (acquire load paired with the publish/clear store-release).
struct Proc *proc_init_proc(void);

// SLUB-allocate a fresh Proc descriptor. Initializes pid (next monotonic
// pid), threads = NULL, thread_count = 0, state = ALIVE, child_waiters
// initialized. parent / children / sibling left NULL (caller wires
// linkage). Returns NULL on OOM.
struct Proc *proc_alloc(void);

// Release a Proc descriptor. Caller must ensure thread_count == 0
// (no live threads) and state == ZOMBIE (or post-reap path). Extincts
// on violation.
void proc_free(struct Proc *p);

// #65 (invariant I-32): the per-Proc resource floor.
//
// proc_resource_exempt -- the TCB (PRINCIPAL_SYSTEM: kproc + the boot/service
//   chain) is exempt from the caps so the floor cannot pinch the FS server /
//   the orphan-adopter / the kthread root. UNFORGEABLE: a post-login Proc
//   cannot acquire PRINCIPAL_SYSTEM (CAP_SET_IDENTITY rejects it, §3.3), and
//   principal_id is immutable on a running Proc, so a plain read is sound. A
//   NULL p is treated as non-exempt (fail-closed).
bool proc_resource_exempt(const struct Proc *p);

// proc_page_charge / proc_page_uncharge -- the SYS_BURROW_ATTACH anon-page
//   counter. The CALLER MUST HOLD p->vma_lock (the lock that serializes the
//   attach/detach path), so the check + charge is atomic against sibling
//   attaches and the page cap is EXACT. charge returns true (and adds npages)
//   if the Proc is exempt OR the new total fits PROC_PAGE_MAX; false (charging
//   nothing) if it would exceed (caller rejects with -ENOMEM) or npages would
//   overflow. uncharge clamp-subtracts (never underflows past 0).
bool proc_page_charge(struct Proc *p, u32 npages);
void proc_page_uncharge(struct Proc *p, u32 npages);

// proc_vma_charge / proc_vma_uncharge -- the live-VMA counter (I-32 FOURTH axis;
//   the overcommit VMA-slab DoS bound). The CALLER MUST HOLD p->vma_lock (the
//   vma_insert/vma_remove domain), so the check + charge is atomic against a
//   sibling attach and the cap is EXACT. charge returns true (and ++vma_count) if
//   the Proc is exempt OR vma_count < PROC_VMA_MAX; false (charging nothing) if it
//   would exceed (the vma_insert caller rejects with -1, like an overlap) or the
//   counter would saturate. uncharge clamp-decrements (never underflows past 0).
bool proc_vma_charge(struct Proc *p);
void proc_vma_uncharge(struct Proc *p);

// proc_shared_map_charge / proc_shared_map_uncharge -- the cross-Proc shared-in
//   mapping counter (G-2; the I-32 FIFTH axis). The CALLER MUST HOLD p->vma_lock
//   (burrow_share_into's precondition -- the same domain as the flagged-VMA
//   teardown sites), so the check + charge is atomic against sibling shares and
//   the cap is EXACT. charge returns true (and adds npages) if the Proc is
//   exempt OR the new total fits PROC_SHARED_MAP_MAX_PAGES; false (charging
//   nothing) on exceed/overflow (the share fails clean, -1 -- the flow stays
//   byte-copy / the surface map fails, never a box event). uncharge
//   clamp-subtracts (never underflows past 0).
bool proc_shared_map_charge(struct Proc *p, u32 npages);
void proc_shared_map_uncharge(struct Proc *p, u32 npages);

// proc_thread_cap_ok -- the thread-spawn gate. Returns true if the Proc is
//   exempt OR thread_count < PROC_THREAD_MAX. Takes g_proc_table_lock for the
//   read (the thread_count write domain). The check and the later
//   thread_link_into_proc ++ are at different lock holds, so a bounded TOCTOU
//   overshoot (<= ncpus-1 concurrent spawners) is possible -- acceptable for a
//   floor.
bool proc_thread_cap_ok(struct Proc *p);

// proc_child_cap_ok -- the rfork/spawn gate. Returns true if the Proc is exempt
//   OR child_count < PROC_CHILD_MAX. Takes g_proc_table_lock for the read (the
//   child_count write domain). Checked EARLY in rfork_internal (before the heavy
//   alloc), so it carries the same bounded TOCTOU overshoot as the thread cap.
bool proc_child_cap_ok(struct Proc *p);

// RW-7 R3-F1: reset every virtio device this Proc drove (via its KObj_MMIO
// handles) before its KObj_DMA pages free back to the buddy allocator, so a
// driver that died abnormally cannot DMA into recycled memory. Called from
// the proc-exit handle-close sites (proc_close_handles_at_exit + proc_free);
// must run only on a quiesced handle table (no peer mutator). Returns the
// number of devices reset (a no-op returning 0 on a NULL Proc / NULL table).
// Exposed for the device-death-quiesce regression test.
int proc_quiesce_owned_devices(struct Proc *p);

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
//   3. Wake parent's child_waiters -- every Thread waiting in wait_pid_for
//      (if a parent exists). #344 multi-waiter.
//   4. yield via sched(). exits() never returns; the thread is left in
//      EXITING state until wait() reaps it.
//
// At v1.0 P2-D, only a single thread per Proc is supported (mirrors
// what rfork(RFPROC) creates). Multi-threaded Procs at Phase 5+ require
// exits() to terminate ALL threads of the Proc atomically (a Phase 2
// close refinement).
//
// Re-parenting of orphaned children: if the exiting Proc has children, they
// re-parent to init (g_init_proc), or to kproc() while init is not up. The
// adopter's child_waiters wake IS load-bearing: a child adopted already-ZOMBIE gets an
// explicit wakeup so an adopter blocked in wait_pid reaps it rather than
// sleeping over it (the I-9 fix in proc_reparent_children); init reaps adoptees
// via its getty supervisor loop.
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
//      the Proc ZOMBIE with exit_status = 0 + wake parent's child_waiters
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

// 8a-1b-beta debugger stop/resume (I-39; docs/DEBUG-FS-DESIGN.md section 4.2/4.4;
// specs/debug_stop.tla). The stop machinery sits on the death-path lineage and
// composes with it (DeathWinsOverStop): the EL0-return tail runs the stop-check
// AFTER el0_return_die_check, so death always wins; a resume re-runs the die-
// check on unpark.
//
// proc_debug_stop_deliver: request every thread of `p` park at its next EL0-
// return checkpoint. Sets p->debug_stop_req (RELEASE), then (8c-2) WAKES every
// sleeping peer (the NON-COMPLETING torpor_stop_wake_all_for_proc [#19: the
// death walk's completing wake fabricates TORPOR_OK, which a surviving stopped
// Proc observes at resume; the stop wake preserves the wait] + the per-peer
// wait_lock rendez wake) so a thread blocked in an indefinite syscall
// sleep returns, re-observes the flag, and DETOURS to park on its debug_rendez
// (proc_stop_sleeper_park), then smp_resched_others() so a peer RUNNING at
// EL0 on another CPU traps to its tail (the periodic tick is the floor). A thread
// already in the kernel observes the flag when its syscall/handler returns to the
// tail. LOCK CONTRACT: caller holds g_proc_table_lock (mirrors
// proc_group_terminate; the flag-set + wake + IPI take no sleeping lock; the
// per-peer wait_lock walk is g_proc_table_lock -> wait_lock -> r->lock).
void proc_debug_stop_deliver(struct Proc *p);

// proc_stop_sleeper_park: the nested stop park a blocking sleep()/tsleep()
// detours into when ANY stop is pending -- a debugger stop (8c-2,
// DEBUG-FS-DESIGN 5c.2) or a job-control stop (PTY-1f; the park predicate is
// proc_stop_requested's disjunction). Sleeps on the caller's own debug_rendez
// (the shared stop-park rendez -- one park, two owners, per-owner clears)
// until BOTH stop owners clear; returns SLEEP_OK (re-check the original wait
// cond + re-block) or SLEEP_INTR (dying/soft-int while stop-parked -> unwind
// + die at the tail; DEATH WINS -- pty_stop.tla DeathWinsOverJobStop is the
// job-owner leg). Called from sched.c's sleep()/tsleep() detour with their
// wait loop locks RELEASED. The `r != &debug_rendez` detour gate makes this
// nested park (r == debug_rendez) skip the stop-check -> no recursion.
// (Named proc_debug_stop_sleeper_park before PTY-1f generalized the cond.)
int proc_stop_sleeper_park(struct Thread *t);

// proc_debug_fault_stop: the EC-path (hardware bp/wp hit or single-step
// completion) stop delivery. Unlike proc_debug_stop_deliver (the ctl `stop`
// verb, which runs under g_proc_table_lock via proc_for_each with debug_owner
// already verified), a hardware fire arrives in the TARGET's own exception
// context holding no lock, so it must SERIALIZE with a concurrent detach / ctl-fd
// close (which clears debug_owner + the hw table + calls proc_debug_resume, all
// under g_proc_table_lock). This takes g_proc_table_lock and delivers the stop
// ONLY while a debugger still owns the slot (p->debug_owner != NULL), then
// returns whether it delivered. Without the gate a fire's debug_stop_req store
// could land AFTER a detach's proc_debug_resume cleared it, parking the target
// with no debugger left to resume it -- the 8a-2 SA-1 strand (specs/debug_stop.
// tla StopImpliesOwned / NoStrand). Returns false (deliver skipped) when the
// debugger detached in the race window; the EC caller then treats the fire as a
// benign STALE arm (disables this CPU's debug regs + resumes the instruction).
bool proc_debug_fault_stop(struct Proc *p);

// proc_debug_resume: resume `p` -- clear p->debug_stop_req (RELEASE, ordered
// BEFORE the wake so a thread registering at the tail after the walk re-observes
// the cleared flag: the I-9 register-then-observe close, the mirror of
// proc_group_terminate's set-before-walk) then wake every thread parked on its
// OWN debug_rendez (walk p->threads, per-peer wait_lock -> wakeup, exactly the
// #811 death-cascade shape, waking only debug-parked peers). Drives the model's
// StartResume (start verb) AND ReleaseSlot (detach / ctl-fd close / debugger
// death) -- the release paths in devproc.c call this after clearing debug_owner,
// so a dead/detached debugger provably resumes its quarry (NoStrand). LOCK
// CONTRACT: caller holds g_proc_table_lock (the p->threads walk + the
// g_proc_table_lock -> wait_lock -> r->lock chain, acyclic).
// PTY-1f (pty_stop.tla ResumeDebug): clears ONLY the debug owner -- a woken
// thread whose Proc is ALSO job-stopped re-checks the park cond (both flags)
// and re-parks; job_stop_req is never touched here (StopCompatI39's twin:
// neither resume may clear the other's owner).
void proc_debug_resume(struct Proc *p);

// =============================================================================
// PTY-1f: the job-control stop (I-20 stop leg; PTY-DESIGN.md section 4;
// specs/pty_stop.tla). The SECOND stop owner beside the debugger's -- same
// park (debug_rendez + the EL0-return tail / sleep-detour machinery), its own
// flag (job_stop_req), per-owner clears.
// =============================================================================

// The park predicate: is ANY stop owner requesting this Proc parked? The
// EL0-return tail, the sleep()/tsleep() stop detours, the 9P client's
// client_stop_pending, and the elected-reader handoff skip ALL read this
// disjunction (round-2 R2-F2: a flag the audited park machinery does not read
// re-opens the #89 whole-FS freeze via the job axis). Two ACQUIRE loads off
// one cache line (job_stop_req occupies debug_stop_req's pad slot).
static inline bool proc_stop_requested(const struct Proc *p) {
    return (__atomic_load_n(&p->debug_stop_req, __ATOMIC_ACQUIRE) |
            __atomic_load_n(&p->job_stop_req,  __ATOMIC_ACQUIRE)) != 0;
}

// proc_job_stop_pgrp: the SYS_TTY_SIGNAL TSTP fan-out -- deliver the
// job-control suspend to every ALIVE member of `pgid` under ONE
// g_proc_table_lock hold (the notes_post_pgrp shape: membership read under
// the lock that serializes setpgid/rfork/exit -- never a half-delivered
// group). Per member, the catchability gate (round-2 R2-F3, the LS-5
// notes_interrupt_should_terminate_locked analog) decides the disposition:
//   - CAUGHT (an async handler registered, OR self-managing via the notes
//     fd, OR every thread masks NOTE_BIT_TTY): the tty:susp note is POSTED
//     (delivered/deferred on the target's own terms) and NO stop happens.
//     (The all-masked case is note-only at v1.0: the POSIX stop-on-unmask
//     of a pending stop signal is a recorded deviation -- masking defers
//     the note, and the stop disposition is evaluated once, at post.)
//   - UNCAUGHT + the group ORPHANED (no member has a parent in the same
//     session but another group -- nobody to resume it): DISCARDED entirely
//     (the POSIX orphan rule's stop-suppression half; no note, no stop).
//   - UNCAUGHT otherwise: the default STOP -- job_stop_req set + the
//     sleeper-wake cascade (the proc_debug_stop_deliver shape) + the
//     stop_report_pending latch + the parent's child_waiters wake (the
//     PTY-1e WAIT_UNTRACED edge). The signal is CONSUMED by the default
//     action (no note queued -- nothing stays pending across the stop,
//     POSIX-exact; a stale susp delivered after a later cont would
//     re-suspend a resumed program).
// An already-job-stopped member is skipped (a second Ctrl-Z on a stopped
// group is a no-op -- POSIX discards a stop signal for a stopped process).
// pgid 0 refused (the notes_post_pgrp precedent). Returns the count of
// members affected (posted or stopped). Lock-free callers only (takes
// g_proc_table_lock itself; the pts seam calls it with g_pts_lock RELEASED).
int proc_job_stop_pgrp(u32 pgid);

// proc_job_cont_pgrp: the tty:cont fan-out -- SYS_TTY_CONT (the shell's
// `fg`/`bg`), the F8 pts-teardown resume, and the orphan rule's cont half.
// Under one g_proc_table_lock hold, every ALIVE member of `pgid` gets the
// tty:cont note POSTED (catchable-informational, best-effort -- the resume
// side effect is the kernel's, not a note disposition) and, iff job-stopped,
// its job_stop_req CLEARED + its debug_rendez-parked threads woken (the
// proc_debug_resume walk -- a member ALSO debugger-stopped re-parks on the
// still-set debug flag: the per-owner clear, pty_stop.tla ResumeJob vs
// BUGGY_DOUBLE_STOP) + cont_report_pending latched + the parent's
// child_waiters woken (the PTY-1e WAIT_CONTINUED edge). The resume is
// UNCONDITIONAL on catchability (POSIX: SIGCONT resumes whether or not
// caught; a handler observes the note after resuming). pgid 0 refused.
// Returns the count of ALIVE members visited.
int proc_job_cont_pgrp(u32 pgid);

// 8a-1b-beta EL0-return-tail stop-check (specs/debug_stop.tla TailStep). Called
// at every return-to-EL0 AFTER el0_return_die_check (+ notes on the sync tail),
// so death/interrupt win over a stop. Fast-paths out when NO stop is pending
// (neither owner -- PTY-1f: the check reads proc_stop_requested's
// debug|job disjunction); otherwise parks the calling Thread on its own
// debug_rendez (register-then-observe under wait_lock) until BOTH owners
// clear, re-checking group death (terminate here, never eret) on every wake.
// Returns to the tail (-> eret) when the stop
// is cleared or a soft interrupt-terminate must be delivered at the next tail.
// 8a-1c: `ctx` is the vector-supplied EL0 trapframe pointer (== the current SP);
// recorded into the Thread so /proc/<pid>/regs reads the RIGHT saved frame (its
// kstack offset is not fixed -- see thread.h debug_trapframe).
struct exception_context;
void el0_return_stop_check(struct exception_context *ctx);

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

// PTY-1e (PTY-DESIGN.md section 4): the job-control wait flags. Mirror
// POSIX WUNTRACED / WCONTINUED. A wait that passes either OPTS INTO the
// packed status encoding below (a plain wait keeps the raw exit status --
// full compatibility for every pre-PTY caller); a stop/continue REPORT
// returns the child's pid + packed status WITHOUT reaping (round-2 R2-F6:
// only an exit runs the unlink/thread_free/proc_free teardown) and
// consumes the per-child latch exactly once. Job stops only -- a DEBUGGER
// stop (I-39) is never waitpid-visible; the debug surface has its own
// /proc wait file.
#define WAIT_UNTRACED  2
#define WAIT_CONTINUED 4

// The packed wait-status encoding (ABI; the Linux wait(2) layout so the
// Pouch boundary-line maps 1:1). In effect ONLY when the caller passed
// WAIT_UNTRACED and/or WAIT_CONTINUED:
//   exited:    (code & 0xff) << 8         (WAIT_IF_EXITED; WAIT_EXITSTATUS)
//   stopped:   0x7f | (20 << 8)           (20 = the SIGTSTP-shaped job-stop
//                                          cause, the Linux value, so the
//                                          boundary-line is the identity)
//   continued: 0xffff
#define WAIT_STATUS_EXITED(code)  ((((code) & 0xff)) << 8)
#define WAIT_STATUS_STOPPED       (0x7f | (20 << 8))
#define WAIT_STATUS_CONTINUED     0xffff
#define WAIT_IF_EXITED(st)        (((st) & 0x7f) == 0)
#define WAIT_EXITSTATUS(st)       (((st) >> 8) & 0xff)
#define WAIT_IF_STOPPED(st)       (((st) & 0xff) == 0x7f)
#define WAIT_IF_CONTINUED(st)     ((st) == 0xffff)

// wait_pid_for — reap a ZOMBIE child (or, under the PTY-1e flags, REPORT a
// stopped/continued one), optionally filtered by pid/pgrp and/or
// non-blocking. The selection-and-blocking generalization underlying
// SYS_WAIT_PID and the wait_pid wrapper.
//
//   want_pid == -1     : any child (the original wait_pid semantics).
//   want_pid  >  0     : only the child with that pid.
//   want_pid == 0      : any child in the CALLER's process group (the
//                        caller's pgid read at each scan -- POSIX).
//   want_pid  < -1     : any child in process group -want_pid.
//   flags & WAIT_WNOHANG   : do not block (see WAIT_WNOHANG).
//   flags & WAIT_UNTRACED  : also report a child with a pending stop
//                            latch (packed WAIT_STATUS_STOPPED).
//   flags & WAIT_CONTINUED : also report a pending continue latch
//                            (packed WAIT_STATUS_CONTINUED).
//
// Report precedence: an exit outranks a continue outranks a stop (a zombie
// frees resources; the order among reportables is otherwise unspecified by
// POSIX). A child leaving the waited pgrp mid-wait (setpgid) simply stops
// matching at the next authoritative re-scan; if no matching child
// remains, the wait answers -1 (the POSIX ECHILD shape).
//
// Returns:
//   > 0 : the child's pid. For a reap, *status_out holds the exit status
//         (raw, or WAIT_STATUS_EXITED(code) when a PTY-1e flag was
//         passed); for a report, the packed stop/continue status.
//   0   : WAIT_WNOHANG set and a matching child is alive with nothing to
//         report. 0 is never a valid child pid (g_next_pid starts at 1),
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

// PTY-1a (PTY-DESIGN.md section 4): POSIX sessions + process groups. The
// SYS_SETSID/SETPGID/GETPGID/GETSID cores -- find + validate + mutate under
// one g_proc_table_lock hold. proc_setsid returns the new sid (> 0) or
// -T_E_ACCES (caller already a group leader); proc_setpgid returns 0 /
// -T_E_SRCH / -T_E_ACCES / -T_E_INVAL per the POSIX contours (EPERM cases
// map to -T_E_ACCES -- the errno.h -1-alias rule); the reads return the id
// or -T_E_SRCH (pid 0 = the caller; ZOMBIEs answer reads -- waitpid(-pgid)
// matches zombies by pgid -- but reject the setpgid mutation).
int proc_setsid(struct Proc *p);
int proc_setpgid(struct Proc *self, int pid, int pgid);
int proc_getpgid(struct Proc *self, int pid);
int proc_getsid(struct Proc *self, int pid);

// PTY-1d: true iff `pgid` names a process group with at least one ALIVE
// member in session `sid` (one g_proc_table_lock walk). The tcsetpgrp
// membership gate -- POSIX requires the new foreground group to be an
// existing group in the caller's session (a dead-members-only group cannot
// receive signals, so ZOMBIEs do not count here, unlike the getpgid read).
bool proc_pgrp_in_session(u32 pgid, u32 sid);

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
// (the unset side the A-4c-2 SAK + the A-5a proc_console_relinquish both use). The
// AND is atomic (multi-writer bit; see proc_mark_console_attached). A
// no-op on a NULL/corrupt Proc (fail-closed). Owner-lifetime is the
// caller's responsibility — proc_console_sak calls this under
// g_proc_table_lock with the owner pinned.
void proc_revoke_console_attached(struct Proc *p);

// proc_is_console_attached — true iff `p` carries
// PROC_FLAG_CONSOLE_ATTACHED. Returns false for a NULL `p`. The read is an
// atomic load (the bit is multi-writer post-A-4c-2).
bool proc_is_console_attached(const struct Proc *p);

// proc_is_console_owner — true iff `p` is the current console owner (the Ctrl-C
// target = the session shell, SPAWN_PERM_CONSOLE_OWNER). Compares the owner
// pointer under g_proc_table_lock WITHOUT dereferencing it (no UAF). RW-11
// SA-1b: gates the devcons_read INTERACTIVE promotion to the trusted console
// session. Fail-closed on NULL.
bool proc_is_console_owner(const struct Proc *p);

// A-4c-1: the kernel console owner (the trusted-path anchor for /dev/cons).
//
// proc_set_console_owner — record `p` as the current console owner = the
// `interrupt`/Ctrl-C target (joey at boot; login's session shell via
// SPAWN_PERM_CONSOLE_OWNER). DISTINCT from the console-ATTACH (the elevation
// authority): RW-7 R2-F1 -- the SAK clears the owner to NULL and grants the
// ATTACH only; it does NOT make corvus the owner. Takes g_proc_table_lock. Pass
// NULL to clear. proc_become_zombie_locked also clears it when the owner dies,
// so the pointer never dangles.
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

// proc_set_console_renderer — claim `p` as the bound console RENDERER (G-4,
// the R2-F6 third console role: the Aurora drain/feed holder). Single-holder:
// under g_proc_table_lock, succeeds (0) only when no live renderer is
// recorded, stamping PROC_FLAG_CONSOLE_RENDERER + g_console_renderer = p;
// returns -1 when a renderer already holds the role (the loser proceeds
// WITHOUT the flag, and the /dev/consdrain//dev/consfeed open gate then
// refuses it -- fail-closed, never a torn double-claim). Called from
// apply_spawn_perms (the child spawn thunk); the parent-side
// spawn_perm_grant_check already refuses the grant when a holder exists, so
// this CAS is the race-closer for two concurrent grants, not the common
// path. proc_become_zombie_locked clears the holder on its death (every
// death path), after which a fresh grant may claim.
int proc_set_console_renderer(struct Proc *p);

// proc_is_console_renderer — true iff `p` carries PROC_FLAG_CONSOLE_RENDERER
// AND is the current g_console_renderer (the live single holder). The
// /dev/consdrain + /dev/consfeed gate (devdev) and the cons drain/feed I/O
// re-gates call this. Never dereferences a stale pointer (compare-only under
// g_proc_table_lock). Fail-closed on NULL.
bool proc_is_console_renderer(const struct Proc *p);

// proc_test_console_renderer — test-only: read g_console_renderer.
struct Proc *proc_test_console_renderer(void);

// proc_test_clear_console_renderer — test-only: release the role + flag.
void proc_test_clear_console_renderer(void);

// proc_console_sak — the A-4c-2 SAK transition (I-27 trusted-path handoff). Run
// from the console_mgr kthread on a recognized serial BREAK. Under
// g_proc_table_lock (RW-7 R2-F1/F2 as-built): revoke the console-ATTACH bit from
// the current owner (NO note -- LS-5 made `interrupt` a terminate note, so the
// old courtesy post would KILL a non-self-managing owner), then grant the ATTACH
// to the trusted login authority and clear the OWNER to NULL. owner and attach
// are distinct roles: corvus is the elevation authority, NEVER the Ctrl-C target
// (the owner is re-established when login spawns the session shell). FAIL-SAFE:
// with no trusted Proc alive, no attach is granted -- no Proc can redeem
// CAP_HOSTOWNER / a clearance until a trusted login claims the console.
// Idempotent once the trusted Proc is the sole attach holder with no owner (a
// BREAK flood is a no-op).
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
