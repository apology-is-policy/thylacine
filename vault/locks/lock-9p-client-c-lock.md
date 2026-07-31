---
id: lock-9p-client-c-lock
type: lock
title: "c->lock (per-p9_client spinlock)"
kind: spin
guards: "The whole shared-client state: the tag-indexed inflight[] rpc table + the session's tag/fid/outstanding tables, out_buf staging, the reader election (reader_active, be_reader hand-off), the send-flow state (send_progress, send_waiters + list registration), done_reply_buf, and the c->dead latch."
orders-before: []
created: 2026-07-31
updated: 2026-07-31
---
## Discipline

- **NEVER held across the blocking recv** — the #841 restoration's core
  rule; the serial regression it retired held this spinlock across the
  sleep. Every sleeper drops it first: the elected reader before
  `reader_recv_frame`, a waiter before sleeping on its own rpc rendez, a
  back-pressured sender before its park.
- **`out_buf` is UNDEFINED across any drop** (the #375 spill contract): a
  peer may legally rebuild it the moment the lock releases. Any path that
  must retry a send after a drop retries from a private spill copy; the sole
  post-drop `out_buf` re-read anywhere is the NOTAG handshake, sound only
  because that client is still private (`p9_attached_create`).
- **Order** (the poll object→list→timerwait→rendez chain, acyclic):
  `c->lock` → `send_waiters_list.lock` → `g_timerwait.lock` → per-waiter
  rendez lock. Peer lock notes land at the registry pass; until then this
  prose is the order's home for the client surface.
- `kmalloc`/`kfree` under it are legal (non-blocking slub/alloc_pages path —
  the `rpc.reply_buf` precedent; the spill buffer relies on this).
- Acquisition contexts: every public op, the demux, the completion seam
  (`on_complete` runs under it — no sleep, no re-entry), destroy.
