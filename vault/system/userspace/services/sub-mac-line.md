---
id: sub-mac-line
type: sub
title: "The line to the Mac -- the mac1 framing and the virtio-serial driver behind /srv/mac"
parent: moc-userspace
code: [usr/lib/macline/Cargo.toml, usr/lib/macline/src/lib.rs, usr/virtio-serial/Cargo.toml, usr/virtio-serial/src/lib.rs, usr/virtio-serial/src/console.rs, usr/virtio-serial/src/relay.rs, usr/virtio-serial/src/main.rs, usr/mac-line-probe/Cargo.toml, usr/mac-line-probe/src/main.rs, tools/mac-app-fake.py, tools/interactive/ls-mac-line.exp]
audit: hard
guarded-by: [inv-i34]
validated-by: [prose, gate-interactive]
locks: []
abis: []
design: ["docs/MAC-RUNNER.md"]
created: 2026-11-14
updated: 2026-11-14
---
## Purpose

Carry the Mac app's 9P2000.L tree into the guest (`docs/MAC-RUNNER.md` 4.4, votes M5 and
M10-M13). The virtio console port `org.thylacine.mac` is one byte stream to the app. The
`virtio-serial` driver posts `/srv/mac` and gives every guest connection to it one channel of
that stream, so every connection is its own 9P2000.L session with the app, attached by the
kernel with the opener's principal and at `/srv`'s message size. The driver relays bytes and
parses no 9P. `macline` is the channel framing, `mac1`, which the driver and the app both build
from one source.

## Contract

