---
id: fnd-thread-11c-r1-f2
type: fnd
round: adt-thread-11c-r1
severity: P3
status: fixed
title: "A server Rlerror(EINTR) that beat the flush re-issued the call"
surface: [sub-kernel-ninep-client, sub-kernel-notes]
threatens: [inv-i19]
fixed-by: chg-2026-11-03-item-11c
regression: "9p_client.note_flush_server_eintr_stands; K54 and K55 red on it"
created: 2026-11-03
---
## Prosecution

`note_call_unwound` is set at the unwind, before the client knows whether the Rflush or the reply wins. A server's own EINTR therefore read as a wait that unwound and restarted, though the server had answered. `client_run` now clears the mark when the flush wait ends with the original reply, so the mark means the wait unwound and nothing happened.
