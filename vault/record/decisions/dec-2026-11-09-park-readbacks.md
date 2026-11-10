---
id: dec-2026-11-09-park-readbacks
type: dec
title: "A readback waits in Lictor for its context's earlier chains, never in QEMU"
date: 2026-11-09
status: standing
decided-by: user-vote
affects: [sub-lictor, sub-tapestryd]
created: 2026-11-09
---
## Fork

Vote 72a's first 900 ms judgment on thyla-pi (QEMU 10.0.11, virglrenderer 1.1.0, V3D, KVM) went red, and red identically with vote 72a taken out. A host sampler of QEMU's threads and QEMU's MMIO trace showed why. Within milliseconds of decoding a client's `TRANSFER_FROM_HOST_3D` of a busy BO, QEMU's main thread sat in V3D's wait-BO ioctl for 859-907 ms holding the big QEMU lock, and no vCPU completed an emulated MMIO write until it let go. Lictor's queue notify, which comes before every timed wait, froze with it, so no guest deadline bounds the hold: vote 72a cannot act on this lane. `ioeventfd=on` does not move the GPU notify off the lock on this QEMU.

## Decision

Operator vote 81, "park readbacks, then land": Lictor parks a `TRANSFER_FROM_HOST_3D`, the client's and the compositor's, until the fences submitted before it retire, and queues that context's later submits behind it, so stream order is kept and nothing tears. QEMU's decode then never waits on the GPU for a readback. GPU-DESIGN 4.5.13 changes first, the C-6 DEADLINE arm is redesigned, the egl-headless `RESOURCE_FLUSH` is weighed for the same rule, and an audit round follows. The landing waits for it. Rejected: landing first and parking next; a lane limit alone.

## Consequences

"The fences submitted before it" are the readback's own context's: vrend waits only for the jobs that reference the resource, and C-6 measured that a client-context readback is not ordered behind another context's work. A readback costs its context one pipeline bubble and nothing more; the old quiescence gate stays rejected, because a parked readback is never skipped. The owner's fence id is minted at submission, the wire id at publication. A dead engine releases a parked chain as refused, since the device never saw it. The egl-headless flush waits for every context's queued jobs, so only a device-wide drain before each flush would bound it; that goes back to the operator with a measurement. Uploads into a busy resource and the event-driven synchronous probes are weighed in GPU-DESIGN 4.5.13 and not parked.
