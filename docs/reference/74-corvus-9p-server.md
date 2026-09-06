# 74 — corvus /srv/corvus 9P2000.L server transport (P5-corvus-srv-impl-b3b) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-corvus-clean-absorb`).
The transport rewrite that made corvus a real 9P server. It redirects to the
daemon (the server side) and the kernel transport (the byte carrier):

- the **corvus 9P server** — the post-then-chroot ordering that survives because
  the listener is a capability not a name, the per-connection `Conn` arena, the
  verb accumulator (one frame per write, reset-on-overflow), `ctl` as a
  **message-oriented** file whose `Tread` ignores the client offset, the
  owner-gated session close with its zero-skipping connection id, the live-caps
  re-query and the accept-time fail-closed `t_srv_peer` read, and the
  full-table accept spin — all in the daemon dossier, whose private 9P codec
  (`p9.rs`) is the **original** the shared runtime codec was lifted from:

      vault/system/userspace/services/sub-corvus.md

- the **kernel-side SrvConn transport** that actually carries the bytes — the
  per-connection ring behind `SYS_SRV_CONNECT`, the handshake it drives, and its
  teardown:

      vault/system/kernel/srv/sub-kernel-srvconn.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- It is the most current of the corvus reference files, but its **test counts
  are point-in-time** (511/511 at the b3b close) — the daemon dossier states
  what the boot round-trip proves without freezing a number that moves.
- It describes `usr/corvus/src/p9.rs` as "the 9P codec corvus's side needs"
  without noting that it is the **ancestor**: the shared codec that `ptyfs`,
  `tapestryd` and `netd` link was lifted out of this module, which inverts the
  usual reading — a fix that reached the library did not necessarily come back
  here. The dossier records that lineage.
