// Process descriptor — Plan 9 Proc struct, Thylacine adaptation.
//
// Per ARCHITECTURE.md §7.2 + §7.4 + §7.9. A Proc owns an address space,
// namespace, fd table, handle table, credentials, threads, notes,
// parent/child links. The struct grows by appending; existing field
// offsets stay stable so incremental sub-chunks don't churn the SLUB
// cache size. New fields default to zero/NULL via KP_ZERO at allocation
// (P2-A audit R4 F40 close).
//
// History:
//   P2-A:  pid + threads list + thread_count.
//   P2-D:  + state (ALIVE / ZOMBIE) + parent + children + sibling
//          + exit_status + exit_msg + child_done Rendez.
//   P2-E:  + namespace pointer (Pgrp).
//   P2-F:  + handle table head.
//   P2-G:  + address space (page table root, vma_tree) + credentials
//          + capability bitmask + notes queue.

#ifndef THYLACINE_PROC_H
#define THYLACINE_PROC_H

#include <thylacine/rendez.h>
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

struct Pgrp;
struct HandleTable;

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

    // P2-Eb: namespace (Plan 9 Pgrp). At v1.0 each Proc has its own
    // private Pgrp (rfork(RFPROC) calls pgrp_clone). Phase 5+ adds
    // RFNAMEG: shared namespace via refcount sharing.
    struct Pgrp      *pgrp;

    // P2-Fc: handle table. Per-Proc fixed-size at v1.0 (PROC_HANDLE_MAX
    // slots; see <thylacine/handle.h>). proc_alloc allocates a fresh
    // empty table; proc_free closes any open handles + releases the
    // table. RFFDG (rfork shared fd table — Plan 9 idiom; v1.0 P2-Fc
    // is one of the slots in a future shared-handle-table design) is
    // forward-looking for the Phase 5+ syscall surface.
    struct HandleTable *handles;
};

_Static_assert(sizeof(struct Proc) == 96,
               "struct Proc size pinned at 96 bytes (P2-Fc: P2-Eb baseline 88 "
               "+ handles 8). Adding a field grows the SLUB cache; update this "
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
// extinction. Subsequent sub-chunks add RFNAMEG (P2-E namespace),
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

// rfork flags. Per ARCH §7.4. Only RFPROC implemented at P2-D.
#define RFPROC      0x0001    // create a new Proc (always required)
#define RFMEM       0x0002    // share address space (future P2-G)
#define RFNAMEG     0x0004    // share namespace (future P2-E)
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

#endif // THYLACINE_PROC_H
