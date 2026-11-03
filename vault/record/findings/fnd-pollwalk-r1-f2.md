---
id: fnd-pollwalk-r1-f2
type: fnd
round: adt-pollwalk-r1
severity: P3
status: deferred
title: "The one-hook-per-call test cannot tell which hook the dedupe keeps"
surface: [sub-kernel-poll]
threatens: [inv-i9]
created: 2026-10-28
---
`poll.one_hook_per_list_per_call` polls ONE fd twice. Keeping the LATER hook
would lose a wake for two DIFFERENT objects on one list (a pipe's two ends: a
mutation of the first after its sample, before the second's registration). The
code keeps the earlier hook, as its comment argues; a test that tells the
directions apart needs a hook between two entries' scans: [[seam-poll-dedupe-direction-test]].
