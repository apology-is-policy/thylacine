---
id: dec-2026-10-07-tag-pool
type: dec
title: "The 9P tag pool grows to 0xFFFE, keeps an op share with flush headroom and an async share, waits instead of failing, and the reader applies every reply"
date: 2026-10-07
status: standing
decided-by: user-vote
affects: [sub-kernel-ninep-client, sub-kernel-ninep-session, sub-kernel-ninep-dev9p, sub-kernel-ninep-dev9p-poll, sub-kernel-loom, inv-i10, spec-9p-client]
created: 2026-10-07
---
## Fork

Each 9P session had 64 tags. A sync op that found all 64 held failed
`-P9_E_IO` at its build (only a clunk drained for a tag), against ARCH 21.5's
"new requests block until a slot frees". Nothing above the client retries, so a
write-behind flush that met a full pool dropped its data and no close reported
it. A burst of 64 or more async clunks leaves the pool full of undrained
Rclunks on a plain mount; a witness run (2026-10-06, while researching
[[seam-close-flush-unbounded]]) filled the pool and saw a sync walk fail. Ten
kinds of holder keep a tag, and four can keep it without bound: an async op
the server defers, a dev9p poll arm, an abandon that could not send its Tflush
(a full pool makes these, so the state feeds itself), and a reply stored for a
waiter that is stopped.

## Research

- **9front (`port/devmnt.c`).** One kernel-wide 16-bit tag bitmap;
  `alloctag` panics "no friggin tags left" when it runs out; `mntralloc`
  never waits (cached `Mntrpc`s keep their tags).
- **Linux (`net/9p/client.c`).** Per-client `idr_alloc(0, P9_NOTAG)`,
  `GFP_NOWAIT`: tags 0..0xFFFE, failure is an error, never a wait;
  back-pressure lives in the transport (virtio ring space,
  `io_wait_event_killable`), and the RPC wait is killable.
- **Zircon and Mach.** No small pool: FIDL transaction ids are 32 bits, and
  Mach gives each RPC its own reply port.
- **The tree.** `alloc_tag` scans 64 slots; `has_free_tag` has three callers
  (the clunk drain, the flush wait, the async submit's `-P9_E_AGAIN`); about
  22 sync wrappers fail `-P9_E_IO` on a full pool. The demux already applies a
  reply at once for an async op and for a reply honoured under flush(5)
  (`client_honour_locked`). Every 9P server in the tree treats a tag as an
  opaque 16-bit value (`usr/lib/ninep`, netd, ptyfs, tapestryd, Stratum's lp9
  and fs_pool).

## Options

1. **A, grow** the table to 0xFFFE (Linux's range).
2. **B, wait**: a sync op that finds no tag waits, killably, as the clunk
   does.
3. **C, shares**: cap the ops no thread waits on (Loom ops, poll arms) and
   keep headroom so a Tflush always finds a tag.
4. **D, the reader applies every reply**, so a tag is free when its reply is
   read and no tag waits on a stopped thread.

Offered as: all four as one design; A alone (Linux's answer, exhaustion stays
an error); or C+B+D at 64 tags.

## The call

All four as one (operator, 2026-10-07, AskUserQuestion). ARCH 21.11 states
the design: tags 0..0xFFFE in 64-entry chunks; an op share of 32767
(`P9_OPS_MAX`), a Tflush on any free tag, so a Tflush always finds one; an
async share of 16384 (`P9_ASYNC_MAX`) within the op share; a sync op waits
for a tag; the reader applies every reply.

## Rationale

A alone moves the cliff, it does not remove it: a deferred Loom op or poll arm
can still take every tag, the abandoned-without-Tflush state still feeds
itself, and a write-behind flush that meets a full pool still loses data.
B alone could wait forever behind those holders. C bounds what nothing waits
on, D removes the one holder a stop can freeze, and with both the wait B adds
ends when the server answers ops it has received. `specs/tag_pool.tla` checks
that (`SyncProgress`, `FlushAlwaysFits`), and each of its buggy cfgs drops one
rule and fails. The size is Linux's; the shares are new, because neither
heritage client carries ops that no thread waits on beside ops that threads
wait on.
