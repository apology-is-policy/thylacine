// Per-Proc hardware allowance -- the Menagerie driver-authority lift
// (ARCH section 28 invariant I-34; docs/MENAGERIE.md section 4;
// specs/allowance.tla). Scopes the coarse CAP_HW_CREATE to a bounded
// resource set: a driver Proc conferred a NARROWED allowance may create
// KObj_MMIO / KObj_IRQ / KObj_DMA handles ONLY within it. The I-25 (legate
// scope) analog for hardware -- a driver's hardware authority is exactly
// its warden-granted allowance, a subset of its bound node's resources,
// never widened, fully revoked on unbind / removal / crash.
//
// Broad vs narrowed (the backward-compat hinge):
//   p->allowance == NULL  -> BROAD. The warden + the existing trusted system
//     servers (kproc / joey / stratumd / netd / the boot-grant CAP_HW_CREATE
//     holders), bounded only by the I-5 kernel reservation -- the as-built
//     v1.0 behavior, unchanged. "Everything not kernel-reserved" (MENAGERIE
//     section 4) is exactly this: the existing kobj_*_create reservation
//     checks already exclude the I-5-reserved ranges.
//   p->allowance != NULL  -> NARROWED. The driver may create only within the
//     conferred set, and only while NOT revoked.
// The allowance is the OPTIONAL narrowing the warden confers when it spawns a
// per-device driver; it never grants -- the coarse CAP_HW_CREATE gate still
// runs first (a Proc with no cap creates nothing regardless of allowance).
//
// Spec mapping (specs/allowance.tla -- the KERNEL mechanism; the warden is
// the implicit privileged actor that drives Confer/Revoke):
//   the mmio/irq/dma fields  <-> conferred == allowance (the live gate set);
//                                IMMUTABLE after confer, so the spec's
//                                AllowanceWithinConferred (never widened)
//                                holds structurally.
//   revoked                   <-> allowance[d] = {} on Revoke.
//   allowance_permits         <-> CreateBegin's gate check (resource in set).
//   allowance_handle_alloc    <-> CreateCommit: re-read revoked UNDER the lock
//                                proc_revoke_allowance takes, then install --
//                                so an in-flight create racing a DeviceRemoved
//                                revocation aborts (BUGGY_COMMIT_NO_RECHECK).

#ifndef THYLACINE_ALLOWANCE_H
#define THYLACINE_ALLOWANCE_H

#include <thylacine/types.h>
#include <thylacine/spinlock.h>
#include <thylacine/handle.h>   // enum kobj_kind, rights_t, hidx_t

struct Proc;

#define ALLOWANCE_MMIO_MAX  8   // permitted MMIO PA windows
#define ALLOWANCE_IRQ_MAX   8   // permitted IRQ INTIDs (SPIs >= 32)

// A permitted MMIO PA window: [base, base + size).
struct hw_window { u64 base; u64 size; };

struct Allowance {
    // The conferred resource set -- IMMUTABLE after proc_confer_allowance
    // (only `revoked` ever flips). No path widens these post-confer, so the
    // spec's AllowanceWithinConferred holds by construction.
    struct hw_window mmio[ALLOWANCE_MMIO_MAX];
    u32              mmio_count;
    u32              irq[ALLOWANCE_IRQ_MAX];
    u32              irq_count;
    u64              dma_max;       // max bytes per KObj_DMA; 0 = no DMA permitted

    // Set on DeviceRemoved (proc_revoke_allowance) -- the spec's Revoke
    // emptying allowance[d]. Once set, the allowance permits nothing. Written
    // __ATOMIC_RELEASE under `lock`; read __ATOMIC_ACQUIRE in the lock-free
    // CreateBegin fast path AND under `lock` at the CreateCommit re-check.
    // u32 (0/1) for the established __atomic flag idiom (cf. proc_flags).
    u32              revoked;

    // Serializes the create gate's commit re-check (allowance_handle_alloc)
    // against proc_revoke_allowance. Lock order: allowance->lock -> handle-
    // table lock (the commit holds it across handle_alloc, which is spinlock-
    // only, never sleeps). proc_revoke_allowance takes it ALONE, releasing
    // BEFORE the caller's proc_group_terminate -- no nesting with
    // g_proc_table_lock. Nothing acquires the handle-table lock then this, so
    // the order is acyclic.
    spin_lock_t      lock;
};

// Hardware resource kinds, for allowance_permits. The (a, b) pair carries:
//   HW_RES_MMIO: a = PA base, b = byte size  -> [a, a+b) within one window.
//   HW_RES_IRQ:  a = INTID,   b = 0          -> a in the irq set.
//   HW_RES_DMA:  a = byte size, b = 0        -> a in (0, dma_max].
enum hw_res_kind { HW_RES_MMIO, HW_RES_IRQ, HW_RES_DMA };

// CreateBegin's gate check (specs/allowance.tla). True iff p may create the
// requested resource. A NULL allowance is BROAD -> true (subject to the
// caller's CAP_HW_CREATE + the kobj_*_create I-5 reservation). A non-NULL
// allowance permits only its conferred set AND only while NOT revoked. This
// is the lock-free fast-path read (the windows are immutable; revoked is read
// ACQUIRE). The authoritative re-check is allowance_handle_alloc at commit.
bool allowance_permits(struct Proc *p, enum hw_res_kind kind, u64 a, u64 b);

// CreateCommit (specs/allowance.tla): install a hw handle, re-checking the
// allowance UNDER its lock so a concurrent proc_revoke_allowance is observed.
// A NULL allowance bypasses the re-check (broad) and calls handle_alloc
// directly. A non-NULL, non-revoked allowance installs under the lock; a
// revoked allowance returns -1 (the in-flight create lost the race -> abort).
// The caller rolls back the kobj on -1 exactly as for a handle_alloc failure.
hidx_t allowance_handle_alloc(struct Proc *p, enum kobj_kind kind,
                              rights_t rights, void *obj);

// Confer a narrowed allowance on a freshly-spawned driver Proc (the warden
// path; specs/allowance.tla Confer). Deep-copies the descriptor into a heap
// Allowance. The caller guarantees a fresh child (p->allowance == NULL).
// Returns 0 on success, -1 on OOM / bad descriptor (count over the cap).
// The warden ensures the conferred set is a subset of the bound node
// (ConferredWithinNode -- the warden's grant policy, MENAGERIE section 11).
int proc_confer_allowance(struct Proc *p,
                          const struct hw_window *mmio, u32 mmio_count,
                          const u32 *irq, u32 irq_count, u64 dma_max);

// Revoke a driver's allowance on DeviceRemoved (specs/allowance.tla Revoke).
// Sets revoked under the lock -- closing the gate for in-flight AND future
// creates. The CALLER then proc_group_terminate's the driver; the #809/#811
// cascade drops the live handles at reap (the handle-axis teardown). A NULL
// allowance is a no-op.
void proc_revoke_allowance(struct Proc *p);

// Deep-copy a parent's allowance into a forked child (rfork inherit). A
// narrowed parent's child is equally narrowed -- the hardware-axis analog of
// caps' monotonic reduction (I-2): a child can never reach a BROADER hardware
// authority than its parent. A broad parent (allowance == NULL) -> child
// stays NULL. The revoked flag is copied, so a child forked AFTER its
// parent's revocation is born revoked (permits nothing). Returns 0 on success
// (incl. the NULL-parent no-op), -1 on OOM.
int allowance_clone_into(struct Proc *child, struct Proc *parent);

// Free a Proc's allowance at reap (proc_free). NULL-tolerant.
void allowance_free(struct Proc *p);

#endif
