---
id: chg-2026-11-09-input-thread
type: chg
title: "Lictor reads input on its own thread, into a 4096-entry queue (vote 72, second half)"
date: 2026-11-09
arc: arc-halcyon-interaction
commits: []
touched:
  - sub-lictor
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-09
---
Operator vote 72's second half ([[dec-2026-11-05-narrow-wait-and-key-thread]]). The first half ([[chg-2026-11-09-narrow-sync-wait]]) bounded a synchronous wait at `SUBMIT_DEADLINE_MS`, but the serve loop still drained the eventqs, so about 600 ms behind a stall was enough to fill a 64-report ring at 30 ms a character. `backend/intake.rs` now owns the virtio-input devices from the end of probe: a `spawn_raw` thread on a 64 KiB burrow stack drains every eventq every 4 ms into one `VecDeque` of 4096 entries behind `libthyla_rs::sync::Mutex`. `Seat::input` swaps the whole queue out each pass and keeps every seat decision on the serve thread.

The thread never prints and never calls the seat endpoint: a console write parks in EXCLUSIVE, and the thread draining the rings must not park in the episode. `InputDev::drain` therefore reports through `input::Report`, and the ring's own notes (the test-mode LOW line that `ls-graphical-keys-behind` reads, a malformed used element) travel in the queue for the serve thread to print. An eventq overrun travels as `Entry::Overran` at its place in the order. An event or overrun that finds the queue full is dropped and marks its device; the mark is set only while the queue is full and is taken under the same lock as the entries, so the loss follows everything taken with it, and `Seat::release_held` releases that device's held keys there (`lictor: input queue overran`). `Input` keeps the InputInfo answer read at probe instead of the device.

The test lever moves with it: under the armed latch, F8 stops the input thread at the pass that read it (`intake::test`, STOPPED set before that batch is queued), and `Device::test_blind_loop` resumes it after the serve loop's five seconds, so `ls-graphical-keys-overrun` still overruns the ring. Written and recorded only: not yet compiled, booted, or audited. The witness owed is `ls-graphical-keys-behind` going green at 30 ms a character on the Pi, with `ls-graphical-keys-overrun` still green.
