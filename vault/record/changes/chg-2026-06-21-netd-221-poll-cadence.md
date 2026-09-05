---
id: chg-2026-06-21-netd-221-poll-cadence
type: chg
title: "netd #221 trim: honor poll_delay while a probe is pending (~6x loopback)"
date: 2026-06-21
arc: arc-net
commits: ["c1e49fb1"]
touched: [sub-netd-nic]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
The #290 bench correction showed ~95% of bulk loopback time was a
transport-independent readiness stall — and a 1 ms-deadline kernel-pump
probe proved the kernel side was NOT the cause. The cause was netd's
serve-loop poll floor: with a pending probe the loop forced a flat
50 ms re-poll, and on loopback the TCP window-update that unblocks a
parked bulk sender is `net.poll`-driven with no 9P frame to wake on.
The fix (netd-only, timing-only — it can only observe an edge SOONER,
never lose one; I-9/net_poll.tla unaffected): while a probe is pending,
clamp the poll delay to [1 ms, ACTIVE_POLL_MAX_MS=2 ms]; the idle
branch keeps the 50 ms floor. M2 byte-copy 2370 → ~14300 KiB/s; MW weft
2436 → ~14000–15000 KiB/s. This TRIMS the netd half of
[[seam-221-idle-pump-wake]] (the kernel wake-channel half stays open —
the seam note records the trim); the in-code constant carries the
sibling task number #291 with the idle-wakeup tradeoff + the
loopback-vs-NIC-aware v1.x refinement.
