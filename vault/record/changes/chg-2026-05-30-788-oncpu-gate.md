---
id: chg-2026-05-30-788-oncpu-gate
type: chg
title: "#788: thread_free gates on on_cpu — the SLEEPING-but-running UAF"
date: 2026-05-30
arc: arc-holotype-rw
commits: ["107186d7"]
touched: [sub-kernel-thread]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
no-dossier-change: "retro backfill -- sub-kernel-thread is established in this same sweep commit"
---
## What

`thread_free` waits for any in-flight `cpu_switch_context` AWAY from the
victim before reclaiming its descriptor and kstack, and counts how often it
had to (`g_thread_free_oncpu_waits`).

## Why

The state gates already proved the victim would not be PICKED again — not
RUNNING, removed from every run tree. But a thread that has just gone
SLEEPING or EXITING can still be `on_cpu`: its OWN `sched()` is in flight on
a peer CPU, physically executing on its kstack, about to perform the
register-SAVE half of the switch. `on_cpu` is cleared only by the
destination CPU's resume frame, AFTER that completes.

Freeing there returns the SLUB slot and the order-3 kstack to the
allocators while a peer is still writing them. Buddy LIFO hands the same
memory to the very next `thread_create`, and the stale register-save
corrupts the RECYCLED thread's `ctx.sp` — which then resumes onto a wild SP
and faults in its own guard page. The symptom reported as "kernel stack
overflow" was never honest stack depth.

The bug is SMP-only (0/20 at `-smp 1`, 2/20 at `-smp 4`) and rode a host
stall that widened the SLEEPING..on_cpu-cleared window past the caller's
drain. That is the whole reason `on_cpu` — not a run state — is the
canonical "stack and ctx still in use" signal.

## Verification

Retro record from `git log` + the surviving code comment, which is unusually
complete and carries the reproduction rates.
