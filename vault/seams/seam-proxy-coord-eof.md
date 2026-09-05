---
id: seam-proxy-coord-eof
type: seam
title: "A coordinator that closes mid-conversation drops the proxy's client without a protocol error"
status: open
surface: [sub-stratum-session]
opened-by: chg-2026-08-02-stratum-sweep
tracker: ""
created: 2026-08-02
updated: 2026-08-02
---
## Owed

Emit a synthetic `Rlerror(EIO)` upstream when the coordinator closes with a
request outstanding, so the kernel client sees a protocol-level failure
rather than a transport one.

## The gap

`stm_proxy_9p_serve_client` forwards one frame and reads one reply. If the
coordinator closes the connection *after* the request was forwarded but
before the reply arrives, the proxy has an upstream client blocked on a
tag it can never answer. v1.0 breaks the loop and closes upstream — so the
kernel sees a dropped connection.

The forward note is in the source and the shape is right; it is unbuilt.

## Why it is small today

The coordinator is the boot stratumd and it does not close connections
under a live session. Reaching this needs the coordinator to die or wedge
mid-request, which is already a whole-system failure by other means.

And the kernel client handles the transport drop correctly: session death
completes every outstanding op with an error, so nothing hangs. The
difference is diagnostic quality, not liveness — an EIO on one op names the
op, a session death does not.

## Risk while open

Low. A coordinator death mid-request surfaces to the guest as a whole-mount
failure rather than a per-operation error, which is a worse first
impression of the same underlying fact.
