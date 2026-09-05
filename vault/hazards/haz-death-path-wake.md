---
id: haz-death-path-wake
type: haz
title: "Death racing a wait/wake or role-transfer protocol"
applies-to: [global]
instances: [fnd-841-r2-f6]
created: 2026-07-31
updated: 2026-07-31
---
## The failure shape

Proc/Thread death arrives concurrently with a wait/wake protocol step: a
death-wake is lost between cond-check and sleep; a handed-off role (reader,
ownership, drain duty) strands because its target dies before assuming it;
teardown frees state a waker still walks. The #788/#806/#860/#809/#811
death-path lineage's recurring class — historically the most bug-prone in
the tree.

## The tell

- Any hand-off whose target can die before assumption, with no re-hand-off
  on the target's death path.
- A wake predicate not re-checked under the serializing lock
  (register-then-observe missing).
- A teardown path that frees or unlinks state without first winning the same
  lock the waker walks under.

## The countermeasure

Register-then-observe under the per-Thread `wait_lock` (#811, universal
death-interruptible sleep); explicit re-hand-off on every DIED return of a
role carrier (gated on the carrier actually holding the hand-off, e.g.
`be_reader`); death-wins ordering (every `group_exit_msg` check precedes the
protocol's own state checks); prosecute the death arm of every new wake
protocol as hard as the happy path.
