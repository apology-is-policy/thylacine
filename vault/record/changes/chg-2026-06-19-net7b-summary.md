---
id: chg-2026-06-19-net7b-summary
type: chg
title: "net-7b: observability — the /net/summary rollup + the native netstat"
date: 2026-06-19
arc: arc-net
commits: ["c6fa8f8c"]
touched: [sub-netd-server]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
The scripture decision realized: the network observability aggregate
lives in netd's /net (where the connection table lives), not a kernel
/ctl/net. `/net/summary` (P_SUMMARY) renders the interface view + the
three per-protocol stats + a one-line-per-live-connection table into a
per-read Vec (a multi-connection table exceeds the fixed Content cap;
h_read offset-slices it; h_getattr reports the true length).
Visibility-not-authority, per-territory by construction. Plus the
native `netstat` coreutil (a thin walk of /net; backfills with the
coreutils sweep). Kernel byte-unchanged.
