---
id: fnd-16c-r2-f1
type: fnd
title: "R1's two deadline fixes interact: a stale lapsed deadline wedges post-attach ops"
round: adt-16c-r2
severity: P1
status: fixed
surface: [sub-kernel-ninep-transport]
threatens: []
fixed-by: chg-2026-05-26-16c-attach-srv
created: 2026-07-31
---
## Prosecution

F1's HANDSHAKE_DEADLINE lingers after the handshake; F2's auto-arm fires
only on deadline == 0, never on stale. 5 s after SYS_ATTACH_9P_SRV every
op sees a lapsed deadline and TIMEDOUTs instantly on a healthy peer.
Dormant at v1.0 (joey's bringup fits the window); a real liveness
regression for any longer-lived FS consumer.

## Disposition

Fixed at the round (auto-arm gate widened to `deadline == 0 OR now >=
deadline`); both prosecutor and self-audit caught it independently.
**Superseded with its parent**: #841 removed the auto-arm entirely and
made the handshake-then-clear split explicit (`srvconn_attach_dev9p_root`
clears the deadline to 0 after the handshake -- the exact residue this
finding demanded, achieved by deletion rather than refinement).
