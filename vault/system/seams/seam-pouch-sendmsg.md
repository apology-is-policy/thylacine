---
id: seam-pouch-sendmsg
type: seam
title: "`sendmsg`/`recvmsg`/`socketpair` stay ENOSYS"
status: open
surface: [sub-pouch-net]
opened-by: chg-2026-06-18-net6a2-datacalls
tracker: "#214"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

The scatter-gather + ancillary-data (cmsg) surface has no pouch arm: a
tagged fd falls through to the seam sentinel and answers `ENOSYS`
(fail-closed -- the tag never reaches a kernel syscall). `socketpair`
likewise.

Deliberately not half-built: a single-iovec, no-cmsg `sendmsg` would be
the very half-feature the net-5 disposition warned against, and no v1.0
in-VM consumer needs one.

## The lift

`sendmsg`/`recvmsg` are a straightforward loop over the iovec array onto
the same data fd once someone needs them; cmsg (fd passing) is a real
design question on a system where fd transfer is 9P's job (I-4), not a
socket's. `socketpair` wants a kernel primitive or a synthetic
`/srv`-less byte pair.
