---
id: chg-2026-06-23-gonet3c-net-over-net
type: chg
title: "go-net Stage 3c: Go net over /net + the netd local-on-announce fix"
date: 2026-06-23
arc: arc-go-build
commits: ["68d785ba"]
touched: [sub-netd-server]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
A Go program drives the full plan9-shaped stack over /net (net.Listen →
cs → clone → announce; a blocking accept goroutine; net.Dial; a
verified round-trip on the resident lo). The netd fix this surfaced:
`ctl_announce` never recorded `slots[n].local`, so `/net/tcp/N/local`
was EMPTY for an announced-but-unconnected listener — latent because
the native client never read `local` post-announce, while Go's
`listenPlan9 → readPlan9Addr` reads it immediately (empty file → EOF →
`net.Listen` failed). The fix records the bound listen endpoint on a
successful announce (a wildcard binds 0.0.0.0) — the correct Plan 9
`local` semantics for ALL clients (pouch getsockname-on-a-listener
included). The go-fork half of the commit backfills with the Go-port
sweep; the go-net boot probe IS the regression.
