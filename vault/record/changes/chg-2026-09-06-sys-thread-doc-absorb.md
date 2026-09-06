---
id: chg-2026-09-06-sys-thread-doc-absorb
type: chg
title: "absorb docs/reference/81-sys-thread (SYS_THREAD_SPAWN/EXIT, pthread substrate): fold the clear_child_tid join handshake into sub-kernel-death, 7-surface redirect stub"
date: 2026-09-06
arc: arc-vault
commits: ["PENDING"]
touched: [sub-kernel-death]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-06
---
The kernel pthread substrate reference (303 lines) -- an audit-trigger surface
(SYS_THREAD_SPAWN=41/EXIT=42/SET_TID_ADDRESS=36). Verified atom-by-atom across
seven owners; one genuine fold, code-grounded.

HOMES: thread_create_user (peer-Thread creation, own kstack, no-validate,
atomic-tid) -> sub-kernel-thread; thread_user_trampoline (context.S, #713 mask) ->
sub-kernel-sched-smp; thread_exit_self + SYS_EXIT_GROUP + the pthread-join handoff
-> sub-kernel-death; the futex the join parks on (torpor_wait/wake, SYS_TORPOR_WAKE
=40) -> sub-kernel-torpor; uaccess_store_u32 (store-a-word + fixup) ->
sub-kernel-uaccess; the 41/42/36 numbers+handlers -> sub-kernel-syscall-abi+dispatch.

FOLD (genuine gap): the kernel-side clear_child_tid JOIN HANDSHAKE had NO kernel
dossier home -- only the pouch userspace side (sub-pouch-thread) documented it.
thread_clear_child_tid_handoff (proc.c:2587): a peer Thread's exit atomically
zeroes its registered clear_child_tid word (set via SYS_SET_TID_ADDRESS) and
torpor_wakes UINT32_MAX joiners on that VA = the KERNEL HALF of pthread_join, fires
for EVERY exiting Thread (not just the last); an unmapped/unwritable tidptr SKIPS
silently (via the uaccess_store_u32 fixup) rather than extincting. Folded into
sub-kernel-death (which owns thread_exit_self + the torpor-wake). Code-confirmed
(proc.c:2578-2588 + uaccess.S:193-210).

WHAT THE DOC GOT WRONG: the kernel-side clear_child_tid handshake lacked a kernel
home (now folded); the pouch mutex/condvar/rwlock layer (9b) it points forward to
is built; content distributed across 7 dossiers.

Render clean; lint 0-fail. view-absorption 81 -> 82.
