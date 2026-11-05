---
id: chg-2026-11-05-input-ring-overrun
type: chg
title: "A release the input ring dropped no longer parks the seat"
date: 2026-11-05
arc: arc-halcyon-interaction
commits: []
touched:
  - sub-lictor
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-05
---
Vote 50's audit round 2, F4, read a blocked serve loop losing keys. Reading on, a lost release does more than lose a key: Lictor's `held` keeps its ACK back and the kernel's own `held` table refuses RESTORED, so one report the device dropped fails every later chord until that key is pressed again. The eventq goes from 16 to 64, what QEMU offers and one page holds. A drain that finds less than a report's worth free releases every held key through the normal path, to the kernel and the compositor. The device gives no other sign of a drop (QEMU's virtio_input_send, read 2026-11-05).

A test-mode lever stops the serve loop on F8 under `thylacine.test-latch-dead`. `ls-graphical-keys-overrun` drops a real release with it, and `ls-graphical-keys-behind` types whole command lines behind a real client's queue. Not booted when written. A thread draining the ring would keep the keystrokes too and is not built; a measurement of `ls-graphical-keys-behind` decides whether one is owed.
