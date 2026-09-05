---
id: chg-2026-07-31-srv-sweep
type: chg
title: "The srv-area sweep: srvconn + devsrv dossiers + the P5/stalk-3/#348/CF-3B audit backfill"
date: 2026-07-31
arc: arc-vault
commits: ["b892047c"]
touched:
  - moc-kernel
  - moc-kernel-ninep
  - sub-kernel-ninep-transport
  - inv-i9
  - arc-identity-detour
  - arc-go-build
established:
  - moc-kernel-srv
  - sub-kernel-srvconn
  - sub-kernel-devsrv
  - lock-srvconn-chan-lock
  - lock-srv-registry-lock
  - inv-i1
  - spec-corvus
  - seam-srv-registry-lifecycle
  - seam-srv-9p-connect-unit
  - view-closed-sub-kernel-srvconn
  - view-closed-sub-kernel-devsrv
closed: []
opened: []
mirrors-checked: []
depth: rich
---
## What

Sweep batch 3 — the `/srv` service layer (`kernel/srvconn.c` +
`kernel/devsrv.c` + both headers, read in full per the standing sweep
bar). Present: a NEW area ([[moc-kernel-srv]]) with two dossiers —
[[sub-kernel-srvconn]] (the ring transport: the chan machinery, the
#354 role park, the three blocking producers / two blocking consumers,
the all-or-nothing frame send, teardown's dual-lock EOF latch + complete
wake set) and [[sub-kernel-devsrv]] (the registry state machine,
create=post / open=connect, accept + the 40-byte `srv_peer_info` read
with its poster gates and #844 hoist, the close discriminator's
client-only kernel_attached skip); two lock notes; [[inv-i1]]
(established — the per-territory-registry realization, territory's own
edge pending its sweep); [[spec-corvus]] (the connection-layer model +
the 8-buggy-cfg gate posture); two seams (the registry lifecycle debt
incl. #30 tombstone accumulation; the 9p-mode connect unit-test gap);
`inv-i9.guards` grown to srvconn. Record: [[arc-corvus-srv]]
(backfill-active) + six retro chgs with git-verified SHAs (the P5 birth,
stalk-3a/3b/3c, #348, CF-3 B) + six adt rounds + 25 fnd notes with
frozen prosecution chains — including the designed cross-chunk closures
([[fnd-348-r1-f1]] deferred at #348, closed by CF-3 B's role park;
[[fnd-stalk3a-r1-f4]] closed by stalk-3b's cap removal). The corvus-
userspace and spec findings of the P5 round stay in the adt body per the
no-fabricated-surfaces rule. `docs/reference/70-devsrv.md` +
`71-srvconn.md` STUBBED (absorbed).

## Why

The recorded batch-3 target. Both reference docs were materially STALE
against the tree (the embedded per-conn 9P client, `srv_conn_open_for_proc`,
the "no-writer-block design" caveat, the 24-byte `srv_peer_info` — all
retired or outgrown); the dossiers are written from the code, with the
test rosters verified against `kernel/test/test.c` (14 srvconn + 26
devsrv + 5 srv_client cases) and every SHA against `git log`.

## Verification

`quaestor lint --all` green through the fail-closed hook; views
re-rendered (dashboard, seams, invariants, audit-triggers, roadmap, the
two new closed-preamble views); three sabotage revert-probes (a dangling
edge, a stale generated view, a dropped dossier section) each failed as
designed and were restored clean.
