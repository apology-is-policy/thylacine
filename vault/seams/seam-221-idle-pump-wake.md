---
id: seam-221-idle-pump-wake
type: seam
title: "dev9p.poll pump: 20 ms idle re-poll while a probe is parked"
status: open
surface: [sub-kernel-ninep-dev9p-poll]
opened-by: fnd-net6b-r1-f3
tracker: "task #221"
created: 2026-07-31
updated: 2026-07-31
---
**Owed**: a transport wake-on-write so the poll-pump kthread parks fully
while a readiness probe is outstanding, instead of re-polling the elected
reader at 50 Hz (the `DEV9P_POLL_IDLE_NS` 20 ms frame-boundary deadline).

**Why open is tolerable**: the deadline is LOAD-BEARING today — it is
what lets the kthread GC stranded ops and notice mask widens; removing it
without a wake channel wedges the kthread on a never-ready socket. The
netd half was trimmed 2026-06-21 (`c1e49fb1`: the serve loop honors
`poll_delay` while a probe is pending, ~6x loopback throughput) — the
kernel-side wake channel remains.

**What closes it**: a srvconn transport wake-on-write (bytes arriving on
s2c wake the parked pump) + dropping the periodic deadline to a pure GC
backstop; close via a chg that updates [[sub-kernel-ninep-dev9p-poll]].

**Risk while open**: idle-power/HVF-exit cost only; correctness is
unaffected.
