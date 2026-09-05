---
id: lin-9p-client
type: lin
title: "The 9P-client hardening saga (#841 → #90)"
surfaces: [sub-kernel-ninep-client]
members:
  - chg-2026-06-03-841-pipeline
  - fnd-841-r1-f1
  - fnd-841-r1-f2
  - fnd-841-r2-f6
  - chg-2026-06-04-845-tflush
  - fnd-845-r1-f1
  - chg-2026-06-24-349-flow-control
  - fnd-349-r1-f1
  - chg-2026-07-13-375-spill
  - fnd-375-r1-f1
  - fnd-375-r1-f2
  - chg-2026-07-13-5253-send-dispositions
  - fnd-5253-r1-f1
  - chg-2026-07-17-8c3-reader-role
  - fnd-8c3-r1-f1
  - fnd-8c3-r2-f1
  - chg-2026-07-19-90-death-block-through
  - fnd-90-r1-f1
created: 2026-07-31
updated: 2026-07-31
---
## The saga

The client shipped serial (a spinlock held across the blocking recv — the
"R15-c F230" regression), which desynced a shared stream under a stalled op
and busy-spun CPUs. **#841** restored the committed elected-reader
pipelining (Plan 9 `mountio`), converging over three prosecution rounds
(reply-buffer UAF; the reader-role stranded on a hand-off-target's death).
Its F2 — a dying owner leaking its tag slot — became **#845**
(Tflush-on-abandon: the abandoned tag reserved until its Rflush, the I-10
reuse guard). The on-device `go build` then proved a full c2s ring was being
treated as session death: **#349** made back-pressure flow-control
(spill-free at first; its round-1 P1 was a single-waiter rendez park — an
unprivileged panic on exactly the parallel-writer workload). **#375** found
the retry re-reading the shared `out_buf` after the park dropped `c->lock` —
the equal-length duplicate frame whose stray Rlerror poisoned a negative
dentry (the task-#50 ~10%-of-cold-builds cluster); the spill closed it, and
its two adjacent-scope P2s became **#52/#53** (never-sent tag reclaim;
flush-EAGAIN rollback; the `abandoned` bit reconciling rollback with the
#294 cancel-then-close). **8c-3 (#89)** released the reader role across a
debug stop — where the holotype refuted "delivery is whole-frame" (chunked
rings ⇒ mid-frame sleeps are real) and forced the frame-atomic recv; the
death twin of that mid-frame unwind, a pre-existing #811 latent, was
user-voted and landed as **#90** (block-through, `reader_frame.tla`
model-first).

## The standing lesson

Four, all load-bearing for any future change here:

1. **The shared client makes every send/recv error disposition a
   whole-system availability decision.** A congestion-class event (full
   ring, EAGAIN, a stop) must never latch the shared session; only a genuine
   transport break may. Three separate chunks (#349, #53, 8c-3) each
   re-learned this on a different path.
2. **`out_buf` is undefined across any `c->lock` drop** — retry from a
   private spill, never from the shared buffer.
3. **"Whole-frame delivery" is false.** Delivery is chunked; frame-atomicity
   of the reader is a designed property ([[haz-shared-stream-desync]]),
   never an assumption.
4. **Two prosecutors, different blind spots.** Every P1 in this lineage was
   caught by exactly one of the pair (the self-audit missed 349-R1-F1 and
   both 8c-3 P1s; the self-audit alone caught 349-SA-1) — the
   different-lineage reviewer discipline is not ceremony here.
