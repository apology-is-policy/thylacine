---
id: seam-242-selftest-nonfatal
type: seam
title: "netd's boot selftest battery FAILs are logged, not fail-closed"
status: open
surface: [sub-netd-nic]
opened-by: adt-net8d-r1
tracker: "task #242"
created: 2026-07-31
updated: 2026-09-24
---
## Owed

`serve()` logs a selftest FAIL line (e.g. `resident_lo_selftest`
non-PASS) and proceeds to post `/srv/net` anyway — netd does not fail
closed at its own layer on a broken loopback/data-path proof
([[fnd-net8d-r1-f3]]).

## What closes it

Return `Err` from `serve` on a deterministic-selftest FAIL (the
`post_srv_net` fail-closed shape): the warden then logs gave-up and
the box boots without /net, instead of serving a stack whose own
proofs failed. The selftests are deterministic (no host coupling), so
this cannot introduce a flake gate.

## Risk while open

Ship risk is ~zero today: the net-echo/go-net boot gates exit non-zero
on a broken loopback path, so a regression cannot ship silently — the
seam is defense-in-depth at the right layer, not a live hole.

Narrowed 2026-09-24. `tools/test.sh` now fails the boot gate once netd is up
(`netd: up mac=`) if netd prints any FAIL line, never serves, or omits the
dial-verdict selftest by name. Until then no gate read these lines. Two
consequences follow:
- A deterministic FAIL in the two selftests that already end netd (resident lo,
  TCP retirement) booted green with no `/net`.
- The rest proceed after a FAIL, so the fail-closed change above is still owed.
  It no longer hides a failure from the gate.
