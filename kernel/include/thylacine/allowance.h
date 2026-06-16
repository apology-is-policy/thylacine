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
//   the mmio/irq/dma/pci      <-> conferred == allowance (the live gate set);
//   fields                       IMMUTABLE after confer, so the spec's
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
#define ALLOWANCE_PCI_MAX   8   // permitted PCI functions (packed bus,dev,fn)

// Pack a PCI (bus, dev, fn) triple into the u32 token the HW_RES_PCI axis
// stores + allowance_permits matches on. The three byte fields occupy disjoint
// bit ranges (bus 16..23, dev 8..15, fn 0..7), so the pack is injective over
// (u8,u8,u8) and a real bus/dev/fn (bus 0..255, dev 0..31, fn 0..7) round-trips
// with no aliasing. "A PCI device's allowance IS its claimed BARs"
// (docs/MENAGERIE.md section 4) -- this is the per-(bus,dev,fn) token.
#define PCI_BDF_PACK(bus, dev, fn) \
    (((u32)(u8)(bus) << 16) | ((u32)(u8)(dev) << 8) | (u32)(u8)(fn))

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

    // The permitted PCI functions, each a PCI_BDF_PACK(bus,dev,fn). A NARROWED
    // driver may SYS_PCI_CLAIM only a (bus,dev,fn) listed here -- the per-device
    // PCI axis ("a PCI device's allowance IS its claimed BARs", MENAGERIE.md
    // section 4). IMMUTABLE after confer, like the other axes.
    u32              pci[ALLOWANCE_PCI_MAX];
    u32              pci_count;

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
//   HW_RES_PCI:  a = PCI_BDF_PACK(b,d,f), b = 0 -> a in the pci set.
enum hw_res_kind { HW_RES_MMIO, HW_RES_IRQ, HW_RES_DMA, HW_RES_PCI };

// CreateBegin's gate check (specs/allowance.tla). True iff p may create the
// requested resource. A NULL allowance is BROAD -> true (subject to the
// caller's CAP_HW_CREATE + the kobj_*_create I-5 reservation). A non-NULL
// allowance permits only its conferred set AND only while NOT revoked. This
// is the lock-free fast-path read (the windows are immutable; revoked is read
// ACQUIRE). The authoritative re-check is allowance_handle_alloc at commit.
bool allowance_permits(struct Proc *p, enum hw_res_kind kind, u64 a, u64 b);

// True iff p carries a NARROWED allowance (p->allowance != NULL). The
// "drivers are leaves" gate (MENAGERIE.md section 13.2): rfork_internal denies
// a narrowed driver a child Proc, so no hw-capable grandchild can inherit a
// clone of the allowance that would survive the parent's per-Proc revoke +
// thread-group-scoped DeviceRemoved terminate (5e-4 F2). A broad Proc (the
// warden + the trusted servers, allowance == NULL) is unaffected. SYS_PCI_CLAIM
// no longer uses this -- it gates on the per-(bus,dev,fn) PCI axis (HW_RES_PCI)
// since build-arc step 6. The complement of "broad".
bool allowance_is_narrowed(struct Proc *p);

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
// Allowance, freeing any prior allowance (an rfork-inherited clone). Called in
// the child's spawn thunk BEFORE it enters EL0, so there is no concurrent
// reader (the proc_confer_allowance set-once contract). Returns 0 on success,
// -1 on OOM / bad descriptor (count over the cap). The warden ensures the
// conferred set is a subset of the bound node (ConferredWithinNode -- the
// warden's grant policy, MENAGERIE section 11); the kernel additionally gates
// it as a NARROWING vs the spawning Proc's own allowance (see
// allowance_confer_within_parent) so a narrowed driver cannot confer a wider
// reach on a child (I-2's hardware-axis analog).
int proc_confer_allowance(struct Proc *p,
                          const struct hw_window *mmio, u32 mmio_count,
                          const u32 *irq, u32 irq_count, u64 dma_max,
                          const u32 *pci, u32 pci_count);

// True iff `parent` may CONFER the described allowance -- i.e. the requested
// set is within the parent's OWN allowance, so the confer is a NARROWING and
// never a widening (I-2's hardware-axis analog; specs/allowance.tla, the
// monotonic-reduction property allowance_clone_into enforces for plain rfork).
// A BROAD parent (allowance == NULL) may confer anything (allowance_permits is
// true for every resource). A NARROWED parent may confer only a subset of its
// own: each requested MMIO window must lie within one parent window, each IRQ
// must be in the parent's set, each PCI (bus,dev,fn) must be in the parent's
// pci set, and dma_max must not exceed the parent's. An empty axis (count 0, a
// size-0 window, or dma_max 0) confers nothing and is trivially within. A
// REVOKED parent confers nothing (allowance_permits is false while revoked).
// The spawn path rejects the spawn with -1 on false.
bool allowance_confer_within_parent(struct Proc *parent,
                                    const struct hw_window *mmio, u32 mmio_count,
                                    const u32 *irq, u32 irq_count, u64 dma_max,
                                    const u32 *pci, u32 pci_count);

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

// Install a new allowance pointer on p under g_proc_table_lock, returning the
// displaced one for the caller to free OUTSIDE the lock (audit F1). Implemented
// in proc.c (g_proc_table_lock is file-static there). proc_confer_allowance uses
// it so a confer in the child's spawn thunk -- which runs AFTER the child is
// proc-tree-linked and thus reachable by a concurrent proc_group_terminate ->
// proc_revoke_allowance -- serializes its swap+free against that revoke's
// lock-of-the-old-allowance, closing the narrowed-parent-spawns-child UAF.
struct Allowance *proc_allowance_install_locked(struct Proc *p, struct Allowance *na);

#endif
