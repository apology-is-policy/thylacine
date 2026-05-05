// Process descriptor — Plan 9 Proc struct, Thylacine adaptation.
//
// Per ARCHITECTURE.md §7.2. A Proc owns an address space, namespace, fd
// table, handle table, credentials, threads, notes, parent/child links.
// At P2-A only the bare minimum lands: pid + threads list + thread_count,
// because that's all P2-A's bringup uses. Subsequent sub-chunks grow the
// struct as the corresponding subsystems land:
//
//   P2-B: state for scheduler bookkeeping (per-Proc statistics).
//   P2-C: namespace pointer (Pgrp).
//   P2-D: handle table head.
//   P2-E: address space (page table root, vma_tree).
//   P2-F: notes queue, parent/children, exit_status.
//   P2-G: credentials, capability bitmask.
//
// The struct grows by appending; existing field offsets stay stable so
// that incremental sub-chunks don't churn the SLUB cache size. New
// fields default to zero/NULL via KP_ZERO at allocation (P2-A audit
// R4 F40 close); explicit field setters cover only the non-zero-default
// values (e.g., `pid`).

#ifndef THYLACINE_PROC_H
#define THYLACINE_PROC_H

#include <thylacine/types.h>

struct Thread;

// PROC_MAGIC — sentinel set at proc_alloc / proc_init; checked at proc_free.
// Sits at offset 0 so SLUB's `*(void **)obj = freelist` write on
// kmem_cache_free naturally clobbers it; a subsequent proc_free reads the
// clobbered value, sees magic != PROC_MAGIC, and extincts with a clear
// double-free diagnostic (P2-A audit R4 F42 close).
#define PROC_MAGIC 0x50524F43C0DEFADEULL    // 'PROC' || 0xC0DE'FADE

struct Proc {
    u64            magic;            // PROC_MAGIC
    int            pid;
    int            thread_count;
    struct Thread *threads;          // doubly-linked list head (Thread.next_in_proc)
};

_Static_assert(sizeof(struct Proc) == 24,
               "struct Proc size pinned at 24 bytes (P2-A baseline). "
               "Adding a field grows the SLUB cache size; update this "
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
// pid), threads = NULL, thread_count = 0. Returns NULL on OOM.
struct Proc *proc_alloc(void);

// Release a Proc descriptor. Caller must ensure thread_count == 0
// (no live threads). Extincts on violation.
void proc_free(struct Proc *p);

// Diagnostic.
u64 proc_total_created(void);
u64 proc_total_destroyed(void);

#endif // THYLACINE_PROC_H
