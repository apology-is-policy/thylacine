---
id: seam-srv-9p-connect-unit
type: seam
title: "No kernel unit test for the 9p-mode open=connect (blocking Tversion/Tattach)"
status: open
surface: [sub-kernel-devsrv]
opened-by: chg-2026-06-03-stalk3b-open-connect
created: 2026-07-31
updated: 2026-07-31
---
## What is owed

`devsrv_open_connect`'s 9p-mode arm — the blocking
`srvconn_attach_dev9p_root` handshake (Tversion + Tattach against a live
poster) — has NO kernel unit test. Only the byte-mode arm
(`devsrv.open_connect_byte`) and the transport adapter
(`9p_srvconn_transport.*`) are unit-pinned; the full 9p-mode connect is
validated ONLY by the boot E2E (joey/login/legate → corvus + stratumd).
Inherent to the synchronous harness: the connect blocks on a handshake a
unit test cannot answer without a second thread acting as a 9P responder.

## What closes it

A loopback fake-server harness — a kthread (or the mq-transport pattern)
answering Tversion/Tattach on the server endpoint while the test drives
the connect. Same owed-harness family as [[seam-841-mi-harness]] (the
cross-Proc multi-in-flight deterministic tests); a shared responder
kthread likely serves both.

## Risk while open

A regression in the 9p-mode connect path surfaces as a boot-time E2E
failure (loud, but late and coarse-grained) rather than a named unit
failure; bisecting it costs a boot cycle per probe.