- **The framing (ABI, `mac1`).** Every frame is `size[4] type[1] chan[4] data`, little-endian,
  `size` counting the whole frame. HELLO (0) carries `version[s] maxframe[4]` and opens a fresh
  line from each side; OPEN (1, the driver's) starts a channel; DATA (2) carries the next bytes
  of a channel's stream; CLOSE (3) says its sender sends no more, and an id is free only once
  both sides sent it; RESET (4, the driver's) says the driver found the line malformed. A
  version other than `mac1` or a `maxframe` under 4096 ends the line.
- **The driver** binds `virtio-pci:3` (warden manifest `virtio-serial`, persistent, a 256 KiB
  DMA pool). It negotiates VERSION_1 and MULTIPORT, sets up the control queues and the queues of
  the first four ports, and takes the first port the device names `org.thylacine.mac`. With no
  such port it posts nothing and says so. It writes READY once the ports have named themselves
  (or 2 s passed) and, when the app is on the line, once the app's HELLO came (or 2 s more).
- **A guest connection** taken while the line is up gets a channel (OPEN); its bytes go as DATA
  no larger than either end allows; its hang-up sends CLOSE. A connection opened while no app is
  on the line, or past 32 connections, is hung up at once, so the opener's attach fails (EIO)
  without waiting for the kernel's 5 s handshake deadline.
- **The app going away** (PORT_OPEN 0) closes every channel: each connection hangs up and its
  mounts read EIO. Bytes the receive queue delivered before the event are dropped. The app's
  return (PORT_OPEN 1) starts a fresh line with the driver's HELLO.

## Mechanism

- `macline` (pure, no_std): `put`/`put_hello` encode, `Parser` reassembles frames from any
  pieces (the type is judged before the body arrives; a size under 9 or over this end's
  `maxframe` is malformed), and `Line` checks each frame against the rules for its end
  (`End::Driver` refuses OPEN and RESET and anything before the app's HELLO; `End::App`
  discards frames until the driver's HELLO). `Chan` is `Open`, `SentClose` or `GotClose`.
- `virtio_serial::console` (pure): the control protocol. DEVICE_ADD of a port inside the queues
  is answered PORT_READY 1, outside them 0. PORT_NAME (a trailing NUL is QEMU's, not part of the
  name) finds the target; the driver answers PORT_OPEN 1 for it. PORT_OPEN from the device
  follows the app: a second open without a close is a new connection whose close was lost.
  DEVICE_REMOVE of the target closes the line. Messages for ports never added are ignored.
- `virtio_serial::relay` (pure): the rules of 4.4 over connections named by their handle. It
  holds a `Line` and a `Parser`, a channel per connection, a bounded queue toward each
  connection (256 KiB; past it the channel closes, so one connection never stalls the others),
  and whole frames toward the app. States: Down, Hello, Up, Reset (input discarded until the app
  goes).
- `main.rs` (the device): one page per queue in the DMA pool, 16 entries; 16 control receive
  buffers, all offered at DRIVER_OK because the device drops a control message it has no buffer
  for; 16 x 4 KiB receive and transmit buffers for the line's port. Each transmit buffer holds
  whole frames only, so a disconnect, which drops whole buffers, never leaves half a frame.
  Every used-ring id and length is checked before it names a buffer.

## Data structures

`Relay { state, line, parser, conns: Key -> {chan, pending}, chans: chan -> Key, out: frames }`;
`Console { ports: [added, named, host], target }`; the driver's `Vq { q, va, notify, avail_idx,
used_idx }` and busy maps for the control and line transmit slots.

## Concurrency

Two threads. The IRQ thread blocks on the device interrupt (a KOBJ_IRQ cannot be polled),
acknowledges it and writes one byte to a nonblocking pipe. The relay thread polls that pipe, the
listener and every connection (at most 34 descriptors), and services the device on every pass.
All of the line's state belongs to the relay thread, so there is no lock. A doorbell the IRQ
thread could not ring costs at most the 1 s poll bound.

## Invariants enforced

- I-34: the driver's authority is the warden's allowance, the function's BDF claim and a 256 KiB
  DMA pool; it creates nothing else.
- The device is not trusted: a used id past the queue, a length past its buffer, a control
  message shorter than its header, or a port id past the queues is dropped and counted.
- The framing's rules (4.4, A fresh line) are `Line`'s and `Relay`'s, host-tested.

## Error paths

A malformed frame from the app sends RESET, hangs up every connection, and discards input until
the app goes away. A failed connection read or write (other than EAGAIN) hangs that connection
up. A port the device removes and adds again is not taken again until the driver binds again:
its rings' indexes live on in the device.

## Performance

Throughput is bounded by 16 x 4 KiB in flight each way and one doorbell per interrupt. Not
measured.

## Prosecution

`macline` 13 host tests, `virtio-serial`'s pure half 22; 47 mutants of the three modules each
turn a test red (aux-gate resid/r6-mut.py, `R6-MUT DONE bad=0`). `tools/mac-app-fake.py`, the
app's end written from 4.4 rather than from `macline`, passes a scripted driver end (aux-gate
resid/r6-fake-check.py, 45 checks). The guest build checks. `ls-mac-line` covers five legs:
- the line up;
- the session's own mount, attached at login as the user `id` names;
- a program's walk of the tree, with ownership checked against `id`;
- the app going away under a held connection and the session's mount;
- a new app.

It passes against a scripted guest that plays the driver's end, with the fake app as the app
(aux-gate `scendry-mac`: 2 runs green, 11 faults red, each at its own check). In a guest under
HVF (run 7, 2026-11-14, 81f009549) it was green twice, all five legs, and each of three control
builds turned it red at its own leg: a driver that sends no HELLO at leg 1, one whose
app-departure hangs no connection up at leg 4, and a newns that does not mount at leg 2. The
Mac's end there is the fake app. Not yet audited (round R5).

## Seams

- **newns** ([[sub-newns]], vote M13): login starts each session's program through it; it opens
  `/srv/mac` as the user, mounts `/mnt/mac` and execs the program in place. login routes `mac` as
  a resident name and makes the mount point ([[sub-stratum-session]]).
- The app's end (`macrunner/app`): the mac1 line and its 9P server. Not yet built.

## Caveats

- Ports are taken only below four; QEMU numbers a virtserialport from 1, and the Mac app's QEMU
  and `run-vm.sh`'s `THYLACINE_MAC_LINE` give it port 1 of two.
- `/srv/mac` is posted whether or not the app is on the line; an open with no app fails at once.

## Provenance

Written 2026-11-14 (aux) with the first build of the driver and the framing, from
`docs/MAC-RUNNER.md` 4.4 and the votes in [[dec-2026-11-13-mac-interop]] and
[[dec-2026-11-14-mac-session-starter]].
