// IRQ forwarding to KObj_IRQ blocker (P4-G).
//
// Per ARCHITECTURE.md §9.3 + ROADMAP §6.1. The hardware-IRQ → driver
// rendezvous: a userspace driver process holds a KObj_IRQ handle for a
// specific GIC INTID; when the IRQ fires, the kernel's irqfwd dispatch
// wakes a Rendez attached to the KObj_IRQ; the driver thread (kernel-
// internal at v1.0; userspace at P4-I+) returns from kobj_irq_wait.
//
// Edge-triggered: each IRQ fire increments a pending counter; a wait
// returns the count and zeroes it. Multiple IRQs collapsed into one
// wait is the documented contract — drivers handle this by checking
// virtqueue used-ring state (which already encodes "how many entries
// completed") rather than relying on a 1:1 IRQ → wait mapping.
//
// At v1.0 P4-G:
//   - Kernel-internal API only. Userspace driver bindings (handle
//     table integration for KOBJ_IRQ release) land at P4-I+ alongside
//     the first userspace driver.
//   - Single-waiter per KObj_IRQ (Rendez convention). Multi-waiter
//     queues land at Phase 5+ (poll/futex).
//   - One KObj_IRQ owns its INTID exclusively. Sharing one IRQ across
//     multiple subscribers would require a wakeup fan-out; v1.0 each
//     driver gets its own GIC line.
//
// Per ARCH §25.4 the irqfwd path is an audit-trigger surface: the
// closing audit (P4-Z) prosecutes IRQ ordering, ARCH §28 I-19 (note
// delivery causal order) — though notes is its own subsystem (Phase 5+),
// the IRQ → wake atomicity is the same wait/wake protocol pinned by
// scheduler.tla's NoMissedWakeup invariant.

#ifndef THYLACINE_IRQFWD_H
#define THYLACINE_IRQFWD_H

#include <thylacine/rendez.h>
#include <thylacine/types.h>

// KOBJ_IRQ_MAGIC — sentinel set at kobj_irq_create; checked at every
// public API entry. Sits at offset 0; SLUB freelist write on free
// clobbers it (use-after-free defense, mirrors burrow.c / spoor.c).
#define KOBJ_IRQ_MAGIC 0x4952510DBADC0DECULL

// Reserved test SGI for irqfwd. SGI 0 = IPI_RESCHED (P2-Cdc); SGI 1 =
// IPI_IRQFWD_TEST. The remaining SGIs (2..15) are available for future
// IPIs (e.g., a TLB shootdown IPI when SMP-aware ASID rollover lands).
#define IPI_IRQFWD_TEST  1u

struct KObj_IRQ {
    u64           magic;        // KOBJ_IRQ_MAGIC
    u32           intid;        // GIC INTID (SGI / PPI / SPI)
    int           ref;          // refcount (kobj_irq_create starts at 1)
    struct Rendez rendez;       // single-waiter; lock guards pending_count
    u32           pending_count; // collapsed-IRQ count since last wait
};

// One-time setup: pre-reserve INTIDs owned by kernel-internal callers
// (those that bypass kobj_irq_create and call gic_attach directly —
// timer at INTID 30, IPI_RESCHED at SGI 0). Without this reservation,
// a userspace caller with CAP_HW_CREATE could call kobj_irq_create on
// the same INTID and clobber the kernel's handler slot (R9 F142, P0).
//
// Must be called AFTER gic_init + timer_init (we reserve known
// kernel-claimed INTIDs after they're attached) and BEFORE any
// kobj_irq_create caller can race. boot_main wires it after virtio_init.
void irqfwd_init(void);

// Allocate a KObj_IRQ + register the GIC handler + enable the IRQ.
// Returns NULL on:
//   - SLUB / kmalloc OOM.
//   - gic_attach failure (handler slot already in use).
//   - gic_enable_irq failure.
// On success, the kernel will route every fire of `intid` to this
// KObj_IRQ. Caller owns the returned reference; balance with
// kobj_irq_unref / kobj_irq_destroy.
struct KObj_IRQ *kobj_irq_create(u32 intid);

// Refcount ops. Mirrors burrow_ref / burrow_unref.
void kobj_irq_ref(struct KObj_IRQ *k);

// Decrement ref. If zero: gic_disable_irq + gic_attach(intid, NULL,
// NULL) to unregister + kfree. After the unref that drops ref to 0,
// `k` is INVALID.
void kobj_irq_unref(struct KObj_IRQ *k);

// Convenience: drop the caller's reference + ensure the IRQ stops
// firing. Equivalent to kobj_irq_unref when the caller holds the only
// reference.
void kobj_irq_destroy(struct KObj_IRQ *k);

// Block until at least one IRQ has fired since the last successful
// wait. Returns the count (always >= 1 on return). The pending counter
// is atomically zeroed before return.
//
// NULL-safe: returns 0 immediately on NULL k.
//
// Spurious wake from a non-IRQ source is ABSENT — sleep's cond loop
// guarantees we only return when pending_count > 0.
u32 kobj_irq_wait(struct KObj_IRQ *k);

// Diagnostic: cumulative IRQ counter (every fire ever observed) +
// live KObj_IRQ count.
u64 kobj_irq_total_fires(void);
u64 kobj_irq_live_count(void);

// Diagnostic: is `intid` currently claimed (by a live KObj_IRQ OR by
// kernel-reserved pre-claim from irqfwd_init)? Used by
// driver-crash-recovery tests to verify the release path cleared the
// intid claim after a driver process exited. Returns false for
// out-of-range intid.
bool kobj_irq_intid_claimed(u32 intid);

#endif  // THYLACINE_IRQFWD_H
