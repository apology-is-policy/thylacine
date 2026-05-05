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
// that incremental sub-chunks don't churn the SLUB cache size.

#ifndef THYLACINE_PROC_H
#define THYLACINE_PROC_H

#include <thylacine/types.h>

struct Thread;

struct Proc {
    int            pid;
    int            thread_count;
    struct Thread *threads;          // doubly-linked list head (Thread.next_in_proc)
};

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
