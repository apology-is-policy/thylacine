---
id: fnd-thread-c7-r1-f2
type: fnd
round: adt-thread-c7-r1
severity: P3
status: fixed
title: "Three per-thread refusal arms had no witness"
surface: [sub-kernel-devproc]
threatens: [inv-i39]
fixed-by: chg-2026-10-29-thread-model-c7
regression: "devproc.thread_files_name_their_thread legs f and g; sabotages S12, S13, S14 RED at phase e"
created: 2026-10-29
---
## Prosecution

A departed thread's open `kstack`, its open tid directory's listing, and a write through a thread object reached no test. Leg (f) of devproc.thread_files_name_their_thread now reads both after the thread leaves (-1), each with a positive control while it is present; leg (g) forges a thread-kind ctl from the qids the walk makes and shows `start` refused through it while the real ctl lands it -- the reviewer's literal kregs write is refused at the ctl fallthrough with or without the gate, so it could not discriminate.
