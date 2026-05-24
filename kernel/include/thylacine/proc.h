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

    // P5-corvus-srv-impl-b2: count of LIVE `/srv` *client* connection
    // handles owned by this Proc (KObj_Srv handles whose obj is a
    // SrvConn). At v1.0 the per-Proc cap is 1 (CORVUS-DESIGN.md §6.2 —
    // "One connection per Proc"); srv_conn_open_for_proc rejects when
    // this is already nonzero. Incremented at handle install, decremented
    // at handle_close (the KObj_Srv SRV_CONN_MAGIC arm). At v1.0 single-
    // thread-per-Proc makes non-atomic r-m-w safe; a future multi-thread-
    // per-Proc lift needs `__atomic_compare_exchange` on this field.
    //
    // NOT propagated by rfork (it counts live handles, not a transferable
    // property — and KObj_Srv connection handles are non-transferable,
    // pinned to the origin Proc).
    u8                 srv_conn_count;
    u8                 _pad_srv[3];       // explicit padding to 8-byte align

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

    // P6-pouch-mem: per-Proc lock serializing VMA-list mutation through
    // the SYS_BURROW_ATTACH / SYS_BURROW_DETACH syscalls (the find-gap +
    // vma_insert / vma_remove sequences). At v1.0 Procs are single-
    // threaded, so it is uncontended by construction; it is in place so
    // the burrow-attach surface is correct when the pouch-threads sub-
    // chunk makes Procs multi-threaded — that sub-chunk extends the
    // lock's coverage to the remaining VMA mutators (exec_setup's
    // burrow_map calls, vma_drain) and the page-fault vma_lookup reader.
    // KP_ZERO at proc_alloc / proc_init zero-inits it to the valid
    // unlocked state (SPIN_LOCK_INIT == (spin_lock_t){ 0 }).
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
};

#define PROC_FLAG_NODUMP            (1u << 0)
#define PROC_FLAG_NOTRACE           (1u << 1)
#define PROC_FLAG_MLOCKED           (1u << 2)
#define PROC_FLAG_CONSOLE_ATTACHED  (1u << 3)
#define PROC_FLAG_MAY_POST_SERVICE  (1u << 4)

_Static_assert(sizeof(struct Proc) == 168,
               "struct Proc size pinned at 168 bytes (sub-chunk 12 baseline "
               "152 + the P6-pouch-signals notes pointer 8 + handler_va 8 = "
               "168). Adding a field grows the SLUB cache; update this "
               "assert deliberately so the change is intentional.");
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

// P2-D: wait_pid — reap a zombie child.
//
// Blocks until any child enters ZOMBIE state. On wake, picks the oldest
// zombie child (head of children list with state == ZOMBIE), unlinks
// it from the parent's children list, copies exit_status to *status_out
// (if non-NULL), and calls thread_free + proc_free to release the
// child's resources.
//
// Returns the reaped child's PID on success. Returns -1 (with errno-
// equivalent semantics deferred to Phase 5+ syscall surface) if there
// are no children at all (live or zombie) — never blocks indefinitely
// when nothing can wake us.
//
// At v1.0 P2-D, this is a kernel-internal primitive; callers are kernel
// test code that rfork'd children and want to reap them. The full
// wait(*WaitMsg) syscall surface (per ARCH §7.9) lands at Phase 5 with
// the syscall layer.
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
// One-way: idempotent, never cleared. Maps to specs/corvus.tla's
// MarkConsoleAttached action. v1.0 caller: joey (the console-login
// chain root) marks itself at boot; P5-login's login process will mark
// the per-user shells it spawns. The bit is NEVER conferred by rfork —
// every console-attached Proc is the result of an explicit call here.
// Extincts on a NULL or corrupted Proc.
void proc_mark_console_attached(struct Proc *p);

// proc_is_console_attached — true iff `p` carries
// PROC_FLAG_CONSOLE_ATTACHED. Returns false for a NULL `p`.
bool proc_is_console_attached(const struct Proc *p);

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

#endif // THYLACINE_PROC_H
