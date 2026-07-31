---
id: seam-220-netd-listener-poll
type: seam
title: "netd check_ready: a poll on a listener never reports accept-readiness"
status: open
surface: [sub-netd-server]
opened-by: chg-2026-06-18-net6b-poll-bridge
tracker: "task #220"
created: 2026-07-31
updated: 2026-07-31
---
## Owed

`check_ready`'s POLLIN arm reports `slot_poll_readable` (buffered data,
or a finished recv side), which is FALSE for a socket in LISTEN — so a
`poll(listener_fd, POLLIN)` (the classic select()-server multiplex
pattern) parks forever even while an inbound call is pending. The
working path is the blocking accept via `open(listen)` (the net-3a
deferred reply), which every v1.0 consumer uses.

## What closes it

`check_ready` (or `slot_poll_readable`) reports `accept_ready` for an
ANNOUNCED TCP slot — i.e. POLLIN on a listener means "a call has
landed" — proven via a loopback E2E leg (the disposition sketch from
the net-6b-4 close). Server-side only; no kernel change.

## Risk while open

A poll()-multiplexing server over /net cannot wait on its listener
(it must dedicate a blocked open(listen) instead). No v1.0 in-VM
consumer polls a listener; correctness of the blocking path is
unaffected.
