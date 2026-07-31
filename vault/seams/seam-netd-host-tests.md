---
id: seam-netd-host-tests
type: seam
title: "netd has no host-test module — the in-guest selftests are the rigor floor"
status: open
surface: [sub-netd-server, sub-netd-nic]
opened-by: adt-net2d-r1
created: 2026-07-31
updated: 2026-07-31
---
## Owed

A host-`cargo test`able netd pure-protocol module (the parsers, the
qid codec, `rreaddir_budget`, the ndb subset) — the netdev
`cfg_attr(not(test), no_std)` pattern applied to netd. netd is a
`no_std` + aarch64-SVC BIN crate (libthyla-rs is not host-buildable),
so it structurally cannot `cargo test` without a feature-gated
refactor that was judged a risk to the green device build at net-4d.

## What closes it

The bin→lib feature-gated split (a main-track chunk), moving the pure
parsers/codecs behind a host-testable feature; the in-guest
`proto_selftest` battery then becomes the device-side twin rather than
the only coverage.

## Risk while open

Parser/codec regressions surface only at boot (the in-guest selftests
run every boot and ARE deterministic, so the floor is real — but the
edit-compile-boot loop is the cost, and negative-input coverage grows
only as selftest arms are hand-added).
