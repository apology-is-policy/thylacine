---
id: chg-2026-07-19-90-death-block-through
type: chg
title: "#90: frame-atomic reader-recv DEATH block-through (spec-first)"
date: 2026-07-19
arc: arc-go-ide
commits: ["7f5d63ab", "8d7688cb", "8524cc1a", "87ede568"]
touched: [sub-kernel-ninep-client]
established: []
closed: [fnd-90-r1-f1]
opened: [seam-90-hung-server]
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
The user-voted death half of the mid-frame unwind (closing
[[seam-90-death-half]]): the #811 die-check becomes frame-atomic for the
elected reader -- a dying reader unwinds only at got==0 and blocks through
mid-frame, reusing the existing stop_no_park/stop_unwinds latches
(`thread_reader_blocks_death` guards all four die-check sites in
sleep()/tsleep()). Spec-first: scripture -> `reader_frame.tla` (clean +
buggy NoDesync counterexample) -> impl -> audit, one commit each. The
residual liveness face against a hung/untrusted server is
[[seam-90-hung-server]]. One Fable round, clean ([[adt-90-r1]]); the P3
(production sleep()-path coverage) fixed in-close, revert-probed. Prose:
the commit messages + ARCH 8.8.1.1.
