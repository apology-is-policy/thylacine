---
id: chg-2026-10-04-tls-close-state
type: chg
title: "TLS completion survives a coalesced clean close"
date: 2026-10-04
arc: arc-halcyon-interaction
commits: []
touched: [sub-tls]
established: []
closed: []
opened: []
depth: skeletal
---
The AS-1 broad boot gate found a real TLS failure. Live CPU1 stress reproduced
server317 EOF/Io and client318 EOF/Io; an actual-driver fixture reproduced the
same failure by delivering Finished and close_notify together. Establishment
was latched only at WriteTraffic, but rustls can report PeerClosed first.
The shared role macro now latches rustls's authenticated protocol state after
each successful step. Early close still fails; no authentication policy changes.

Fresh CPU1 boot1830/1830, coalesced/bytewise/early-close controls, the existing
untrusted-certificate control and1000 live handshakes pass. Full matrix restart
is pending. Single-agent self-review in ASYNC-SERVICE-SELF-REVIEW; evidence
work/oct4-async-service/as-r3. No Main landing or async activation.
