---
id: lock-dev9p-wb-priv
type: lock
title: "dev9p_priv.wb_lock — the write-behind run leaf"
kind: spin
orders-before: []
guards: "the per-open-file append-run state (wb_buf/off/len/cap/base/known/eligible/flushers/err)"
created: 2026-07-31
updated: 2026-07-31
---
A PURE LEAF: only byte copies and state reads happen under it — wire I/O
NEVER (blocking 9P under a spinlock is the #360 violation). The only
thing ever acquired below it is the buddy zone lock via kmalloc/kfree
(the established Larder-leaf → buddy order; both non-blocking). The
flush drops it across the wire Twrites and re-takes it to retire —
sound because `wb_flushers != 0` FREEZES the run (no stage, no growth
realloc), so the out-of-lock reads of the captured buffer cannot race a
free. The single-flight wait (`wb_flush_locked`'s yield loop) releases
it per iteration — the on_cpu-spin class, no Rendez, no I-9 leg.
Sleep-illegal under it.
