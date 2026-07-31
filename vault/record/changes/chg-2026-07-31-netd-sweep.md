---
id: chg-2026-07-31-netd-sweep
type: chg
title: "The netd sweep: the first userspace area — nic + server dossiers + the net/weft audit backfill"
date: 2026-07-31
arc: arc-vault
commits: ["*(pending)*"]
touched:
  - inv-i9
  - spec-net-poll
  - arc-net
  - arc-go-build
established:
  - moc-userspace
  - moc-userspace-netd
  - sub-netd-nic
  - sub-netd-server
  - haz-driver-panic-dos
  - seam-220-netd-listener-poll
  - seam-netd-host-tests
  - seam-240-lo-redial
  - seam-242-selftest-nonfatal
  - arc-weft
  - view-closed-sub-netd-server
  - view-closed-sub-netd-nic
closed: []
opened: [seam-220-netd-listener-poll, seam-netd-host-tests, seam-240-lo-redial, seam-242-selftest-nonfatal]
mirrors-checked: []
depth: rich
---
## What

Sweep batch 4 — netd (`usr/netd`: main.rs 710 + server.rs 5878 +
ndb.rs 142, read in full per the standing sweep bar). Present: the
FIRST userspace area — [[moc-userspace]] (the plane spine) +
[[moc-userspace-netd]] with two dossiers: [[sub-netd-nic]] (the driver
probe gate, the phy tokens, DHCP bring-up + the poll_dhcp re-apply,
the serve-loop delivery choreography + the #221/#291 poll-cadence
bands, the 10-test selftest battery) and [[sub-netd-server]] (the
qid-encoded tree, the shared refcounted slot pool + the mint-gen
guard, the FIVE deferred-reply engines + the cs/dns deferred read with
their four-site cancel matrix, the #293 remove-not-abort connect
sweep, the net-8a dual-stack routing, the weft in-place drive, the
#52 nonblock arm, the parsers). [[haz-driver-panic-dos]] established
(the sole-owner panic = subsystem DoS frame; smoltcp's panicking
contracts as the sharp instances). inv-i9.guards + spec-net-poll.models
grew to the netd server half (the serve-loop-order I-9 analog). FOUR
seams minted: [[seam-220-netd-listener-poll]] (the promised #220
discharge from the batch-1 net-6b close), [[seam-netd-host-tests]],
[[seam-240-lo-redial]] + [[seam-242-selftest-nonfatal]] (the net-8d
deferred findings). Record: [[arc-weft]] (backfill-active) + 14 retro
chgs with git-verified SHAs (net-2 birth → net-8, #221, #293,
go-net-3c into arc-go-build, #52-nonblock, Weft-0/6b/6c-2/7) + 7 adts
(net-2d; net-3d r1 DIRTY → r2; net-4d r1 → precautionary r2; net-8d —
the arc EXIT round this sweep surfaced from memory; weft-7) + 20 fnds
with frozen prosecution chains — including [[fnd-294-r1-f3]], the
batch-1 promise discharged as a WITHDRAWN netd-surface note, and the
two net-8d deferred fnds routing to the seam inbox.
`docs/reference/121-netd.md` STUBBED (absorbed).

## Why

The recorded batch-4 target; netd owned the deferred #294-F3/#220
items from batch 1. Unlike batches 1–3, the reference doc was LARGELY
CURRENT (maintained through the go-arc era — nonblock/#293/net-8d all
present); the dossiers still come from the code, which surfaced what
the doc lacks: the full weft-drive mechanism (thin — one mention), the
#221-vs-#291 task-number split, and a now-stale bring-up DHCP comment
that poll_dhcp superseded (recorded as a dossier caveat). Test rosters
verified against main.rs (10 asserted selftests + 3 best-effort
probes); every SHA against git log; the #220/#240/#242/seam-56
behaviors re-verified live in current code rather than trusted from
the 40-day-old closed lists.

## Verification

`quaestor lint --all` green through the fail-closed hook; views
re-rendered (dashboard, seams, invariants, audit-triggers, roadmap +
the two new closed-preamble views); three sabotage revert-probes (a
dangling edge, a stale generated view, a dropped dossier section)
each failed as designed and were restored clean.
