---
id: moc-kernel-srv
type: moc
title: "Kernel /srv service layer"
parent: moc-kernel
created: 2026-07-31
updated: 2026-07-31
---
The `/srv` service layer — Plan 9's `#s` heritage: the kernel surface by
which a userspace server publishes a name (create=post) and the kernel
mediates each client connection (open=connect), stamping it with the
client's kernel-attested identity. Every kernel-attached FS mount, the
corvus key-agent channel, netd's `/net` posting, ptyfs, and tapestryd all
stand on this layer; the 9P stack ([[moc-kernel-ninep]]) rides it via the
srvconn transport backend ([[sub-kernel-ninep-transport]]).

## Children

- [[sub-kernel-srvconn]] — the per-connection byte transport (the
  `SrvConn` rings, blocking flow control, the role park, teardown).
- [[sub-kernel-devsrv]] — the service registry + the `/srv` Dev + the
  accept/peer syscall layer (the policy half; owns the namespace-resident
  per-territory registry).

## Cross-cutting

- Invariants: [[inv-i1]] (per-territory isolation — the registry is
  reached only through the mounted `/srv` root) · [[inv-i9]] (every
  blocking transport path is register-then-observe).
- Spec: [[spec-corvus]] — the connection identity/lifecycle model above
  the bytes.
- Hazards: [[haz-single-waiter-rendez]] · [[haz-death-path-wake]].
- Consumers: the kernel 9P client over the srvconn backend
  ([[sub-kernel-ninep-attach]] drives the handshake); corvus / stratumd /
  netd / ptyfs / tapestryd as posters; pouch AF_UNIX sockets as the
  byte-mode POSIX face.
