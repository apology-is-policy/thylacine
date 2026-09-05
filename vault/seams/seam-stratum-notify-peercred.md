---
id: seam-stratum-notify-peercred
type: seam
title: "The corvus notify socket is trusted by path, not by peer credentials"
status: open
surface: [sub-stratum-session, sub-stratum-server]
opened-by: chg-2026-08-02-stratum-sweep
tracker: ""
created: 2026-08-02
updated: 2026-08-02
---
## Owed

`SO_PEERCRED` gating on the notify socket, matching the discipline every
other stratumd socket already follows.

## The gap

Each per-user stratumd subscribes to corvus's notify socket and acts on the
frames it receives: a matching `SESSION_CLOSED` sets the daemon's stop flag,
which unmounts the filesystem and zeroes the per-dataset DEKs. That is a
consequential action driven entirely by bytes off a socket.

Every other socket in the daemon resolves its peer. The proxy checks its
*upstream* peer to stamp ownership, and — with `--coordinator-uid` — checks
its *downstream* peer specifically to defeat socket-bind impersonation,
where a local user pre-binds a fake socket at the configured path before the
real daemon starts. The notify consumer performs no such check. Its stated
reason is that corvus is the only process that creates that path.

The parser itself is properly defensive — every length field is bounds
checked, the user string is capped, embedded NUL and control bytes are
refused, malformed frames are `STM_EPROTOCOL`. The gap is not in the frames.
It is in who may send them.

## The asymmetry that makes it small

The action a forged frame buys is a **teardown**: unmount, zero the keys,
exit. That is denial of service against one user's session, not disclosure
— the forger gains no data and no key, and the DEKs are destroyed rather
than exposed. The failure direction is toward locked, which is the safe
direction for this particular surface.

On Thylacine there is a second structural barrier: reaching the socket at
all requires it to be nameable in the attacker's namespace, and `/srv` is
per-territory ([[inv-i1]]) — so an ordinary session cannot name the path
whose credentials would need checking.

## Risk while open

Low on Thylacine, higher on a host deployment with a global socket
namespace. The concrete danger is not today's exposure but tomorrow's
extension: v1.0 handles only `SESSION_CLOSED`, and the future kinds named in
the design (`USER_KEY_ROTATED`, `ADMIN_FORCE_EVICT`) are verbs where an
unauthenticated sender would buy considerably more than a teardown. The
credential check should land **before** the parser learns those verbs.
